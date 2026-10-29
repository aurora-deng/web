#include "backend/application/modules/activities/application/ActivityService/ActivityService.h"
#include "backend/application/modules/tree/application/TreeService/TreeService.h"
#include "backend/application/modules/tree/infrastructure/InMemoryTreeRepository/InMemoryTreeRepository.h"
#include <iostream>
using namespace webserver::function3;
void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(Failure code,F f){try{f();}catch(const BusinessError&e){check(e.code==code,"wrong failure");return;}throw std::runtime_error("unexpected success");}
struct Views:IActivityBookmarks,IProfileDirectory {
    std::map<Id,std::set<Id>> values;
    void set(Id user,Id post,bool enabled,Milliseconds)override{if(enabled)values[user].insert(post);else values[user].erase(post);}
    std::set<Id> contained(Id user,const std::vector<Id>&ids)const override{std::set<Id>result;for(Id id:ids)if(values.contains(user)&&values.at(user).contains(id))result.insert(id);return result;}
    std::vector<Id> page(Id user,Id before,std::size_t limit)const override{std::vector<Id>result;if(!values.contains(user))return result;for(auto i=values.at(user).rbegin();i!=values.at(user).rend()&&result.size()<limit;++i)if(!before||*i<before)result.push_back(*i);return result;}
    PublicProfile profile(Id viewer,Id user)const override{return{user,"user","用户","简介","",viewer==user,false,true,true,true,viewer==2};}
    void configure(Id,bool,bool,std::optional<bool>)override{}
};
int main(){try{
    InMemoryTreeRepository store;for(Id id:{1,2,3})store.seedActor({id,"用户"+std::to_string(id)});
    store.seedCategory({1,"生活"});store.seedFriendship(1,2);
    TreeService content(store,store,store,store,store,&store);Views views;
    ActivityService service(store,store,content,views,views);
    PublishPost p{"profile-1","好友可见的日常",0};p.treeChannel=false;p.visibility="friends";
    auto friends=service.publish(1,p,false,1000);check(content.count(1)==0,"profile must not grow tree");
    check(service.get(2,friends.id).id==friends.id,"friend access");rejects(Failure::NotFound,[&]{service.get(3,friends.id);});
    auto membersProfile=p;membersProfile.requestId="profile-members";membersProfile.visibility="members";
    const auto members=service.publish(1,membersProfile,false,1000);
    rejects(Failure::NotFound,[&]{service.get(3,members.id);});
    check(service.get(2,members.id).id==members.id,"friend sees profile member audience");
    auto privatePost=p;privatePost.requestId="private-1";privatePost.visibility="private";
    const auto onlyMe=service.publish(1,privatePost,false,1001);rejects(Failure::NotFound,[&]{service.get(2,onlyMe.id);});
    check(service.profileActivities(2,1,0,false,24).size()==2,"profile visibility and pagination");
    check(service.profileActivities(3,1,0,false,24).empty(),"stranger profile isolation");
    check(store.events().front().type=="activity.private.changed","private post must not use global tree event");
    auto publicPost=p;publicPost.requestId="both-1";publicPost.treeChannel=true;publicPost.category=1;publicPost.visibility="members";
    rejects(Failure::Invalid,[&]{service.publish(1,publicPost,false,1002);});
    const auto both=service.publish(1,publicPost,true,1002);
    check(service.treeAuthor(3,both.id).id==1,"public fruit author card");
    check(service.treeFriendTarget(3,both.id)==1,"opt-in stranger request");
    rejects(Failure::Conflict,[&]{service.treeFriendTarget(2,both.id);});
    rejects(Failure::NotFound,[&]{service.treeAuthor(1,friends.id);});
    PostQuery batch;batch.ids={friends.id,onlyMe.id,both.id};
    auto visible=content.list(3,batch);check(visible.size()==1&&visible[0].id==both.id,"batch cannot widen channel or private audience");
    check(content.count(2)==1 && service.get(3,both.id).id==both.id,"public content common ID");
    check(service.publish(1,publicPost,true,1003).id==both.id,"channel idempotence");
    auto changed=publicPost;changed.treeChannel=false;rejects(Failure::Conflict,[&]{service.publish(1,changed,false,1003);});
    content.like(2,both.id,true,1004);check(service.get(3,both.id).likes==1,"channels share interactions");
    service.favorite(2,both.id,true,1005);service.favorite(2,both.id,true,1005);
    check(service.favorites(2,0,24).items.size()==1 && service.get(2,both.id).favoritedByViewer,"favorite idempotence");
    check(service.get(2,both.id).likes==1,"favorites do not affect heat");
    rejects(Failure::NotFound,[&]{service.favorite(3,friends.id,true,1005);});
    check(service.share(2,both.id).find("?post=")!=std::string::npos,"share canonical URL");
    content.remove(1,both.id,1006);rejects(Failure::NotFound,[&]{service.share(2,both.id);});
    check(service.favorites(2,0,24).items.empty(),"deleted favorite content must disappear");
    std::cout<<"PASS activity channels, privacy, common IDs/interactions, bookmarks, links and private events\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
