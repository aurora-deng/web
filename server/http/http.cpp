
#include"http.h"


HttpRequest parse_request(const std::string& data)
{
    HttpRequest req;

    std::istringstream stream(data);
    std::string line;

    // 解析第一行
    std::getline(stream,line);
    std::istringstream line_stream(line);

    line_stream>>req.method>>req.path>>req.version;

    // 解析header
    while(std::getline(stream,line)&&line!="\r")
    {
        auto pos =line.find(":");
        if(pos!=std::string::npos)
        {
            std::string key=line.substr(0,pos);
            std::string value=line.substr(pos+2);           //跳过“：”
            value.pop_back();       //去掉\r
            req.headers[key]=value;
        }
    }
    return req;
}

