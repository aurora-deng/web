#include "RuntimeConfig.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
namespace webserver::config {
    namespace {
        std::size_t integer(std::string value,const char*name,std::size_t fallback,std::size_t low,std::size_t high){
            if(value.empty())return fallback;
            std::size_t result{
            };
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
            if(parsed.ec!=std::errc{
            }
            ||parsed.ptr!=value.data()+value.size()||result<low||result>high)
            throw std::invalid_argument(std::string(name)+" is outside the allowed range");
            return result;
        }
        bool boolean(std::string value,const char*name){
            std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){
                return std::tolower(c);
            }
            );
            if(value.empty()||value=="0"||value=="false"||value=="no"||value=="off")return false;
            if(value=="1"||value=="true"||value=="yes"||value=="on")return true;
            throw std::invalid_argument(std::string(name)+" must be a boolean");
        }
    }
    std::string RuntimeConfig::environment(const char*name){
        const char*value=std::getenv(name);
        return value?value:"";
    }
    bool RuntimeConfig::flag(const char*name){
        return boolean(environment(name),name);
    }
    std::size_t RuntimeConfig::size(const char*name,std::size_t fallback,std::size_t low,std::size_t high){
        return integer(environment(name),name,fallback,low,high);
    }
    std::vector<std::string> RuntimeConfig::list(std::string value){
        // Own the string while views/indices are in use; never bind a view to a temporary getenv copy.
        std::vector<std::string> result;
        std::size_t first=0;
        while(first<=value.size()){
            auto end=value.find(',',first);
            if(end==std::string::npos)end=value.size();
            auto item=value.substr(first,end-first);
            auto start=item.find_first_not_of(" \t\r\n");
            if(start!=std::string::npos)result.push_back(item.substr(start,item.find_last_not_of(" \t\r\n")-start+1));
            if(end==value.size())break;
            first=end+1;
        }
        return result;
    }
    RuntimeConfig RuntimeConfig::load(Lookup get){
        RuntimeConfig c;
        c.production=boolean(get("WEB_PRODUCTION_MODE"),"WEB_PRODUCTION_MODE");
        c.port=integer(get("WEB_SERVER_PORT"),"WEB_SERVER_PORT",8080,1,65535);
        c.tlsPort=integer(get("WEB_TLS_PORT"),"WEB_TLS_PORT",8443,1,65535);
        c.tlsCert=get("WEB_TLS_CERT");
        c.tlsKey=get("WEB_TLS_KEY");
        c.authSecret=get("WEB_AUTH_SECRET");
        c.allowedOrigins=list(get("WEB_ALLOWED_ORIGINS"));
        c.database=get("WEB_PHASE11_DATABASE");
        c.tokenKeyFile=get("WEB_PHASE11_TOKEN_KEY_FILE");
        if(c.tlsCert.empty()!=c.tlsKey.empty())throw std::invalid_argument("WEB_TLS_CERT and WEB_TLS_KEY must be configured together");
        if(c.database.empty()!=c.tokenKeyFile.empty())throw std::invalid_argument("WEB_PHASE11_DATABASE and WEB_PHASE11_TOKEN_KEY_FILE must be set together");
        if(boolean(get("WEB_FUNCTION3_ENABLE"),"WEB_FUNCTION3_ENABLE")&&c.database.empty())throw std::invalid_argument("WEB_FUNCTION3_ENABLE requires WEB_PHASE11_DATABASE");
        if(c.production){
            if(c.authSecret.size()<32)throw std::invalid_argument("WEB_PRODUCTION_MODE requires WEB_AUTH_SECRET with at least 32 bytes");
            if(c.allowedOrigins.empty()||std::find(c.allowedOrigins.begin(),c.allowedOrigins.end(),"*")!=c.allowedOrigins.end())throw std::invalid_argument("Production requires explicit WEB_ALLOWED_ORIGINS");
            if(c.tlsCert.empty())throw std::invalid_argument("WEB_PRODUCTION_MODE requires WEB_TLS_CERT and WEB_TLS_KEY");
            if(c.database.empty())throw std::invalid_argument("Production requires persistent WEB_PHASE11_DATABASE");
        }
        return c;
    }
}
