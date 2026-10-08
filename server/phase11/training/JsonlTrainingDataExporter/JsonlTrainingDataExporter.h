#pragma once

#include "server/phase11/training/TrainingDataService/TrainingDataService.h"

#include <cstddef>
#include <filesystem>

namespace webserver::phase11
{

struct JsonlTrainingDataExporterConfig
{
    std::filesystem::path root;
    std::size_t maximumDatasetBytes = 64U * 1024U * 1024U;
};

/**
 * 把已审核样本导出为内容寻址 JSONL。
 *
 * 文件名由正文 SHA-256 决定，相同数据重复导出只会复用同一文件；先写临时文件再
 * rename，训练进程永远看不到半份数据集。数据库只保存返回的相对路径和指纹。
 */
class JsonlTrainingDataExporter final : public ITrainingDataExporter
{
public:
    explicit JsonlTrainingDataExporter(JsonlTrainingDataExporterConfig config);

    TrainingDatasetArtifact exportDataset(
        std::string_view datasetName,
        const std::vector<TrainingCandidate> &candidates) override;

private:
    JsonlTrainingDataExporterConfig config_;
};

} // namespace webserver::phase11
