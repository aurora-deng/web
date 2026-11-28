#pragma once
#include <array>
#include <charconv>
#include <optional>
#include <string_view>
#include <cstdint>
#include <locale>
#include <sstream>
#include <string>

namespace webserver::phase11 {
// Counts come from model token IDs, never UTF-8 bytes or stream callbacks.
// Decode excludes the first token/prefill; includes scheduler and sink overhead.
struct TokenizerUsage {
    std::uint64_t promptTokens = 0, outputTokens = 0;
    std::uint64_t decodeTokens = 0, decodeMicroseconds = 0;
    std::uint64_t firstTokenMicroseconds = 0;
    std::uint64_t modelLoadMicroseconds = 0, queueWaitMicroseconds = 0;
    bool modelLoadPerformed = false;
    [[nodiscard]] bool valid() const noexcept {
        constexpr std::uint64_t hour = 3'600'000'000;
        return promptTokens <= 16'777'216 && outputTokens <= 4096 &&
            decodeTokens == (outputTokens ? outputTokens - 1 : 0) &&
            decodeMicroseconds <= hour && firstTokenMicroseconds <= hour &&
            modelLoadMicroseconds <= hour && queueWaitMicroseconds <= hour &&
            (modelLoadPerformed || modelLoadMicroseconds == 0) &&
            (decodeTokens != 0 || decodeMicroseconds == 0);
    }
};

inline constexpr char tokenizerUsageMetadata[] = "x-nebula-tokenizer-usage";
// Versioned bounded ASCII metadata; no content, IDs or absolute paths.
inline std::string encodeTokenizerUsage(const TokenizerUsage &u) {
    if(!u.valid()) return {};
    return "1:"+std::to_string(u.promptTokens)+":"+std::to_string(u.outputTokens)+":"+
        std::to_string(u.decodeTokens)+":"+std::to_string(u.decodeMicroseconds)+":"+
        std::to_string(u.firstTokenMicroseconds)+":"+std::to_string(u.modelLoadMicroseconds)+":"+
        std::to_string(u.queueWaitMicroseconds)+":"+(u.modelLoadPerformed?"1":"0");
}
inline std::optional<TokenizerUsage> decodeTokenizerUsage(std::string_view input) noexcept {
    if(input.size()>192 || !input.starts_with("1:")) return std::nullopt;
    input.remove_prefix(2); std::array<std::uint64_t,8> fields{};
    for(std::size_t i=0;i<fields.size();++i){
        const auto split=input.find(':');
        if((i+1==fields.size()) != (split==input.npos)) return std::nullopt;
        const auto part=input.substr(0,split);
        if(part.empty() || part.size()>20) return std::nullopt;
        const auto result=std::from_chars(part.data(),part.data()+part.size(),fields[i]);
        if(result.ec!=std::errc{} || result.ptr!=part.data()+part.size()) return std::nullopt;
        if(split!=input.npos) input.remove_prefix(split+1);
    }
    if(fields[7]>1) return std::nullopt;
    TokenizerUsage u{fields[0],fields[1],fields[2],fields[3],fields[4],fields[5],fields[6],fields[7]!=0};
    return u.valid()?std::optional<TokenizerUsage>{u}:std::nullopt;
}

// Accessed under AiChatService's existing mutex, then copied for formatting.
struct GenerationMetricsSnapshot {
    static constexpr std::array<double, 11> rateBounds{1,2,4,8,16,32,64,128,256,512,1024};
    static constexpr std::array<std::uint64_t, 9> timeBounds{
        1'000,10'000,100'000,500'000,1'000'000,5'000'000,30'000'000,120'000'000,300'000'000};
    struct TimeHistogram {
        std::array<std::uint64_t, timeBounds.size()+1> buckets{};
        std::uint64_t count = 0, microseconds = 0;
        void observe(std::uint64_t value) noexcept {
            for (std::size_t i=0;i<timeBounds.size();++i) if(value<=timeBounds[i]) ++buckets[i];
            ++buckets.back(); ++count; microseconds += value;
        }
    };
    std::uint64_t measuredGenerations = 0, rejectedMeasurements = 0;
    std::uint64_t outputTokens = 0, decodeTokens = 0, decodeMicroseconds = 0;
    std::array<std::uint64_t, rateBounds.size()+1> decodeRateBuckets{};
    std::uint64_t decodeRateCount = 0;
    double decodeRateSum = 0;
    TimeHistogram modelLoad, queueWait, firstToken;

    void observe(const TokenizerUsage &usage) noexcept {
        if (!usage.valid()) { ++rejectedMeasurements; return; }
        ++measuredGenerations; outputTokens += usage.outputTokens;
        queueWait.observe(usage.queueWaitMicroseconds);
        if(usage.modelLoadPerformed) modelLoad.observe(usage.modelLoadMicroseconds);
        if(usage.outputTokens) firstToken.observe(usage.firstTokenMicroseconds);
        if(!usage.decodeTokens || !usage.decodeMicroseconds) return;
        decodeTokens += usage.decodeTokens; decodeMicroseconds += usage.decodeMicroseconds;
        const double rate = static_cast<double>(usage.decodeTokens)*1'000'000.0/usage.decodeMicroseconds;
        for(std::size_t i=0;i<rateBounds.size();++i) if(rate<=rateBounds[i]) ++decodeRateBuckets[i];
        ++decodeRateBuckets.back(); ++decodeRateCount; decodeRateSum += rate;
    }
    [[nodiscard]] std::string format() const {
        std::ostringstream out; out.imbue(std::locale::classic());
        const auto counter=[&](const char *name,std::uint64_t value){out<<"# TYPE "<<name<<" counter\n"<<name<<' '<<value<<'\n';};
        counter("webserver_phase11_ai_tokenizer_generations_total",measuredGenerations);
        counter("webserver_phase11_ai_tokenizer_invalid_measurements_total",rejectedMeasurements);
        counter("webserver_phase11_ai_tokenizer_output_tokens_total",outputTokens);
        counter("webserver_phase11_ai_decode_tokens_total",decodeTokens);
        out<<"# TYPE webserver_phase11_ai_decode_seconds_total counter\nwebserver_phase11_ai_decode_seconds_total "<<decodeMicroseconds/1'000'000.0<<'\n';
        const char *rate="webserver_phase11_ai_decode_tokens_per_second";
        out<<"# TYPE "<<rate<<" histogram\n";
        for(std::size_t i=0;i<rateBounds.size();++i) out<<rate<<"_bucket{le=\""<<rateBounds[i]<<"\"} "<<decodeRateBuckets[i]<<'\n';
        out<<rate<<"_bucket{le=\"+Inf\"} "<<decodeRateBuckets.back()<<'\n'
           <<rate<<"_count "<<decodeRateCount<<'\n'<<rate<<"_sum "<<decodeRateSum<<'\n';
        const auto time=[&](const char *name,const TimeHistogram &h){
            out<<"# TYPE "<<name<<" histogram\n";
            for(std::size_t i=0;i<timeBounds.size();++i) out<<name<<"_bucket{le=\""<<timeBounds[i]/1'000'000.0<<"\"} "<<h.buckets[i]<<'\n';
            out<<name<<"_bucket{le=\"+Inf\"} "<<h.buckets.back()<<'\n'
               <<name<<"_count "<<h.count<<'\n'<<name<<"_sum "<<h.microseconds/1'000'000.0<<'\n';
        };
        time("webserver_phase11_ai_model_load_seconds",modelLoad);
        time("webserver_phase11_ai_provider_queue_wait_seconds",queueWait);
        time("webserver_phase11_ai_tokenizer_first_token_seconds",firstToken);
        return out.str();
    }
};
}
