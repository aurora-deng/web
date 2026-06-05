#pragma once
#include <sys/uio.h>
#include <vector>
#include <string>
#include <memory>
#include "server/http/http.h"

// 抽象类模版
class RespBody
{
public:
    virtual ~RespBody() = default;

    virtual bool buildIov(std::vector<iovec> &vec) = 0;

    virtual void consume(size_t bytes) = 0;

    virtual bool finished() const = 0;
};

class StringBody : public RespBody
{
public:
    ~StringBody() override = default;
    explicit StringBody(std::shared_ptr<ResponseBody> body) : body_(std::move(body)) {}
    bool buildIov(std::vector<iovec> &vec) override;
    void consume(size_t bytes) override;
    bool finished() const override;

private:
    std::shared_ptr<ResponseBody> body_;
    size_t offset_ = 0;
};

struct ChunkNode
{
    std::string sizeLine;
    std::string data;
    std::string tail = "\r\n";
};

class ChunkedBody : public RespBody
{
public:
    bool buildIov(std::vector<iovec> &vec) override;
    void consume(size_t bytes) override;
    bool finished() const override;

private:
    std::vector<ChunkNode> chunks;

    size_t chunkIndex = 0;

    size_t offset = 0;
};

class FileBody : public RespBody
{
public:
    int fd;
    off_t offset;
    size_t fileSize;
    bool buildIov(std::vector<iovec> &vec) override;
    void consume(size_t bytes) override;
    bool finished() const override;
};