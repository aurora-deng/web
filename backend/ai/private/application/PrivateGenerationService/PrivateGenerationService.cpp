#include "PrivateGenerationService.h"
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include "backend/application/runtime/BoundedExecutor/BoundedExecutor.h"
#include <openssl/sha.h>
#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
namespace webserver::phase11 {
namespace {
using Clock=std::chrono::steady_clock;
bool active(const std::string&state){return state=="queued"||state=="running";}
std::string identity(UserId owner,const std::string&id){return std::to_string(owner)+":"+id;}
std::string signature(UserId ai,const GenerationRequest&request){
    std::string canonical=std::to_string(ai)+":"+std::to_string(request.maxOutputTokens)+":";
    for(const auto&m:request.messages)canonical+=std::to_string(m.role.size())+":"+m.role+std::to_string(m.content.size())+":"+m.content;
    unsigned char digest[SHA256_DIGEST_LENGTH];SHA256(reinterpret_cast<const unsigned char*>(canonical.data()),canonical.size(),digest);
    return std::string(reinterpret_cast<char*>(digest),sizeof(digest));
}
}
struct PrivateGenerationService::State {
    struct Job{UserId owner{};std::string id,signature,state{"queued"},error;std::vector<std::string>pieces;
        std::size_t bytes{};std::uint64_t firstToken{};MessageModelTrace trace;CancellationToken cancellation;
        Clock::time_point accepted{},renewed{},finished{};};
    IPrivateInference&model;PrivateGenerationConfig config;BoundedExecutor executor;
    std::mutex mutex;std::condition_variable changed;std::map<std::string,Job>jobs;bool stopping{};std::thread sweeper;
    State(IPrivateInference&m,PrivateGenerationConfig c):model(m),config(c),executor(2,c.activeLimit){}
    void expire(){const auto now=Clock::now();for(auto it=jobs.begin();it!=jobs.end();){auto&j=it->second;
        if(active(j.state)&&(now-j.renewed>config.lease||now-j.accepted>config.timeout)){j.cancellation.cancel();j.state="cancelled";j.error="private_generation_expired";j.finished=now;}
        if(!active(j.state)&&now-j.finished>config.retention)it=jobs.erase(it);else++it;
    }}
    PrivateGenerationView view(const Job&j,std::size_t after)const {
        if(after>j.pieces.size())throw ApplicationError(ErrorCode::InvalidArgument,"invalid private token cursor");
        const auto end=std::min(j.pieces.size(),after+64);
        return{j.id,j.state,j.error,{j.pieces.begin()+after,j.pieces.begin()+end},end,j.bytes,j.firstToken,j.trace};
    }
};
PrivateGenerationService::PrivateGenerationService(IPrivateInference&model,PrivateGenerationConfig config)
    :state_(std::make_shared<State>(model,config)) {
    if(!config.activeLimit||config.activeLimit>64||config.recordsLimit<config.activeLimit||config.recordsLimit>1024||
       !config.responseBytesLimit||config.responseBytesLimit>1024*1024||config.lease.count()<=0||config.timeout.count()<=0||config.retention.count()<=0)
        throw std::invalid_argument("invalid private inference limits");
    auto*s=state_.get();s->sweeper=std::thread([s]{std::unique_lock lock(s->mutex);while(!s->stopping){s->expire();s->changed.wait_for(lock,std::chrono::milliseconds(100),[s]{return s->stopping;});}});
}
PrivateGenerationService::~PrivateGenerationService(){
    auto s=state_;{std::lock_guard lock(s->mutex);s->stopping=true;for(auto&[_,j]:s->jobs)j.cancellation.cancel();}s->changed.notify_all();
    s->executor.shutdown();if(s->sweeper.joinable())s->sweeper.join();state_.reset();
}
PrivateGenerationView PrivateGenerationService::start(UserId owner,UserId ai,GenerationRequest request){
    if(!owner||!ai||request.requestId.empty()||request.requestId.size()>128||request.messages.empty()||request.messages.size()>26||
       !request.maxOutputTokens||request.maxOutputTokens>2048)throw ApplicationError(ErrorCode::InvalidArgument,"invalid private generation request");
    for(char c:request.requestId)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'))
        throw ApplicationError(ErrorCode::InvalidArgument,"invalid private generation id");
    std::size_t inputBytes=0;for(const auto&m:request.messages){if(m.role!="system"&&m.role!="user"&&m.role!="assistant")throw ApplicationError(ErrorCode::InvalidArgument,"invalid private prompt role");inputBytes+=m.content.size();}
    if(inputBytes>32768)throw ApplicationError(ErrorCode::InvalidArgument,"private context exceeds 32 KiB");
    request.requester=owner;const auto id=request.requestId,key=identity(owner,id),hash=signature(ai,request);auto s=state_;CancellationToken cancellation;
    {
        std::lock_guard lock(s->mutex);s->expire();if(s->stopping)throw ApplicationError(ErrorCode::Unavailable,"private inference is stopping");
        if(auto found=s->jobs.find(key);found!=s->jobs.end()){
            if(found->second.signature!=hash)throw ApplicationError(ErrorCode::Conflict,"private request id already has different content");
            found->second.renewed=Clock::now();return s->view(found->second,0);
        }
        std::size_t total=0;for(const auto&[_,j]:s->jobs)if(active(j.state)){++total;if(j.owner==owner)throw ApplicationError(ErrorCode::RateLimited,"one private generation per user");}
        if(total>=s->config.activeLimit||s->jobs.size()>=s->config.recordsLimit)throw ApplicationError(ErrorCode::Unavailable,"private inference queue is full");
        State::Job j;j.owner=owner;j.id=id;j.signature=hash;j.accepted=j.renewed=Clock::now();j.cancellation=cancellation;s->jobs.emplace(key,std::move(j));
    }
    const std::weak_ptr<State>weak=s;
    const bool submitted=s->executor.submit([weak,key,ai,request=std::move(request),cancellation]()mutable{
        auto state=weak.lock();if(!state)return;
        {std::lock_guard lock(state->mutex);auto i=state->jobs.find(key);if(i==state->jobs.end()||!active(i->second.state))return;i->second.state="running";}
        auto token=[weak,key](std::string_view piece){if(auto state=weak.lock()){
            std::lock_guard lock(state->mutex);auto i=state->jobs.find(key);if(i==state->jobs.end()||!active(i->second.state))return;auto&j=i->second;
            if(piece.size()>16384||j.bytes+piece.size()>state->config.responseBytesLimit||j.pieces.size()>=4096){j.cancellation.cancel();j.state="failed";j.error="private_output_limit";j.finished=Clock::now();return;}
            if(piece.empty())return;if(!j.firstToken)j.firstToken=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-j.accepted).count();
            j.bytes+=piece.size();j.pieces.emplace_back(piece);
        }};
        auto complete=[weak,key](GenerationCompletion result){if(auto state=weak.lock()){
            std::lock_guard lock(state->mutex);auto i=state->jobs.find(key);if(i==state->jobs.end()||!active(i->second.state))return;auto&j=i->second;
            j.state=result.cancelled?"cancelled":result.success?"completed":"failed";j.error=result.success?"":"private_model_failed";j.trace=result.trace;j.finished=Clock::now();
        }};
        try{state->model.generate(ai,std::move(request),std::move(token),complete,cancellation);}
        catch(...){complete({false,cancellation.cancelled(),"",{}});}
    });
    if(!submitted){std::lock_guard lock(s->mutex);s->jobs.erase(key);throw ApplicationError(ErrorCode::Unavailable,"private inference queue is full");}
    return poll(owner,id,0);
}
PrivateGenerationView PrivateGenerationService::poll(UserId owner,const std::string&id,std::size_t after){auto s=state_;std::lock_guard lock(s->mutex);s->expire();auto found=s->jobs.find(identity(owner,id));if(found==s->jobs.end())throw ApplicationError(ErrorCode::NotFound,"private generation unavailable");found->second.renewed=Clock::now();return s->view(found->second,after);}
bool PrivateGenerationService::cancel(UserId owner,const std::string&id){auto s=state_;std::lock_guard lock(s->mutex);auto found=s->jobs.find(identity(owner,id));if(found==s->jobs.end())return false;auto&j=found->second;if(active(j.state)){j.cancellation.cancel();j.state="cancelled";j.finished=Clock::now();}return true;}
}
