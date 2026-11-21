#include "TreeTrainingService.h"
#include "backend/application/modules/shared/TextRules/TextRules.h"

namespace webserver::function3 {
TreeTrainingSource TreeTrainingService::ownSource(Id actor, Id post) const {
    activeActor(actors_, actor);
    auto source = repository_.source(post);
    if (!source || source->author != actor)
        throw BusinessError(Failure::NotFound, "training choice is available to the author");
    return *source;
}

bool TreeTrainingService::consent(Id actor, Id post) const {
    return ownSource(actor, post).allowed;
}

void TreeTrainingService::setConsent(Id actor, Id post, bool allowed, Milliseconds now) {
    work_.transact([&] {
        const auto source = ownSource(actor, post);
        if (allowed && !source.eligible)
            throw BusinessError(Failure::Invalid, "only published public text is eligible");
        if (repository_.setConsent(post, allowed))
            audit(actor, post, allowed ? "tree_training_opt_in" : "tree_training_revoked", now);
    });
}

void TreeTrainingService::requireAdministrator(Id actor) const {
    if (activeActor(actors_, actor).role != Role::Administrator)
        throw BusinessError(Failure::Forbidden, "site administrator required");
}

void TreeTrainingService::review(Id actor, Id post, std::string text, Milliseconds now) {
    text = trimText(std::move(text));
    if (!textLength(text, 2000))
        throw BusinessError(Failure::Invalid, "reviewed text must contain 1 to 2000 characters");
    work_.transact([&] {
        requireAdministrator(actor);
        if (!repository_.review(post, actor, text))
            throw BusinessError(Failure::Conflict, "published post or active authorization no longer available");
        audit(actor, post, "tree_training_reviewed", now);
    });
}

std::vector<TreeTrainingText> TreeTrainingService::page(Id actor, Id before, bool approved) const {
    requireAdministrator(actor);
    return repository_.page(before, approved);
}

void TreeTrainingService::audit(Id actor, Id post, const std::string& action, Milliseconds now) {
    events_.appendEvent({0, "admin.audit", "tree.training", post, static_cast<std::uint64_t>(now),
        {{"actorId", std::to_string(actor)}, {"action", action}, {"reason", "public text training consent"}}, now});
}
}
