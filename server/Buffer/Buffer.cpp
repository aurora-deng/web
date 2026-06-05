#include "Buffer.h"
#include <algorithm>

Buffer::Buffer(size_t initial)
{
    buf.resize(initial);
}

size_t Buffer::readableBytes() const
{
    return writePos - readPos;
}

size_t Buffer::writableBytes() const
{
    return buf.size() - writePos;
}

const char *Buffer::peek() const
{
    return buf.data() + readPos;
}

void Buffer::retrieve(size_t len)
{
    readPos += len;

    if (readPos == writePos)
    {
        readPos = 0;
        writePos = 0;
    }
}

void Buffer::ensureWrite(size_t len)
{
    if (writableBytes() >= len)
    {
        return;
    }

    // 前面有空间
    if (readPos + writableBytes() >= len)
    {
        size_t readable = readableBytes();

        std::copy(buf.begin() + readPos, buf.begin() + writePos, buf.begin());
    }
    else
    {
        buf.resize(writePos + len);
    }
}

void Buffer::append(const char *data, size_t len)
{
    ensureWrite(len);
    std::copy(data, data + len, buf.begin() + writePos);

    writePos += len;
}

const char *Buffer::findCRLFCRLF() const
{
    const char *begin=peek();
    const char *end=begin+readableBytes();

    const char *p=std::search(begin,end,"\r\n\r\n","\r\n\r\n"+4);

    if(p==end)return nullptr;

    return p;
}
