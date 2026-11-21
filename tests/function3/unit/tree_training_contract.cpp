#include "backend/application/modules/tree/application/TreeTrainingService/TreeTrainingService.h"
#include "backend/application/modules/tree/infrastructure/InMemoryTreeRepository/InMemoryTreeRepository.h"
#include <iostream>
#include <stdexcept>

using namespace webserver::function3;

namespace {
constexpr Id author = 1, visitor = 2, moderatorId = 3, administrator = 4;
constexpr Id publicPost = 101, privatePost = 102;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void rejects(Failure code, const auto& operation) {
    try { operation(); }
    catch (const BusinessError& error) {
        check(error.code == code, "wrong authorization error");
        return;
    }
    throw std::runtime_error("operation unexpectedly accepted");
}

class TrainingRows final : public ITreeTrainingRepository {
public:
    std::optional<TreeTrainingSource> source(Id post) const override {
        if (post == publicPost) return TreeTrainingSource{author, true, allowed_};
        if (post == privatePost) return TreeTrainingSource{author, false, false};
        return std::nullopt;
    }

    bool setConsent(Id post, bool allowed) override {
        check(post == publicPost, "private post reached consent storage");
        if (allowed == allowed_) return false;
        allowed_ = allowed;
        reviewed_.clear();
        return true;
    }

    bool review(Id post, Id reviewer, const std::string& text) override {
        check(post == publicPost && reviewer == administrator, "invalid reviewer reached storage");
        if (!allowed_) return false;
        reviewed_ = text;
        return true;
    }

    std::vector<TreeTrainingText> page(Id before, bool approved) const override {
        if (!allowed_ || (before && publicPost >= before) || (approved && reviewed_.empty())) return {};
        return {{publicPost, approved ? reviewed_ : "source text", !reviewed_.empty()}};
    }
private:
    bool allowed_{};
    std::string reviewed_;
};
}

int main() {
    try {
        InMemoryTreeRepository store;
        store.seedActor({author, "author"});
        store.seedActor({visitor, "visitor"});
        store.seedActor({moderatorId, "moderator", Role::Moderator});
        store.seedActor({administrator, "administrator", Role::Administrator});
        TrainingRows rows;
        TreeTrainingService training(store, store, rows, store);

        check(!training.consent(author, publicPost), "public visibility granted training consent");
        rejects(Failure::NotFound, [&] { training.setConsent(visitor, publicPost, true, 1); });
        rejects(Failure::Invalid, [&] { training.setConsent(author, privatePost, true, 2); });
        rejects(Failure::Forbidden, [&] { training.page(visitor, 0, false); });
        rejects(Failure::Forbidden, [&] { training.review(moderatorId, publicPost, "sample", 3); });
        rejects(Failure::Conflict, [&] { training.review(administrator, publicPost, "sample", 4); });
        training.setConsent(author, publicPost, true, 5);
        training.setConsent(author, publicPost, true, 6);
        check(store.events().size() == 1, "duplicate consent created an audit event");
        check(training.page(administrator, 0, true).empty(), "unreviewed text reached export");
        training.review(administrator, publicPost, "  redacted text  ", 7);
        const auto corpus = training.page(administrator, 0, true);
        check(corpus.size() == 1 && corpus.front().text == "redacted text", "export did not use reviewed text");
        training.setConsent(author, publicPost, false, 8);
        check(training.page(administrator, 0, true).empty(), "revoked post reached export");
        training.setConsent(author, publicPost, true, 9);
        check(training.page(administrator, 0, true).empty(), "renewed consent kept the old review");
        check(store.events().size() == 4, "consent and review audit events missing");
        std::cout << "PASS training consent, role boundaries and review reset\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
