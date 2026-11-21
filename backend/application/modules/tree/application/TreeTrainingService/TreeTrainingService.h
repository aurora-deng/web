#pragma once
#include "backend/application/modules/tree/ports/ITreeTrainingRepository/ITreeTrainingRepository.h"

namespace webserver::function3 {
class TreeTrainingService final {
public:
    TreeTrainingService(IUnitOfWork& work, IActorDirectory& actors, ITreeTrainingRepository& repository, IEventWriter& events)
        : work_(work), actors_(actors), repository_(repository), events_(events) {}
    bool consent(Id actor, Id post) const;
    void setConsent(Id actor, Id post, bool allowed, Milliseconds now);
    void review(Id actor, Id post, std::string text, Milliseconds now);
    std::vector<TreeTrainingText> page(Id actor, Id before, bool approved) const;
private:
    TreeTrainingSource ownSource(Id actor, Id post) const;
    void requireAdministrator(Id actor) const;
    void audit(Id actor, Id post, const std::string& action, Milliseconds now);
    IUnitOfWork& work_;
    IActorDirectory& actors_;
    ITreeTrainingRepository& repository_;
    IEventWriter& events_;
};
}
