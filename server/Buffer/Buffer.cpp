#include "Buffer.h"

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

char *Buffer::beginWrite()
{
    return buf.data()+writePos;
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
        // 未读取的有效数据 整体搬到缓冲区最前面，重置读位置。
        //         缓冲区原始：[已读数据][有效数据][空闲空间]
        // 下标：      0       readPos      writePos     end
        // 变为
        // [有效数据][空闲空间................]
        std::copy(buf.begin() + readPos, buf.begin() + writePos, buf.begin());
        // 更新位置,这里容易出现段错误
        readPos=0;
        writePos=readable;
    }
    else
    {
        buf.resize(writePos + len);
    }
}

void Buffer::append(const char *data, size_t len)
{
    ensureWrite(len);
    // std::copy(data, data + len, buf.begin() + writePos);
    memcpy(beginWrite(),data,len);
    writePos += len;
}

void Buffer::append(std::string_view s)
{
    append(s.data(),s.size());
}

void Buffer::clear()
{
    readPos=0;
    writePos=0;
}

const char *Buffer::findCRLFCRLF() const
{
    const char *begin=peek();
    const char *end=begin+readableBytes();

    const char *p=std::search(begin,end,"\r\n\r\n","\r\n\r\n"+4);

    if(p==end)return nullptr;

    return p;
}
