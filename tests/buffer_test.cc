#include "buffer.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <string>

using buffer::Buffer;

TEST(BufferTest, InitialState)
{
    Buffer buf;
    EXPECT_EQ(buf.readableBytes(), 0u);
    EXPECT_EQ(buf.writableBytes(), Buffer::kInitialSize);
    EXPECT_EQ(buf.prependableBytes(), Buffer::kCheapPrepend);
}

TEST(BufferTest, AppendAndRetrieve)
{
    Buffer buf;
    buf.append(std::string("hello"));
    EXPECT_EQ(buf.readableBytes(), 5u);
    EXPECT_EQ(buf.writableBytes(), Buffer::kInitialSize - 5);

    std::string s = buf.retrieveAsString(2);
    EXPECT_EQ(s, "he");
    EXPECT_EQ(buf.readableBytes(), 3u);

    EXPECT_EQ(buf.retrieveAllAsString(), "llo");
    EXPECT_EQ(buf.readableBytes(), 0u);
    // After full retrieve, indices reset to kCheapPrepend.
    EXPECT_EQ(buf.prependableBytes(), Buffer::kCheapPrepend);
}

TEST(BufferTest, GrowsWhenNeeded)
{
    Buffer buf;
    std::string big(2000, 'x');     // Exceeds kInitialSize ( 1024 )
    buf.append(big);
    EXPECT_EQ(buf.readableBytes(), 2000u);
    EXPECT_EQ(buf.retrieveAllAsString(), big);
}

TEST(BufferTest, CompactInsteadOfGrowing)
{
    Buffer buf;
    // Fill, consume part, then append again; should compact rather than grow.
    buf.append(std::string(900, 'a'));
    buf.retrieve(800);  // 100 readable remain; consumed space is reclaimable
    buf.append(std::string(200, 'b'));

    // Content is preserved and correct after the internal move.
    std::string result = buf.retrieveAllAsString();
    ASSERT_EQ(result.size(), 300u);
    EXPECT_EQ(result.substr(0, 100), std::string(100, 'a'));
    EXPECT_EQ(result.substr(100), std::string(200, 'b'));
}

TEST(BufferTest, MultipleAppendAccumutable)
{
    Buffer buf;
    buf.append(std::string("abc"));
    buf.append(std::string("def"));
    buf.append(std::string("ghi"));
    EXPECT_EQ(buf.readableBytes(), 9u);
    EXPECT_EQ(buf.retrieveAllAsString(), "abcdefghi");
}

TEST(BufferTest, ReadFdReadsAllData)
{
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    // Payload larger than kInitialSize so both the buffer's writable region and 
    // the readv extrabuf path are exercised.
    std::string payload(100000, 'z');

    Buffer buf;
    // Non-blocking write end so writes don't deadlock when the pipe fills; we
    // interleave writes and reads to drain the pipe
    ::fcntl(fds[1], F_SETFL, O_NONBLOCK);

    size_t written = 0;
    while (written < payload.size())
    {
        ssize_t w = ::write(fds[1], payload.data() + written,
                            payload.size() - written);
        if (w > 0)
        {
            written += static_cast<size_t>(w);
        }

        int err = 0;
        buf.readFd(fds[0], &err);
    }

    // Final drain of anything still buffered in the pipe.
    int err = 0;
    while (buf.readableBytes() < payload.size())
    {
        ssize_t n = buf.readFd(fds[0], &err);
        if (n <= 0) break;
    }

    EXPECT_EQ(buf.readableBytes(), payload.size());
    EXPECT_EQ(buf.retrieveAllAsString(), payload);

    ::close(fds[0]);
    ::close(fds[1]);
}