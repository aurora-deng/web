#ifndef HTTP_H
#define HTTP_H
#include <string>
#include<map>
#include<iostream>
#include<sstream>
#include<algorithm>


struct HttpRequest{
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string,std::string> headers;
};

HttpRequest parse_request(const std::string& data);
#endif