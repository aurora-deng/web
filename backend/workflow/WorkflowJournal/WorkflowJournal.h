#pragma once

#include "../WorkflowGraph/WorkflowGraph.h"

#include <mutex>
#include <optional>
#include <stdexcept>

namespace nebula::workflow {

// open() rejected a new run before any node or external effect was started.
class JournalCapacityExceeded final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Checkpoint {
    unsigned attempts{};
    Effect effect{};
    std::optional<WorkResult> result;
};

// One active run owns a journal. Journal methods can be called by its worker threads.
class WorkflowJournal {
public:
    virtual ~WorkflowJournal() = default;
    std::unique_lock<std::mutex> claim();
    virtual std::string open(std::string_view run, std::string_view definition, std::string_view trace) = 0;
    virtual Checkpoint node(std::string_view run, std::string_view node) = 0;
    virtual void start(std::string_view run, std::string_view node, Effect effect, unsigned attempt) = 0;
    virtual void settle(std::string_view run, std::string_view node, const WorkResult& result) = 0;

private:
    std::mutex owner_;
};

}
