#pragma once

#include <span>
#include <string_view>

namespace nebula::agent {

enum class SkillEffect { read, proposal };

struct SkillSpec {
    std::string_view id;
    unsigned version;
    SkillEffect effect;
};

inline constexpr SkillSpec getTaskStatus{"fleet.get_task_status", 1, SkillEffect::read};
inline constexpr SkillSpec reviewFailures{"fleet.review_failures", 1, SkillEffect::proposal};
inline constexpr SkillSpec proposeSimReschedule{"fleet.propose_sim_reschedule", 1, SkillEffect::proposal};

std::span<const SkillSpec> skillCatalog();
const SkillSpec* findSkill(std::string_view id, unsigned version);

}
