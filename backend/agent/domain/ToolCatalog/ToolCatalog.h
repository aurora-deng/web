#pragma once

#include <array>
#include <string_view>

namespace nebula::agent {

struct ToolSpec {
    std::string_view id;
    std::string_view description;
    std::string_view input;
};

// Explicit read-only contracts; service/repository types cannot register tools.
inline constexpr std::array<ToolSpec, 3> tools{{
    {"fleet.get_task_status", "Read one authorized task status.", "taskId"},
    {"fleet.list_tasks", "Read a page of at most 16 authorized Fleet tasks.", "beforeEpoch,limit"},
    {"fleet.query_evidence", "Read the task's trace ID, epoch, outcome and reconciliation state.", "taskId"},
}};

}  // namespace nebula::agent
