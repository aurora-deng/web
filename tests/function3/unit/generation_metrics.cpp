#include "backend/ai/inference/metrics/GenerationMetrics.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
using namespace webserver::phase11;
namespace { void check(bool ok){if(!ok)throw std::runtime_error("generation metrics contract failed");}
struct Comma final:std::numpunct<char>{char do_decimal_point()const override{return ',';}}; }
int main(){try{
    GenerationMetricsSnapshot m;
    check(m.format().find("webserver_phase11_ai_decode_tokens_per_second_count 0")!=std::string::npos);
    TokenizerUsage u{10,5,4,1'000'000,200'000,1'500'000,3'000,true};
    check(u.valid()); const auto encoded=encodeTokenizerUsage(u);
    const auto decoded=decodeTokenizerUsage(encoded);check(decoded&&decoded->outputTokens==5&&decoded->modelLoadPerformed);
    m.observe(*decoded);check(m.outputTokens==5&&m.decodeRateCount==1&&m.decodeTokens==4&&m.decodeMicroseconds==1'000'000);
    check(m.decodeRateBuckets[1]==0&&m.decodeRateBuckets[2]==1&&m.decodeRateBuckets.back()==1);
    check(m.modelLoad.count==1&&m.queueWait.count==1&&m.firstToken.count==1);
    TokenizerUsage single{10,1,0,0,200'000,0,0,false};m.observe(single);
    check(m.outputTokens==6&&m.decodeRateCount==1&&m.modelLoad.count==1);
    auto invalid=u;invalid.outputTokens=4097;m.observe(invalid);check(m.rejectedMeasurements==1&&m.outputTokens==6);
    invalid=u;invalid.decodeTokens=5;check(!invalid.valid());
    invalid=u;invalid.modelLoadPerformed=false;check(!invalid.valid());
    for(const auto *bad:{"", "2:1:2:1:1:1:0:0:0", "1:1:2:1:1:1:0:0:0:9", "1:1:2:1:1:1:0:0", "1:+1:2:1:1:1:0:0:0", "1:1:2:1:1:1:0:0:2", "1:1:2:1:1:1:0:0:0x", "1:18446744073709551616:2:1:1:1:0:0:0"})check(!decodeTokenizerUsage(bad));
    check(!decodeTokenizerUsage(std::string(193,'9')));
    std::locale::global(std::locale(std::locale::classic(),new Comma));
    check(m.format().find("_sum 1.5")!=std::string::npos);
    std::cout<<"GENERATION_METRICS_OK\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
