#pragma once
#include "backend/application/modules/assistant/ports/IPrivateArchive/IPrivateArchive.h"
namespace webserver::function3 {
class PrivateArchiveService final{
public:
    PrivateArchiveService(IActorDirectory&actors,IPrivateArchive&archives,std::function<void(Id)>invalidate={}):actors_(actors),archives_(archives),invalidate_(std::move(invalidate)){}
    auto resources(Id actor){activeActor(actors_,actor);return archives_.resources(actor);}
    LegacyPrivatePage exportPage(Id actor,std::string kind,Id resource,Id after,Id upper);
    void purgePage(Id actor,std::string kind,Id resource,Id after,Id through,std::string checksum,bool confirmed);
    static std::string digest(const LegacyPrivatePage&page);
private:IActorDirectory&actors_;IPrivateArchive&archives_;std::function<void(Id)>invalidate_;
};
}
