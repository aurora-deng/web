#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nebula::agent {

enum class SkillEffect { read, proposal, external };
enum class Precondition { authorizedActor, humanApproval, confirmedUserIntent, atomicIdempotentPort };

// Typed input, not arbitrary JSON text or a transaction/SQL port.
struct ParameterValue {
    using Object = std::map<std::string, ParameterValue, std::less<>>;
    using Array = std::vector<ParameterValue>;
    std::variant<bool, std::int64_t, std::string, Array, Object> value{Object{}};
    ParameterValue() = default;
    ParameterValue(bool v) : value(v) {}
    ParameterValue(int v) : value(static_cast<std::int64_t>(v)) {}
    ParameterValue(std::int64_t v) : value(v) {}
    ParameterValue(const char* v) : value(std::string(v)) {}
    ParameterValue(std::string v) : value(std::move(v)) {}
    ParameterValue(Array v) : value(std::move(v)) {}
    ParameterValue(Object v) : value(std::move(v)) {}
    bool operator==(const ParameterValue&) const = default;
};
using Arguments = ParameterValue::Object;

struct ParameterSchema {
    enum class Kind { object, array, string, integer, boolean };
    Kind kind;
    std::map<std::string, ParameterSchema, std::less<>> properties;
    std::vector<std::string> required;
    std::shared_ptr<const ParameterSchema> items;
    std::int64_t minimum{}, maximum{};
    bool unique{};
    std::string pattern;
    std::vector<std::string> choices;
    std::optional<bool> constant;
    static ParameterSchema object(std::map<std::string, ParameterSchema, std::less<>>, std::vector<std::string>);
    static ParameterSchema array(ParameterSchema, unsigned, unsigned, bool);
    static ParameterSchema string(unsigned, unsigned, std::string, std::vector<std::string>);
    static ParameterSchema integer(std::int64_t, std::int64_t);
    static ParameterSchema boolean(std::optional<bool>);
};

struct SkillSpec {
    std::string_view id;
    unsigned version;
    SkillEffect effect;
    std::string_view parameterSchema; // Generated alongside its executable typed validator.
    std::span<const Precondition> preconditions;
    std::string_view idempotencyKey;
    unsigned timeoutMs;
    std::string_view description;
    const ParameterSchema* schema;
};

extern const SkillSpec getTaskStatus;
extern const SkillSpec reviewFailures;
extern const SkillSpec proposeSimReschedule;
std::span<const SkillSpec> skillCatalog();
const SkillSpec* findSkill(std::string_view id, unsigned version);
const SkillSpec* findSkill(std::span<const SkillSpec> skills, std::string_view id, unsigned version);
// Throws at startup on duplicate (id,version), invalid effects or preconditions.
void validateCatalog(std::span<const SkillSpec> skills);
bool validateArguments(const SkillSpec& skill, const Arguments& parameters);

}
