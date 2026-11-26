#include "SkillCatalog.h"

#include <algorithm>
#include <array>
#include <regex>
#include <set>
#include <stdexcept>

namespace nebula::agent {
ParameterSchema ParameterSchema::object(std::map<std::string, ParameterSchema, std::less<>> p, std::vector<std::string> r) {
    ParameterSchema s{Kind::object}; s.properties = std::move(p); s.required = std::move(r); return s;
}
ParameterSchema ParameterSchema::array(ParameterSchema child, unsigned low, unsigned high, bool u) {
    ParameterSchema s{Kind::array}; s.items = std::make_shared<ParameterSchema>(std::move(child));
    s.minimum=low; s.maximum=high; s.unique=u; return s;
}
ParameterSchema ParameterSchema::string(unsigned low, unsigned high, std::string p, std::vector<std::string> c) {
    ParameterSchema s{Kind::string}; s.minimum=low; s.maximum=high; s.pattern=std::move(p); s.choices=std::move(c); return s;
}
ParameterSchema ParameterSchema::integer(std::int64_t low, std::int64_t high) {
    ParameterSchema s{Kind::integer}; s.minimum=low; s.maximum=high; return s;
}
ParameterSchema ParameterSchema::boolean(std::optional<bool> c) { ParameterSchema s{Kind::boolean}; s.constant=c; return s; }

#include "SkillCatalog.generated.inc"

namespace {
bool matches(const ParameterSchema& s, const ParameterValue& v, unsigned depth=0) {
    using Kind=ParameterSchema::Kind;
    if (depth>8) return false;
    if (s.kind==Kind::object) {
        const auto* obj=std::get_if<Arguments>(&v.value); if (!obj || obj->size()>s.properties.size()) return false;
        for (const auto& key:s.required) if (!obj->contains(key)) return false;
        for (const auto& [key,value]:*obj) {
            const auto found=s.properties.find(key);
            if (found==s.properties.end() || !matches(found->second,value,depth+1)) return false;
        }
        return true;
    }
    if (s.kind==Kind::array) {
        const auto* a=std::get_if<ParameterValue::Array>(&v.value);
        if (!a || !s.items || a->size()<static_cast<std::size_t>(s.minimum) || a->size()>static_cast<std::size_t>(s.maximum)) return false;
        for (auto it=a->begin();it!=a->end();++it)
            if (!matches(*s.items,*it,depth+1) || (s.unique && std::find(a->begin(),it,*it)!=it)) return false;
        return true;
    }
    if (s.kind==Kind::string) {
        const auto* text=std::get_if<std::string>(&v.value); if (!text) return false;
        // Count UTF-8 code points, rejecting malformed sequences and controls.
        std::size_t count=0;
        for (std::size_t i=0;i<text->size();) {
            const auto c=static_cast<unsigned char>((*text)[i]); unsigned n=0;
            std::uint32_t cp=0;
            if (c<0x80) { n=1; cp=c; }
            else if (c>=0xc2 && c<=0xdf) { n=2; cp=c&31; }
            else if (c>=0xe0 && c<=0xef) { n=3; cp=c&15; }
            else if (c>=0xf0 && c<=0xf4) { n=4; cp=c&7; }
            else return false;
            if (i+n>text->size()) return false;
            for (unsigned j=1;j<n;++j) { const auto b=static_cast<unsigned char>((*text)[i+j]); if ((b&0xc0)!=0x80) return false; cp=(cp<<6)|(b&63); }
            if (cp<32 || (n==2 && cp<0x80) || (n==3 && cp<0x800) || (n==4 && cp<0x10000) || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return false;
            i+=n; ++count;
        }
        return count>=static_cast<std::size_t>(s.minimum) && count<=static_cast<std::size_t>(s.maximum) &&
            (s.pattern.empty() || std::regex_match(*text,std::regex(s.pattern))) &&
            (s.choices.empty() || std::ranges::find(s.choices,*text)!=s.choices.end());
    }
    if (s.kind==Kind::integer) { const auto* i=std::get_if<std::int64_t>(&v.value); return i && *i>=s.minimum && *i<=s.maximum; }
    const auto* b=std::get_if<bool>(&v.value); return b && (!s.constant || *b==*s.constant);
}
}

void validateCatalog(std::span<const SkillSpec> skills) {
    std::set<std::pair<std::string_view,unsigned>> identities;
    for (const auto& s:skills) {
        if (s.id.empty() || !s.version || !s.schema || s.parameterSchema.empty() || s.description.empty() ||
            !s.timeoutMs || s.timeoutMs>30000 || !identities.emplace(s.id,s.version).second || s.preconditions.empty())
            throw std::invalid_argument("invalid or duplicate skill");
        std::set<Precondition> conditions;
        for (const auto p:s.preconditions) {
            if (p!=Precondition::authorizedActor && p!=Precondition::humanApproval &&
                p!=Precondition::confirmedUserIntent && p!=Precondition::atomicIdempotentPort)
                throw std::invalid_argument("unknown precondition");
            if (!conditions.insert(p).second) throw std::invalid_argument("duplicate precondition");
        }
        if (!conditions.contains(Precondition::authorizedActor) ||
            (s.effect==SkillEffect::proposal && !conditions.contains(Precondition::humanApproval)) ||
            (s.effect==SkillEffect::external && (s.idempotencyKey.empty() || !conditions.contains(Precondition::confirmedUserIntent) || !conditions.contains(Precondition::atomicIdempotentPort))) ||
            (s.effect!=SkillEffect::read && s.effect!=SkillEffect::proposal && s.effect!=SkillEffect::external))
            throw std::invalid_argument("invalid effect preconditions");
    }
}
std::span<const SkillSpec> skillCatalog() {
    static const bool valid=[] { validateCatalog(generatedSkills); return true; }(); (void)valid;
    return generatedSkills;
}
const SkillSpec* findSkill(std::span<const SkillSpec> skills, std::string_view id, unsigned version) {
    const auto found=std::ranges::find_if(skills,[&](const auto& s) { return s.id==id && s.version==version; });
    return found==skills.end()?nullptr:&*found;
}
const SkillSpec* findSkill(std::string_view id,unsigned version) { return findSkill(skillCatalog(),id,version); }
bool validateArguments(const SkillSpec& skill,const Arguments& args) {
    if (!skill.schema || !matches(*skill.schema,ParameterValue(args))) return false;
    if (skill.id=="chat.recall") {
        const auto scope=std::get<std::string>(args.at("scope").value);
        if ((scope=="conversation")!=args.contains("conversationId")) return false;
    }
    return true;
}
}
