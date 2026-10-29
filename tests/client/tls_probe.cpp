#include "client/companion/SChannelStream/SChannelStream.h"
#include "client/companion/HttpConnectionPool/HttpConnectionPool.h"
#include <boost/beast/http.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <vector>
int main(int argc,char**argv){try{
    if(argc!=4&&argc!=5&&argc!=6)throw std::runtime_error("usage: tls_probe host port confirmed-sha256 [127.0.0.1-for-SSH] [--measure]");
    boost::asio::io_context io;nebula::client::ServerTrust trust{argv[1],static_cast<unsigned short>(std::stoul(argv[2])),argv[3]};
    if(argc>=5)trust.connectHost=argv[4];
    const bool reuse=argc==6&&std::string(argv[5])=="--measure-reuse";
    const bool measure=argc==6&&std::string(argv[5])=="--measure";if(argc==6&&!measure&&!reuse)throw std::runtime_error("unknown measurement option");
    if(reuse){
        // Match the proxy's upstream connection policy; exclude the warmup handshake.
        nebula::client::HttpConnectionPool pool(trust);std::vector<double>values;
        auto call=[&]{boost::beast::http::request<boost::beast::http::string_body>request{boost::beast::http::verb::get,"/health/live",11};request.set(boost::beast::http::field::host,trust.host);return pool.request(std::move(request),0);};
        if(call().result_int()!=200)throw std::runtime_error("pooled warmup failed");
        for(int i=0;i<30;++i){const auto start=std::chrono::steady_clock::now();if(call().result_int()!=200)throw std::runtime_error("pooled health failed");values.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}
        const auto stats=pool.statistics();if(stats.connected!=1||stats.reused<30||stats.failures)throw std::runtime_error("pooled measurement did not reuse one verified connection");
        std::sort(values.begin(),values.end());std::cout<<"{\"samples\":30,\"connectionPolicy\":\"verified pooled TLS, warmup excluded\",\"p50Ms\":"<<(values[14]+values[15])/2<<",\"p95Ms\":"<<values[28]<<"}\n";return 0;
    }
    std::vector<double>milliseconds;
    for(unsigned run=0;run<(measure?30u:1u);++run){const auto start=std::chrono::steady_clock::now();
    nebula::client::SChannelStream stream(io,trust);stream.connect();
    namespace http=boost::beast::http;
    http::request<http::empty_body>request{http::verb::get,"/health/live",11};request.set(http::field::host,trust.host);request.keep_alive(false);
    http::write(stream,request);boost::beast::flat_buffer buffer;http::response<http::string_body>response;boost::system::error_code error;http::read(stream,buffer,response,error);if(error)throw std::runtime_error(error.message()+" TLS state: "+stream.lastFailure());
    if(response.result_int()!=200)throw std::runtime_error("health request failed");milliseconds.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}
    if(measure){std::sort(milliseconds.begin(),milliseconds.end());std::cout<<"{\"samples\":30,\"p50Ms\":"<<milliseconds[15]<<",\"p95Ms\":"<<milliseconds[28]<<"}\n";}
    else std::cout<<"PASS verified Windows native TLS; health 200\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
