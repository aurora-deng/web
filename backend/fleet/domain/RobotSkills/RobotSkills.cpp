#include "RobotSkills.h"

#include <algorithm>

namespace nebula::fleet {

const RobotSkill* resolveRobotSkill(SkillReference ref) {
    const auto match = std::ranges::find_if(robotSkills, [&](const RobotSkill& skill) {
        return ref.id == skill.id && ref.version == skill.version;
    });
    return match == robotSkills.end() ? nullptr : &*match;
}

bool validRobotSkills(std::span<const SkillReference> declared) {
    if (declared.size() != robotSkills.size()) return false;
    return std::ranges::all_of(robotSkills, [&](const RobotSkill& required) {
        return std::ranges::count_if(declared, [&](const SkillReference& ref) {
            return resolveRobotSkill(ref) == &required;
        }) == 1;
    });
}

}
