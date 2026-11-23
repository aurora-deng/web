#include "SkillCatalog.h"

#include <algorithm>

namespace nebula::agent {

std::span<const SkillSpec> skillCatalog() {
    static constexpr SkillSpec skills[]{getTaskStatus, reviewFailures, proposeSimReschedule};
    return skills;
}

const SkillSpec* findSkill(std::string_view id, unsigned version) {
    const auto skills = skillCatalog();
    const auto found = std::ranges::find(skills, id, &SkillSpec::id);
    return found != skills.end() && found->version == version ? &*found : nullptr;
}

}
