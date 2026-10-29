#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <cmath>
#include <vector>
namespace webserver::function3 {
struct RankCandidate { Id post{}; std::string authorName; Milliseconds createdAt{}; std::uint64_t likes{}, repliers{}; };
struct RankEntry { Id post{}; double score{}; };
struct RankSnapshot { Id version{}; Milliseconds settledAt{}; std::vector<RankEntry> entries; };
inline double heat(const RankCandidate &p,Milliseconds now) {
    if(p.createdAt>now || now-p.createdAt>=7LL*24*60*60*1000) return -1;
    const double hours=static_cast<double>(now-p.createdAt)/3600000.;
    return (3.*static_cast<double>(p.likes)+2.*static_cast<double>(p.repliers))/std::pow(hours+2.,1.2);
}
}
