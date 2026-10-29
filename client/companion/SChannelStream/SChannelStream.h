#pragma once
#include <boost/asio.hpp>
#include <boost/beast/core/role.hpp>
#include <chrono>
#include <memory>
#include <functional>
#include <string>
namespace nebula::client {
class BoundedPool;
struct ServerTrust {
    std::string host;unsigned short port{443};
    // 用户确认的学习证书 SHA-256；只用于本进程内的独占根证书存储。
    std::string certificateSha256;
    std::chrono::milliseconds readTimeout{30000};
    // 可选SSH转发的本地连接地址；TLS名称、固定Origin和证书校验仍使用上面的host。
    std::string connectHost;
};
/** Windows 原生 TLS 流。证书验证完成之前，应用层 write_some 一律拒绝。
 * 自签名证书使用进程内根存储，仍检查域名/地址、有效期与服务端用途。
 * 不导入系统/用户根存储，不依赖 Windows OpenSSL，不自实现密码算法。
 */
class SChannelStream {
public:
    using executor_type=boost::asio::any_io_executor;
    SChannelStream(boost::asio::io_context&io,ServerTrust trust);
    ~SChannelStream();
    SChannelStream(const SChannelStream&)=delete;SChannelStream&operator=(const SChannelStream&)=delete;
    executor_type get_executor()noexcept;
    void connect();
    void close()noexcept;
    std::string lastFailure()const;
    boost::asio::ip::tcp::socket&socket()noexcept;
    void setAsyncPools(BoundedPool&reads,BoundedPool&writes);
    template<class Buffers,class CompletionToken>auto async_read_some(const Buffers&buffers,CompletionToken&&token){
        return boost::asio::async_initiate<CompletionToken,void(boost::system::error_code,std::size_t)>(
            [this,buffers](auto handler){
                auto completion=std::make_shared<std::decay_t<decltype(handler)>>(std::move(handler));
                const auto executor=boost::asio::get_associated_executor(*completion,get_executor());
                auto done=[completion,executor](boost::system::error_code error,std::size_t n){boost::asio::post(executor,[completion,error,n]()mutable{(*completion)(error,n);});};
                if(!schedule(false,[this,buffers,done]{boost::system::error_code error;const auto n=read_some(buffers,error);done(error,n);}))done(make_error_code(boost::system::errc::no_buffer_space),0);
            },token);
    }
    template<class Buffers,class CompletionToken>auto async_write_some(const Buffers&buffers,CompletionToken&&token){
        return boost::asio::async_initiate<CompletionToken,void(boost::system::error_code,std::size_t)>(
            [this,buffers](auto handler){
                auto completion=std::make_shared<std::decay_t<decltype(handler)>>(std::move(handler));
                const auto executor=boost::asio::get_associated_executor(*completion,get_executor());
                auto done=[completion,executor](boost::system::error_code error,std::size_t n){boost::asio::post(executor,[completion,error,n]()mutable{(*completion)(error,n);});};
                if(!schedule(true,[this,buffers,done]{boost::system::error_code error;const auto n=write_some(buffers,error);done(error,n);}))done(make_error_code(boost::system::errc::no_buffer_space),0);
            },token);
    }
    template<class MutableBuffers>std::size_t read_some(const MutableBuffers&buffers,boost::system::error_code&ec){
        for(auto i=boost::asio::buffer_sequence_begin(buffers);i!=boost::asio::buffer_sequence_end(buffers);++i){auto b=boost::asio::mutable_buffer(*i);if(b.size())return read(b.data(),b.size(),ec);}ec.clear();return 0;
    }
    template<class MutableBuffers>std::size_t read_some(const MutableBuffers&buffers){boost::system::error_code ec;auto n=read_some(buffers,ec);if(ec)throw boost::system::system_error(ec);return n;}
    template<class ConstBuffers>std::size_t write_some(const ConstBuffers&buffers,boost::system::error_code&ec){
        for(auto i=boost::asio::buffer_sequence_begin(buffers);i!=boost::asio::buffer_sequence_end(buffers);++i){auto b=boost::asio::const_buffer(*i);if(b.size())return write(b.data(),b.size(),ec);}ec.clear();return 0;
    }
    template<class ConstBuffers>std::size_t write_some(const ConstBuffers&buffers){boost::system::error_code ec;auto n=write_some(buffers,ec);if(ec)throw boost::system::system_error(ec);return n;}
private:
    std::size_t read(void*data,std::size_t length,boost::system::error_code&ec);
    std::size_t write(const void*data,std::size_t length,boost::system::error_code&ec);
    bool schedule(bool write,std::function<void()>operation);
    struct Impl;std::unique_ptr<Impl>impl_;
};
void teardown(boost::beast::role_type role,SChannelStream&stream,boost::system::error_code&ec);
inline void beast_close_socket(SChannelStream&stream){stream.close();}
template<class Handler>void async_teardown(boost::beast::role_type,SChannelStream&stream,Handler&&handler){
    stream.close();const auto executor=boost::asio::get_associated_executor(handler,stream.get_executor());
    boost::asio::post(executor,[completion=std::forward<Handler>(handler)]()mutable{completion(boost::system::error_code{});});
}
}
