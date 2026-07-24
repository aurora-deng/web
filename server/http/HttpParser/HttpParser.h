#pragma once
#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include "../http.h"

enum class ParseStage
{
    REQUEST_LINE,
    HEADERS,
    BODY,
    CHUNK_SIZE,
    CHUNK_DATA,
    COMPLETE,
    ERROR
};
// 负责解析
class HttpParser
{
public:
    ParseState parse(Buffer &buffer, HttpRequest &req);
    void reset();
    size_t getContentLength() const
    {
        return contentLength;
    }

    bool keepAlive() const
    {
        return keepAlive_;
    }

private:
    ParseStage stage = ParseStage::REQUEST_LINE;

    size_t contentLength = 0;

    bool keepAlive_ = true;

    bool hasRange = false;

    RangeInfo range;
    bool headerFinished = false;

    bool chunked = false;
    size_t currentChunkSize = 0;
    ParseState parseRequestLine(Buffer &, HttpRequest &);
    ParseState parseHeaders(Buffer &, HttpRequest &);
    ParseState parseChunkSize(Buffer &);
    ParseState parseChunkData(Buffer& buf,HttpRequest& req);
    ParseState parseBody(Buffer &, HttpRequest &);
};
#endif