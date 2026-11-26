#include "backend/agent/domain/SkillCatalog/SkillCatalog.h"
#include <iostream>
#include <stdexcept>
using namespace nebula::agent;
ParameterValue sample(const ParameterSchema& s) {
    using K=ParameterSchema::Kind;
    if (s.kind==K::object) { Arguments o; for(const auto& k:s.required) o.emplace(k,sample(s.properties.at(k))); return o; }
    if (s.kind==K::array) { ParameterValue::Array a; for(auto i=0;i<s.minimum;++i) a.push_back(sample(*s.items)); return a; }
    if (s.kind==K::integer) return s.minimum;
    if (s.kind==K::boolean) return s.constant.value_or(true);
    if(!s.choices.empty()) return s.choices.front();
    if(s.pattern=="\\d{4}-\\d{2}-\\d{2}") return "2026-11-25";
    return std::string(static_cast<std::size_t>(std::max<std::int64_t>(s.minimum,1)),'a');
}
int main() { try {
    std::size_t checked=0;
    for (const auto& skill:skillCatalog()) {
        auto args=std::get<Arguments>(sample(*skill.schema).value);
        if(skill.id=="chat.recall") args.emplace("conversationId","a");
        if(!validateArguments(skill,args)) throw std::runtime_error("valid sample rejected: "+std::string(skill.id));
        auto extra=args; extra.emplace("unknown",true);
        if(validateArguments(skill,extra)) throw std::runtime_error("extra field accepted");
        for(const auto& key:skill.schema->required) { auto missing=args; missing.erase(key); if(validateArguments(skill,missing)) throw std::runtime_error("missing field accepted"); }
        ++checked;
    }
    std::cout<<"Executable schemas passed for "<<checked<<" skills across read/proposal/external\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
