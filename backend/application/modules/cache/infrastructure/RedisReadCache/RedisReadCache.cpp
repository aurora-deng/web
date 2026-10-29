#include "RedisReadCache.h"
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
namespace webserver::function3 {
namespace {
using Clock=std::chrono::steady_clock;
std::int64_t now(){return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();}
constexpr std::size_t maxValue=128*1024;
void check(bool value){if(!value)throw std::runtime_error("Redis cache unavailable");}
}
struct RedisReadCache::Impl {
    struct Connection {
        std::mutex mutex;int fd{-1};
        // 分块接收 RESP，避免为响应头的每个字符执行一次 recv。
        std::array<char,4096> buffer{};std::size_t begin{},end{};
        ~Connection(){reset();}
        void reset(){if(fd>=0)::close(fd);fd=-1;begin=end=0;}
    };
    RedisCacheConfig config;std::array<Connection,4> connections;
    std::atomic<std::uint64_t> hits{},misses{},errors{},bypassed{},cursor{};
    std::atomic<std::int64_t> retryAfter{};
    explicit Impl(RedisCacheConfig c):config(std::move(c)){
        in_addr address{};check(inet_pton(AF_INET,config.address.c_str(),&address)==1);
        check(config.port>0&&config.port<65536&&config.timeoutMs>=1&&config.timeoutMs<=200);
        check(!config.prefix.empty()&&config.prefix.size()<160&&config.password.size()<=512);
    }
    void ready(int fd,short events,std::int64_t deadline){
        for(;;){auto remaining=deadline-now();check(remaining>0);pollfd p{fd,events,0};
            int n=poll(&p,1,static_cast<int>(remaining));if(n<0&&errno==EINTR)continue;
            check(n>0&&!(p.revents&(POLLERR|POLLHUP|POLLNVAL))&&(p.revents&events));return;}
    }
    void sendAll(Connection&c,std::string_view bytes,std::int64_t deadline){
        while(!bytes.empty()){auto n=::send(c.fd,bytes.data(),bytes.size(),MSG_NOSIGNAL);
            if(n>0){bytes.remove_prefix(n);continue;}if(n<0&&errno==EINTR)continue;
            check(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK));ready(c.fd,POLLOUT,deadline);}
    }
    void receive(Connection&c,char*target,std::size_t bytes,std::int64_t deadline){
        while(bytes){
            if(c.begin<c.end){const auto count=std::min(bytes,c.end-c.begin);std::copy_n(c.buffer.data()+c.begin,count,target);c.begin+=count;target+=count;bytes-=count;continue;}
            auto n=::recv(c.fd,c.buffer.data(),c.buffer.size(),0);
            if(n>0){c.begin=0;c.end=static_cast<std::size_t>(n);continue;}
            if(n<0&&errno==EINTR)continue;check(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK));ready(c.fd,POLLIN,deadline);
        }
    }
    std::string line(Connection&c,std::int64_t deadline){std::string result;char ch{};
        while(result.size()<256){receive(c,&ch,1,deadline);result+=ch;
            if(result.ends_with("\r\n")){result.resize(result.size()-2);return result;}}
        throw std::runtime_error("Redis response header too long");
    }
    std::optional<std::string> command(Connection&c,const std::vector<std::string>&args,std::int64_t deadline){
        std::string request="*"+std::to_string(args.size())+"\r\n";
        for(auto&a:args)request+="$"+std::to_string(a.size())+"\r\n"+a+"\r\n";
        sendAll(c,request,deadline);auto header=line(c,deadline);check(!header.empty());
        if(header[0]=='+'||header[0]==':')return header.substr(1);
        check(header[0]=='$');long long count{};auto begin=header.data()+1;auto parsed=std::from_chars(begin,header.data()+header.size(),count);
        check(parsed.ec==std::errc{}&&parsed.ptr==header.data()+header.size()&&count>=-1&&count<=static_cast<long long>(maxValue+256));
        if(count==-1)return std::nullopt;
        std::string result(static_cast<std::size_t>(count)+2,'\0');receive(c,result.data(),result.size(),deadline);
        check(result.ends_with("\r\n"));result.resize(count);return result;
    }
    void connect(Connection&c,std::int64_t deadline){if(c.fd>=0)return;
        c.fd=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);check(c.fd>=0);
        sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons(config.port);inet_pton(AF_INET,config.address.c_str(),&target.sin_addr);
        if(::connect(c.fd,reinterpret_cast<sockaddr*>(&target),sizeof(target))<0){check(errno==EINPROGRESS);ready(c.fd,POLLOUT,deadline);int error{};socklen_t size=sizeof(error);check(getsockopt(c.fd,SOL_SOCKET,SO_ERROR,&error,&size)==0&&error==0);}
        if(!config.password.empty())check(command(c,{"AUTH",config.password},deadline)=="OK");
    }
    template<class Operation>void use(Operation operation)noexcept{
        if(now()<retryAfter.load()){++bypassed;return;}
        const auto start=cursor.fetch_add(1);
        for(std::size_t i=0;i<connections.size();++i){auto&c=connections[(start+i)%connections.size()];std::unique_lock lock(c.mutex,std::try_to_lock);if(!lock)continue;
            try{const auto deadline=now()+config.timeoutMs;connect(c,deadline);operation(c,deadline);}
            catch(...){c.reset();++errors;retryAfter=now()+1000;}return;}
        ++bypassed;
    }
    std::string revision(std::string_view bucket){return config.prefix+std::string(bucket)+":generation";}
    std::string key(std::string_view bucket,std::string_view generation){return config.prefix+std::string(bucket)+":"+std::string(generation);}
};
RedisReadCache::RedisReadCache(RedisCacheConfig c):impl_(std::make_unique<Impl>(std::move(c))){}
RedisReadCache::~RedisReadCache()=default;
CacheLookup RedisReadCache::lookup(std::string_view bucket)noexcept{
    CacheLookup result;impl_->use([&](auto&c,auto deadline){
        // 固定只读脚本在 Redis 内原子取得“代次+该代内容”，一次往返替代两次 GET。
        // 失效仍用 INCR；迟到的数据库查询只能写回旧代次，污染不了新缓存。
        const auto response=impl_->command(c,{"EVAL",
            "local g=redis.call('GET',KEYS[1]) or '0'; local v=redis.call('GET',ARGV[1]..g); return g..'\\n'..(v and '1'..v or '0')",
            "1",impl_->revision(bucket),impl_->key(bucket,"")},deadline);
        check(response.has_value());const auto split=response->find('\n');
        check(split!=std::string::npos&&split>0&&split<64&&response->size()>split+1);
        result.generation=response->substr(0,split);
        const char state=(*response)[split+1];check(state=='0'||state=='1');
        if(state=='1')result.value=response->substr(split+2);
    });
    if(result.value)++impl_->hits;else ++impl_->misses;return result;
}
void RedisReadCache::store(std::string_view bucket,std::string_view generation,std::string_view value,unsigned seconds)noexcept{
    if(generation.empty()||value.size()>maxValue||seconds==0||seconds>60)return;
    impl_->use([&](auto&c,auto deadline){check(impl_->command(c,{"SETEX",impl_->key(bucket,generation),std::to_string(seconds),std::string(value)},deadline)=="OK");});
}
void RedisReadCache::invalidate(std::string_view bucket)noexcept{
    impl_->use([&](auto&c,auto deadline){(void)impl_->command(c,{"INCR",impl_->revision(bucket)},deadline);});
}
CacheMetrics RedisReadCache::metrics()const noexcept{return {impl_->hits.load(),impl_->misses.load(),impl_->errors.load(),impl_->bypassed.load()};}
}
