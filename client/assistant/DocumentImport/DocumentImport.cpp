#include "DocumentImport.h"
#include <windows.h>
#include <fstream>
#include <stdexcept>
#include <boost/json.hpp>
namespace nebula::client {
namespace{
std::wstring quote(const std::filesystem::path&path){
    std::wstring result=L"\"";std::size_t slashes=0;for(wchar_t c:path.native()){if(c==L'\\'){++slashes;continue;}if(c==L'\"')result.append(slashes*2+1,L'\\');else result.append(slashes,L'\\');slashes=0;result+=c;}result.append(slashes*2,L'\\');return result+L'\"';
}
}
std::string DocumentImport::extract(const std::filesystem::path&file)const{
    if(!python_.is_absolute()||!script_.is_absolute()||!std::filesystem::exists(python_)||!std::filesystem::exists(script_))throw std::runtime_error("请先配置已有Python和文档解析脚本，不会自动安装解析器");
    const auto output=std::filesystem::path(file.wstring()+L".parsed.json");
    struct Clean{std::filesystem::path file;~Clean(){std::error_code ignored;std::filesystem::remove(file,ignored);}}clean{output};
    std::wstring command=quote(python_)+L" -I "+quote(script_)+L" "+quote(file)+L" "+quote(output);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    HANDLE job=CreateJobObjectW(nullptr,nullptr);if(!job)throw std::runtime_error("无法建立文档任务限制");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.ProcessMemoryLimit=512*1024*1024;limits.BasicLimitInformation.ActiveProcessLimit=1;
    if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){CloseHandle(job);throw std::runtime_error("无法限制文档任务资源");}
    if(!CreateProcessW(python_.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&process)){CloseHandle(job);throw std::runtime_error("无法启动本机文档解析");}
    if(!AssignProcessToJobObject(job,process.hProcess)){TerminateProcess(process.hProcess,1);CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseHandle(job);throw std::runtime_error("文档任务无法进入受限进程组");}
    ResumeThread(process.hThread);CloseHandle(process.hThread);const auto result=WaitForSingleObject(process.hProcess,10000);DWORD code=1;
    if(result==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&code);else TerminateJobObject(job,1);
    CloseHandle(process.hProcess);CloseHandle(job);
    if(result!=WAIT_OBJECT_0)throw std::runtime_error("文档解析超过10秒限制，已停止");
    if(!std::filesystem::exists(output)||std::filesystem::file_size(output)>1024*1024)throw std::runtime_error("文档解析失败或结果过大");
    std::ifstream input(output,std::ios::binary);const auto value=boost::json::parse(std::string(std::istreambuf_iterator<char>(input),{}));
    if(!value.is_object())throw std::runtime_error("无效的文档解析结果");const auto&obj=value.as_object();
    if(code!=0){const auto*error=obj.if_contains("error");throw std::runtime_error(error&&error->is_string()?std::string(error->as_string()):"文档解析失败");}
    const auto*text=obj.if_contains("text");if(!text||!text->is_string()||text->as_string().empty()||text->as_string().size()>200*1024)throw std::runtime_error("提取文本必须为1..200 KiB，扫描PDF需要另行OCR");return std::string(text->as_string());
}
}
