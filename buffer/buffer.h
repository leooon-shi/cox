#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace buffer
{
// A growable byte buffer for non-blocking I/O, modeled after muduo::net::Buffer.
//
// Layout:
//   +-------------------+-------------------+-------------------+
//   | prependable bytes |  readable bytes   |   writable bytes  |
//   |                   |      (CONTENT)    |                   |
//   +-------------------+-------------------+-------------------+
//   0      <=      readerIndex     <=    writerIndex    <=    size
//
// The prependable region allows cheap prepending of, e.g., a length header
// without shifting the payload. When the writable region is exhaused, the
// buffer either compacts (reclaiming consumed prependable space) or grows.
class Buffer {
public:
    static constexpr size_t kCheapPrepend = 8;
    static constexpr size_t kInitialSize = 1024;

    explicit Buffer(size_t initial_size = kInitialSize)
    : data_(kCheapPrepend + initial_size),
      reader_index_(kCheapPrepend),
      writer_index_(kCheapPrepend)
      {
      }
    
    size_t readableBytes() const { return writer_index_ - reader_index_; }
    size_t writableBytes() const { return data_.size() - writer_index_; }
    size_t prependableBytes() const { return reader_index_; }

    // Pointer to the first readable byte.
    const char* peek() const { return begin() + reader_index_; }

    // Consume `len` bytes from the readable region.
    void retrieve(size_t len)
    {
        assert(len <= readableBytes());
        if (len < readableBytes())
        {
            reader_index_ += len;
        } else {
            retrieveAll();
        }
    }

    void retrieveAll()
    {
        reader_index_ = kCheapPrepend;
        writer_index_ = kCheapPrepend;
    }

    // Consume all readable bytes and return them as a string
    std::string retrieveAllAsString() {
        return retrieveAsString(readableBytes());
    }

    std::string retrieveAsString(size_t len)
    {
        assert(len <= readableBytes());
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    // Append data to the writable region, growing if necessary.
    void append(const char* data, size_t len)
    {
        ensureWritable(len);
        std::memcpy(beginWrite(), data, len);
        writer_index_ += len;
    }

    void append(const std::string& str)
    {
        append(str.data(), str.size());
    }

    void ensureWritable(size_t len)
    {
        if (writableBytes() < len)
        {
            makeSpace(len);
        }
        assert(writableBytes() >= len);
    }

    char* beginWrite()
    {
        return begin() + writer_index_;
    }
    const char* beginWrite() const
    {
        return begin() + writer_index_;
    }

    void hasWritten(size_t len)
    {
        assert(len <= writableBytes());
        writer_index_ += len;
    }

    // Read available data from an fd into this buffer using a scatter read that
    // avoids repeated small reads. Returns the number of bytes read, or -1 on
    // error (errno set), following ::read sematics.
    ssize_t readFd(int fd, int* saved_errno);

private:
    char* begin() { return data_.data(); }
    const char* begin() const { return data_.data(); }

    void makeSpace(size_t len)
    {
        if (writableBytes() + prependableBytes() < len + kCheapPrepend)
        {
            // Not enough room even after compaction: grow.
            data_.resize(writer_index_ + len);
        } else {
            // Compact: move readable data back to kCheapPrepend.
            size_t readable = readableBytes();
            std::memmove(begin() + kCheapPrepend, begin() + reader_index_, readable);
            reader_index_ = kCheapPrepend;
            writer_index_ = kCheapPrepend;
        }
    }

    std::vector<char> data_;
    size_t reader_index_;
    size_t writer_index_;
};
} // namespace buffer