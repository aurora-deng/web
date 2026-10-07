#include "server/phase11/ai/DeterministicKnowledgeIndex/DeterministicKnowledgeIndex.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <unordered_set>

namespace webserver::phase11
{
namespace
{

using Frequencies = std::unordered_map<std::string, double>;

Frequencies tokenize(std::string_view text)
{
    Frequencies tokens;
    std::string ascii;
    const auto flush = [&]
    {
        if (!ascii.empty())
        {
            ++tokens[ascii];
            ascii.clear();
        }
    };
    for (std::size_t index = 0; index < text.size();)
    {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte < 0x80)
        {
            if (std::isalnum(byte))
                ascii.push_back(static_cast<char>(std::tolower(byte)));
            else
                flush();
            ++index;
            continue;
        }
        flush();
        std::size_t width = 1;
        if ((byte & 0xE0U) == 0xC0U) width = 2;
        else if ((byte & 0xF0U) == 0xE0U) width = 3;
        else if ((byte & 0xF8U) == 0xF0U) width = 4;
        width = std::min(width, text.size() - index);
        ++tokens[std::string(text.substr(index, width))];
        index += width;
    }
    flush();
    return tokens;
}

double cosine(const Frequencies &query, const Frequencies &document)
{
    double dot = 0.0;
    double queryNorm = 0.0;
    double documentNorm = 0.0;
    for (const auto &[token, count] : query)
    {
        queryNorm += count * count;
        const auto found = document.find(token);
        if (found != document.end()) dot += count * found->second;
    }
    for (const auto &[_, count] : document)
        documentNorm += count * count;
    if (dot == 0.0 || queryNorm == 0.0 || documentNorm == 0.0)
        return 0.0;
    return dot / (std::sqrt(queryNorm) * std::sqrt(documentNorm));
}

} // namespace

void DeterministicKnowledgeIndex::replaceAll(
    std::vector<KnowledgeChunk> chunks)
{
    std::unique_lock lock(mutex_);
    chunks_.clear();
    for (auto &chunk : chunks)
        chunks_.insert_or_assign(chunk.id, std::move(chunk));
}

void DeterministicKnowledgeIndex::upsert(std::vector<KnowledgeChunk> chunks)
{
    std::unique_lock lock(mutex_);
    for (auto &chunk : chunks)
        chunks_.insert_or_assign(chunk.id, std::move(chunk));
}

void DeterministicKnowledgeIndex::removeDocument(
    KnowledgeDocumentId document)
{
    std::unique_lock lock(mutex_);
    std::erase_if(chunks_, [document](const auto &entry)
                  { return entry.second.documentId == document; });
}

std::vector<KnowledgeSearchHit> DeterministicKnowledgeIndex::search(
    std::string_view query,
    const std::vector<KnowledgeDocumentId> &allowedDocuments,
    std::size_t limit) const
{
    if (limit == 0) return {};
    const auto queryTokens = tokenize(query);
    if (queryTokens.empty()) return {};
    const std::unordered_set<KnowledgeDocumentId> allowed(
        allowedDocuments.begin(), allowedDocuments.end());
    std::shared_lock lock(mutex_);
    std::vector<KnowledgeSearchHit> hits;
    for (const auto &[_, chunk] : chunks_)
    {
        if (!allowed.contains(chunk.documentId)) continue;
        const auto score = cosine(queryTokens, tokenize(chunk.content));
        if (score > 0.0)
            hits.push_back({chunk.documentId, chunk.id, score});
    }
    std::sort(hits.begin(), hits.end(), [](const auto &left, const auto &right)
              {
                  if (std::abs(left.score - right.score) > 1e-12)
                      return left.score > right.score;
                  return left.chunkId < right.chunkId;
              });
    if (hits.size() > limit) hits.resize(limit);
    return hits;
}

} // namespace webserver::phase11
