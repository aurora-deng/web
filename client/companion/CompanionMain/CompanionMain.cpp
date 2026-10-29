#include "client/companion/ProxyServer/ProxyServer.h"
#include "client/assistant/LocalAssistantService/LocalAssistantService.h"
#include "client/storage/CredentialVault/CredentialVault.h"
#include <windows.h>
#include <iostream>
#include <map>
#include <thread>
#include <fstream>
#include "client/storage/WindowsProtection/WindowsProtection.h"
using namespace nebula::client;
namespace{ProxyServer*running{};BOOL WINAPI interrupt(DWORD event){if(event==CTRL_C_EVENT||event==CTRL_BREAK_EVENT||event==CTRL_CLOSE_EVENT){if(running)running->stop();return TRUE;}return FALSE;}}
int wmain(int argc,wchar_t**argv){SetConsoleOutputCP(CP_UTF8);try{
    std::map<std::wstring,std::wstring>args;for(int i=1;i<argc;i+=2){if(i+1==argc)throw std::runtime_error("每个选项需要一个值");args[argv[i]]=argv[i+1];}
    for(const auto&[key,value]:args)if(key!=L"--server"&&key!=L"--tls-port"&&key!=L"--port"&&key!=L"--certificate-sha256"&&key!=L"--frontend"&&key!=L"--data-dir"&&key!=L"--python"&&key!=L"--parser"&&key!=L"--connect-host"&&key!=L"--stop-file")throw std::runtime_error("未知客户端选项");
    auto ascii=[&](const wchar_t*key,std::string fallback={}){const auto found=args.find(key);if(found==args.end())return fallback;std::string result;for(const auto c:found->second){if(c<32||c>126)throw std::runtime_error("网络配置需要ASCII值");result+=static_cast<char>(c);}return result;};
    if(!args.contains(L"--data-dir")||!args.contains(L"--frontend"))throw std::runtime_error("请指定 --data-dir 私人资料绝对目录 与 --frontend 前端目录；不会自动选择系统安装位置");
    auto port=[&](const wchar_t*key,unsigned fallback){const auto value=std::stoul(ascii(key,std::to_string(fallback)));if(value<1024||value>65535)throw std::runtime_error("端口必须为1024..65535");return static_cast<unsigned short>(value);};
    ServerTrust trust{ascii(L"--server","192.168.239.135"),port(L"--tls-port",18443),ascii(L"--certificate-sha256")};trust.connectHost=ascii(L"--connect-host");
    if(!trust.connectHost.empty()&&trust.connectHost!="127.0.0.1")throw std::runtime_error("SSH转发连接地址仅接受127.0.0.1");
    if(!trust.certificateSha256.empty()&&(trust.certificateSha256.size()!=64||trust.certificateSha256.find_first_not_of("0123456789abcdef")!=std::string::npos))throw std::runtime_error("证书指纹需要64位小写SHA-256，首次使用前须自行确认");
    const auto root=std::filesystem::path(args.at(L"--data-dir"));if(!root.is_absolute())throw std::runtime_error("私人资料位置必须是绝对路径");
    const auto marker=root/L"NEBULA_PRIVATE_ROOT";constexpr const char*format="nebula-private-root-v1\n";
    if(root.lexically_normal()==root.root_path())throw std::runtime_error("请为私人资料选择专用子目录，不能使用磁盘根目录");
    if(std::filesystem::exists(root)&&!std::filesystem::is_empty(root)){
        if(!std::filesystem::is_regular_file(marker)||std::filesystem::file_size(marker)>64)throw std::runtime_error("该目录已有其他文件且不是本客户端的私人目录；请选择新的专用目录");
        std::ifstream input(marker,std::ios::binary);if(std::string(std::istreambuf_iterator<char>(input),{})!=format)throw std::runtime_error("私人目录标记不匹配，未修改目录权限");
    }
    createPrivateDirectory(root);if(!std::filesystem::exists(marker)){std::ofstream out(marker,std::ios::binary);out<<format;if(!out)throw std::runtime_error("无法建立私人目录标记");}
    UpstreamTransport upstream(trust);CredentialVault vault(root/L"credentials",upstream.origin());
    try{const auto cookie=vault.load();if(!cookie.empty()){upstream.restoreCookie(cookie);ProxyRequest me{client_http::verb::get,"/api/me",11};upstream.request(std::move(me));}}catch(...){upstream.clearCookie();std::cout<<"原会话未恢复，请在网页重新登录。\n";}
    upstream.setCredentialSink([&](const auto&session){vault.save(session);});
    DocumentImport importer(args.contains(L"--python")?std::filesystem::path(args.at(L"--python")):std::filesystem::path{},args.contains(L"--parser")?std::filesystem::path(args.at(L"--parser")):std::filesystem::path{});
    LocalAssistantService assistant(root/L"accounts",upstream,std::move(importer));
    ProxyServer proxy({port(L"--port",18091),std::filesystem::path(args.at(L"--frontend"))},upstream,[&](const auto&request,const auto&identity){return assistant.handle(request,identity);});
    running=&proxy;SetConsoleCtrlHandler(interrupt,TRUE);
    // 引导凭据只写入启动终端，浏览器读取后从地址栏移除；不写日志文件。
    std::cout<<"Nebula 本机入口（仅监听127.0.0.1和::1）：\n"<<proxy.launchUrl()<<std::endl;
    std::jthread stopWatcher;const auto stopFile=args.find(L"--stop-file");if(stopFile!=args.end()){const std::filesystem::path file(stopFile->second);stopWatcher=std::jthread([&proxy,file](std::stop_token stop){while(!stop.stop_requested()){if(std::filesystem::exists(file)){proxy.stop();return;}std::this_thread::sleep_for(std::chrono::milliseconds(200));}});}
    proxy.run();stopWatcher.request_stop();assistant.stop();running=nullptr;SetConsoleCtrlHandler(interrupt,FALSE);std::cout<<"本机入口已安全退出。\n";return 0;
}catch(const std::exception&e){std::cerr<<"客户端启动失败："<<e.what()<<'\n';return 1;}}
