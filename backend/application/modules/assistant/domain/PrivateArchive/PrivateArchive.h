#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <vector>
namespace webserver::function3 {
struct LegacyPrivateResource{std::string kind;Id id{};std::string title;};
struct LegacyPrivateEntry{Id id{};std::string role,body;};
struct LegacyPrivatePage{
    Id owner{},resource{},after{},through{},upper{};std::string kind,title,checksum;
    std::vector<LegacyPrivateEntry>entries;bool attachments{};
};
/** 长度前缀按UTF-8字节计数，Windows BCrypt与Linux OpenSSL计算相同摘要。 */
inline std::string privateArchiveMaterial(const LegacyPrivatePage&p){
    std::string result;auto field=[&](std::string_view value){result+=std::to_string(value.size())+":";result+=value;};
    field("nebula-private-archive-v1");field(std::to_string(p.owner));field(p.kind);field(std::to_string(p.resource));field(std::to_string(p.after));field(std::to_string(p.through));field(p.title);field(p.attachments?"true":"false");
    for(const auto&e:p.entries){field(std::to_string(e.id));field(e.role);field(e.body);}return result;
}
}
