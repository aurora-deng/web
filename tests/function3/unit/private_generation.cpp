#include "backend/ai/private/application/PrivateGenerationService/PrivateGenerationService.h"
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
using namespace webserver::phase11;
void check(bool v,const char*m){if(!v)throw std::runtime_error(m);}
template<class F>void rejects(ErrorCode code,F f){try{f();}catch(const ApplicationError&e){check(e.code()==code,"wrong failure");return;}throw std::runtime_error("unexpected success");}
class FakeModel final:public IPrivateInference {
public:
 struct Call{GenerationRequest request;TokenSink token;CompletionSink complete;CancellationToken cancel;};
 std::mutex mutex;std::condition_variable changed;std::vector<Call>calls;
 GenerationHandle generate(UserId,GenerationRequest request,TokenSink token,CompletionSink complete,CancellationToken cancel)override{
    const auto id=request.requestId;{std::lock_guard lock(mutex);calls.push_back({std::move(request),std::move(token),std::move(complete),cancel});}changed.notify_all();return{id,cancel};
 }
 Call call(std::size_t index){std::unique_lock lock(mutex);check(changed.wait_for(lock,std::chrono::seconds(2),[&]{return calls.size()>index;}),"model dispatch timed out");return calls[index];}
};
int main(){try{
 FakeModel model;PrivateGenerationConfig config;config.lease=std::chrono::milliseconds(100);config.retention=std::chrono::seconds(5);
 {
 PrivateGenerationService service(model,config);GenerationRequest request;request.requestId="private-1";request.messages={{"user","私有资料测试文本"}};
 service.start(1,9,request);auto first=model.call(0);check(first.request.requester==1,"request owner not bound");
 check(service.start(1,9,request).id=="private-1","private idempotence");
 auto different=request;different.messages[0].content="changed";rejects(ErrorCode::Conflict,[&]{service.start(1,9,different);});
 rejects(ErrorCode::NotFound,[&]{service.poll(2,"private-1",0);});
 auto other=request;other.requestId="private-2";rejects(ErrorCode::RateLimited,[&]{service.start(1,9,other);});
 first.token("你好");auto view=service.poll(1,"private-1",0);check(view.pieces.size()==1&&view.pieces[0]=="你好"&&view.next==1,"token cursor");
 check(service.cancel(1,"private-1"),"cancel missing");first.token("迟到token");first.complete({true,false,"",{}});
 check(service.poll(1,"private-1",0).state=="cancelled"&&service.poll(1,"private-1",0).pieces.size()==1,"late completion overwrote cancel");
 service.start(1,9,other);auto second=model.call(1);second.token("回答");second.complete({true,false,"",{}});
 check(service.poll(1,"private-2",1).state=="completed","completion missing");
 other.requestId="private-expire";service.start(1,9,other);auto third=model.call(2);std::this_thread::sleep_for(std::chrono::milliseconds(260));
 check(service.poll(1,"private-expire",0).state=="cancelled"&&third.cancel.cancelled(),"lease did not cancel model");
 }
 auto late=model.call(2);late.token("service destroyed");late.complete({true,false,"",{}});
 std::cout<<"PASS private owner binding, bounded leases, idempotence, token cursor, cancellation and late callback safety\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
