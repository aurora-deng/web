#pragma once
#include <filesystem>
#include <string>
namespace nebula::client {
/** 只调用明确配置的本地解析器；不启动Shell，不下载模型，不执行文档宏。 */
class DocumentImport final{
public:
    DocumentImport(std::filesystem::path python,std::filesystem::path script):python_(std::move(python)),script_(std::move(script)){}
    std::string extract(const std::filesystem::path&file)const;
private:std::filesystem::path python_,script_;
};
}
