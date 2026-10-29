// 与 ProxyServer 的连接登记、预算共用同一生命周期；不是另一个监听服务器。
class StreamBridge:public Closeable,public std::enable_shared_from_this<StreamBridge>{
    std::weak_ptr<ProxyServer::State>state_;beast::tcp_stream browser_;SChannelStream remote_;
    ProxyRequest request_;BoundServerSession identity_;std::shared_ptr<Ticket>connection_,permit_;
    beast::flat_buffer buffer_{32768};http::response_parser<http::buffer_body>parser_;
    http::response<http::empty_body>header_;std::optional<http::response_serializer<http::empty_body>>serializer_;
    std::array<char,16384>chunk_{};bool closed_{},headerSent_{};
public:
    StreamBridge(const std::shared_ptr<ProxyServer::State>&s,beast::tcp_stream socket,ProxyRequest request,std::shared_ptr<Ticket>connection,std::shared_ptr<Ticket>permit)
      :state_(s),browser_(std::move(socket)),remote_(s->io,s->upstream.trust()),request_(std::move(request)),identity_(s->upstream.session()),connection_(std::move(connection)),permit_(std::move(permit)){
        request_=s->upstream.prepareBound(std::move(request_),identity_);
        remote_.setAsyncPools(s->reads,s->writes);parser_.header_limit(32768);parser_.body_limit(boost::none);
    }
    bool stale(std::uint64_t epoch)const override{return epoch!=identity_.epoch;}
    void close()override{if(closed_)return;closed_=true;remote_.close();boost::system::error_code ignored;browser_.socket().close(ignored);}
    void start(){auto s=state_.lock();if(!s)return;auto self=shared_from_this();
        if(!s->work.submit([self,weak=state_]{boost::system::error_code error;try{self->remote_.connect();}catch(...){error=make_error_code(boost::system::errc::permission_denied);}if(auto s=weak.lock())net::post(s->io,[self,error]{if(error)return self->fail();self->send();});}))fail();
    }
    void fail(){if(headerSent_)return close();auto message=std::make_shared<ProxyResponse>(localJson(502,{{"message","流式连接失败或证书校验未通过"}}));auto self=shared_from_this();browser_.expires_after(std::chrono::seconds(10));http::async_write(browser_,*message,[self,message](boost::system::error_code,std::size_t){self->close();});}
    void send(){if(closed_)return;auto self=shared_from_this();http::async_write(remote_,request_,[self](boost::system::error_code error,std::size_t){if(error)return self->fail();http::async_read_header(self->remote_,self->buffer_,self->parser_,[self](boost::system::error_code error,std::size_t){if(error)return self->fail();self->headers();});});}
    void headers(){if(closed_)return;const auto&up=parser_.get();header_.version(11);header_.result(up.result());
        for(const auto&field:up.base()){const auto name=field.name_string();if(field.name()==http::field::content_length||field.name()==http::field::transfer_encoding||field.name()==http::field::connection||field.name()==http::field::set_cookie||name.starts_with("Access-Control-"))continue;header_.set(field.name_string(),field.value());}
        header_.set(http::field::cache_control,"no-store");header_.set("X-Content-Type-Options","nosniff");header_.keep_alive(false);header_.chunked(true);serializer_.emplace(header_);headerSent_=true;
        browser_.expires_after(std::chrono::seconds(30));auto self=shared_from_this();http::async_write_header(browser_,*serializer_,[self](boost::system::error_code error,std::size_t){if(error)return self->close();self->read();});
    }
    void read(){if(closed_)return;if(parser_.is_done())return finish();parser_.get().body().data=chunk_.data();parser_.get().body().size=chunk_.size();auto self=shared_from_this();
        http::async_read_some(remote_,buffer_,parser_,[self](boost::system::error_code error,std::size_t){
            if(error==http::error::need_buffer)error.clear();if(error)return self->close();const auto size=self->chunk_.size()-self->parser_.get().body().size;
            if(!size)return self->read();self->browser_.expires_after(std::chrono::seconds(30));
            // 上一个浏览器写完成才读下一个上游块：每条流最多持有16KiB正文。
            auto piece=std::make_shared<http::chunk_body<net::mutable_buffer>>(http::make_chunk(net::buffer(self->chunk_.data(),size)));
            net::async_write(self->browser_,*piece,[self,piece](boost::system::error_code error,std::size_t){if(error)return self->close();self->read();});
        });
    }
    void finish(){auto last=std::make_shared<http::chunk_last<http::chunk_crlf>>(http::make_chunk_last());auto self=shared_from_this();net::async_write(browser_,*last,[self,last](boost::system::error_code,std::size_t){self->close();});}
};
class WebSocketBridge:public Closeable,public std::enable_shared_from_this<WebSocketBridge>{
    std::weak_ptr<ProxyServer::State>state_;beast::websocket::stream<beast::tcp_stream>browser_;
    beast::websocket::stream<SChannelStream>remote_;ProxyRequest request_;BoundServerSession identity_;
    std::shared_ptr<Ticket>connection_,permit_;beast::flat_buffer inbound_{131072},outbound_{131072};bool closed_{};
public:
    WebSocketBridge(const std::shared_ptr<ProxyServer::State>&s,beast::tcp_stream socket,ProxyRequest request,std::shared_ptr<Ticket>connection,std::shared_ptr<Ticket>permit)
      :state_(s),browser_(std::move(socket)),remote_(s->io,s->upstream.trust()),request_(std::move(request)),identity_(s->upstream.session()),connection_(std::move(connection)),permit_(std::move(permit)){
        remote_.next_layer().setAsyncPools(s->reads,s->writes);browser_.read_message_max(131072);remote_.read_message_max(131072);
        auto timeout=beast::websocket::stream_base::timeout::suggested(beast::role_type::client);timeout.idle_timeout=std::chrono::seconds(40);timeout.keep_alive_pings=true;remote_.set_option(timeout);
        browser_.set_option(beast::websocket::stream_base::timeout::suggested(beast::role_type::server));
        remote_.set_option(beast::websocket::stream_base::decorator([cookie=identity_.cookie,origin=s->upstream.origin()](beast::websocket::request_type&r){r.set(http::field::cookie,cookie);r.set(http::field::origin,origin);}));
    }
    bool stale(std::uint64_t epoch)const override{return epoch!=identity_.epoch;}
    void close()override{if(closed_)return;closed_=true;remote_.next_layer().close();boost::system::error_code ignored;browser_.next_layer().socket().close(ignored);}
    void start(){auto s=state_.lock();if(!s)return;auto self=shared_from_this();
        if(!s->work.submit([self,weak=state_]{boost::system::error_code error;try{self->remote_.next_layer().connect();}catch(...){error=make_error_code(boost::system::errc::permission_denied);}if(auto s=weak.lock())net::post(s->io,[self,error]{if(error)return self->close();self->handshake();});}))close();
    }
    void handshake(){if(closed_)return;auto s=state_.lock();if(!s)return close();auto self=shared_from_this();
        remote_.async_handshake(s->upstream.trust().host+":"+std::to_string(s->upstream.trust().port),std::string(request_.target()),[self](boost::system::error_code error){if(error)return self->close();self->browser_.next_layer().expires_never();self->browser_.async_accept(self->request_,[self](boost::system::error_code error){if(error)return self->close();self->fromBrowser();self->fromServer();});});
    }
    void fromBrowser(){if(closed_)return;auto self=shared_from_this();browser_.async_read(outbound_,[self](boost::system::error_code error,std::size_t){if(error)return self->close();self->remote_.text(self->browser_.got_text());self->remote_.async_write(self->outbound_.data(),[self](boost::system::error_code error,std::size_t){if(error)return self->close();self->outbound_.consume(self->outbound_.size());self->fromBrowser();});});}
    void fromServer(){if(closed_)return;auto self=shared_from_this();remote_.async_read(inbound_,[self](boost::system::error_code error,std::size_t){if(error)return self->close();self->browser_.text(self->remote_.got_text());self->browser_.async_write(self->inbound_.data(),[self](boost::system::error_code error,std::size_t){if(error)return self->close();self->inbound_.consume(self->inbound_.size());self->fromServer();});});}
};
void startStream(const std::shared_ptr<ProxyServer::State>&s,beast::tcp_stream socket,ProxyRequest request,std::shared_ptr<Ticket>connection){
    const auto permit=s->permit(s->streams,1,s->config.streams);if(!permit){boost::system::error_code ignored;socket.socket().close(ignored);return;}
    auto bridge=std::make_shared<StreamBridge>(s,std::move(socket),std::move(request),std::move(connection),permit);s->track(bridge);bridge->start();
}
void startWebSocket(const std::shared_ptr<ProxyServer::State>&s,beast::tcp_stream socket,ProxyRequest request,std::shared_ptr<Ticket>connection){
    const auto permit=s->permit(s->streams,1,s->config.streams);if(!permit||s->upstream.currentAccount().empty()){boost::system::error_code ignored;socket.socket().close(ignored);return;}
    auto bridge=std::make_shared<WebSocketBridge>(s,std::move(socket),std::move(request),std::move(connection),permit);s->track(bridge);bridge->start();
}
