#pragma once

#include <array>
#include <span>
#include <string_view>

namespace nebula::fleet {

enum class RobotEffect { read, external, idempotent };

struct RobotSkill {
    std::string_view id;
    unsigned version;
    RobotEffect effect;
};

struct SkillReference {
    std::string_view id;
    unsigned version;
};

inline constexpr RobotSkill getRobotStatus{"robot.get_status", 1, RobotEffect::read};
inline constexpr RobotSkill navigateToSimStation{"robot.navigate_to_sim_station", 1, RobotEffect::external};
inline constexpr RobotSkill reportTaskOutcome{"robot.report_task_outcome", 1, RobotEffect::idempotent};
inline constexpr std::array robotSkills{getRobotStatus, navigateToSimStation, reportTaskOutcome};

const RobotSkill* resolveRobotSkill(SkillReference ref);
bool validRobotSkills(std::span<const SkillReference> declared);

}
