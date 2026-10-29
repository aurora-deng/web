#include "client/companion/SChannelStream/SChannelStream.h"
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
    const bool measure=argc==6&&std::string(argv[5])=="--measure";if(argc==6&&!measure)throw std::runtime_error("unknown measurement option");
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
