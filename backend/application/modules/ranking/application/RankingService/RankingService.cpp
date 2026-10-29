#include "RankingService.h"
#include <algorithm>
namespace webserver::function3 {
bool RankingService::settle() {
    bool changed=false;
    work_.transact([&]{
        const auto now=repository_.databaseNow(); const auto slot=now/30000;
        if(!repository_.lockSettlementSlot(slot)) return;
        auto candidates=tree_.topCandidates(now);
        std::erase_if(candidates,[&](const auto &p){return heat(p,now)<0;});
        std::sort(candidates.begin(),candidates.end(),[&](const auto&a,const auto&b){
            auto x=heat(a,now),y=heat(b,now);
            if(x!=y)return x>y;
            if(a.createdAt!=b.createdAt)return a.createdAt<b.createdAt;
            return a.post<b.post;
        });
        if(candidates.size()>10)candidates.resize(10);
        std::vector<RankEntry> entries;
        for(auto &p:candidates) entries.push_back({p.post,heat(p,now)});
        const auto old=repository_.latest();
        changed=old.entries.size()!=entries.size();
        if(!changed) for(std::size_t i=0;i<entries.size();++i) if(old.entries[i].post!=entries[i].post){changed=true;break;}
        const auto snapshot=repository_.save(slot,now,entries);
        // 首轮建立基线，后续只比较成员和位置，分值衰减不会触发全服刷屏。
        if(changed && old.version) announcements_.rankingChanged(snapshot,candidates);
    });
    return changed;
}
}
