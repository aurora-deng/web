#include "backend/application/config/RuntimeConfig/RuntimeConfig.h"
#include <map>
#include <stdexcept>
#include <iostream>
using webserver::config::RuntimeConfig;
int main(){
    std::map<std::string,std::string> values;auto load=[&]{return RuntimeConfig::load([&](const char*k){return values[k];});};
    auto reject=[&]{try{load();}catch(const std::invalid_argument&){return;}throw std::runtime_error("invalid configuration accepted");};
    if(load().port!=8080)throw std::runtime_error("default port");
    for(const auto&v:{"-1","65536","8080junk"," 80","1.0"}){values["WEB_SERVER_PORT"]=v;reject();}values.clear();
    values["WEB_FUNCTION3_ENABLE"]="1";reject();values.clear();values["WEB_TLS_KEY"]="private-key-path";reject();values.clear();
    values["WEB_PRODUCTION_MODE"]="1";reject();values["WEB_AUTH_SECRET"]=std::string(32,'x');values["WEB_ALLOWED_ORIGINS"]="*";reject();
    values["WEB_ALLOWED_ORIGINS"]=" https://example.invalid , https://second.invalid ";values["WEB_TLS_CERT"]="certificate";values["WEB_TLS_KEY"]="key";reject();
    values["WEB_PHASE11_DATABASE"]="database";values["WEB_PHASE11_TOKEN_KEY_FILE"]="key-file";if(load().allowedOrigins.size()!=2)throw std::runtime_error("list parsing");
    values["WEB_PRODUCTION_MODE"]="truthy";reject();std::cout<<"PASS strict runtime configuration and production cross-field validation\n";
}
