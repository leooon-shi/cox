#include "logger.h"

#include "async_sink.h"
#include "default_formatter.h"
#include "formatter.h"
#include "json_formatter.h"
#include "log_record.h"
#include "sink.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace logging;
using namespace std::chrono_literals;

namespace
{
// Build a Logger writing into a MemorySink we can inspect. Output goes through
// the real Formatter + Sink path -- no stdout redirection.
struct Harness {
    std::shared_ptr<MemorySink> sink{std::make_shared<MemorySink>()};
    std::unique_ptr<Logger> log;

    Harness(LogLevel level, std::unique_ptr<Formatter> fmt)
    {
        log = std::make_unique<Logger>(level, std::move(fmt), sink);
    }
    std::string text() const { return sink->text(); }
};

Harness makeDefault(LogLevel level = LogLevel::Info)
{
    return Harness(level, Formatter::newDefaultFormater());
}

Harness makeJson(LogLevel level = LogLevel::Info) {
    return Harness(level, Formatter::newJsonFormatter());
}
}   // namespace

TEST(LoggerTest, LevelFilteringDropsBelowThreshold)
{
    auto h = makeDefault(LogLevel::Warning);    // only Warning+ emitted
    h.log->debug("dbg");
    h.log->info("inf");
    h.log->warning("warn");
    h.log->error("err");

    std::string out = h.text();
    EXPECT_EQ(out.find("dbg"), std::string::npos);
    EXPECT_EQ(out.find("inf"), std::string::npos);
    EXPECT_NE(out.find("warn"), std::string::npos);
    EXPECT_NE(out.find("err"), std::string::npos);
}

TEST(LoggerTest, DefaultFormatterHumanReadable)
{
    auto h = makeDefault();
    h.log->info("hello");
    std::string out = h.text();
    EXPECT_NE(out.find("INFO"), std::string::npos);
    EXPECT_NE(out.find("hello"), std::string::npos);
    EXPECT_EQ(out.find("\"msg\""), std::string::npos);    // not JSON
}

TEST(LoggerTest, JsonFormatterEmitsJson)
{
    auto h = makeJson();
    h.log->info("hello");
    std::string out = h.text();
    EXPECT_NE(out.find("\"level\":\"INFO\""), std::string::npos);
    EXPECT_NE(out.find("\"msg\":\"hello\""), std::string::npos);
    EXPECT_NE(out.find("\"ts\":"), std::string::npos);
}

TEST(LoggerTest, StructuredFieldDefault)
{
    auto h = makeDefault();
    h.log->info("request done",
                {{"method", "GET"}, {"path", "/users"}, {"status", "200"}});
    std::string out = h.text();
    EXPECT_NE(out.find("method=GET"), std::string::npos);
    EXPECT_NE(out.find("path=/users"), std::string::npos);
    EXPECT_NE(out.find("status=200"), std::string::npos);
}

TEST(LoggerTest, StructuredFieldsJson)
{
    auto h = makeJson();
    h.log->info("request done", {{"method", "GET"}, {"status", "200"}});
    std::string out = h.text();
    EXPECT_NE(out.find("\"method\":\"GET\""), std::string::npos);
    EXPECT_NE(out.find("\"status\":\"200\""), std::string::npos);
}

TEST(JsonFormatterTest, EscapesSpecialCharacters)
{
    JsonFormatter fmt;
    LogRecord rec{LogLevel::Info, Timestamp::now(), "1",
                  "quote\" back\\slash\new\nnewline", {}};
    std::string out = fmt.format(rec);
    EXPECT_NE(out.find("\\\""), std::string::npos);
    EXPECT_NE(out.find("\\\\"), std::string::npos);
    EXPECT_NE(out.find("\\n"), std::string::npos);
    EXPECT_EQ(out.find('\n'), std::string::npos);    // no raw newline
}

TEST(LoggerTest, LevelToString)
{
    EXPECT_STREQ(toString(LogLevel::Debug), "DEBUG");
    EXPECT_STREQ(toString(LogLevel::Info), "INFO");
    EXPECT_STREQ(toString(LogLevel::Warning), "WARNING");
    EXPECT_STREQ(toString(LogLevel::Error), "ERROR");
    EXPECT_STREQ(toString(LogLevel::Fatal), "FATAL");
}

// ---- Sinks ----

TEST(SinkTest, LoggerWritesOneLinePerRecord)
{
    auto h = makeDefault();
    h.log->info("a");
    h.log->info("b");
    h.log->info("c");
    EXPECT_EQ(h.sink->lines().size(), 3u);
}

TEST(SinkTest, FormatterEmitsNoTrailingNewLine)
{
    // The sink owns the line terminator, so formatters must not add one.
    auto h = makeDefault();
    h.log->info("hello");
    ASSERT_EQ(h.sink->lines().size(), 1u);
    EXPECT_EQ(h.sink->lines()[0].find('\n'), std::string::npos);
}

TEST(SinkTest, FatalFlushesImmediately)
{
    auto h = makeDefault();
    h.log->info("not flushed");
    size_t before = h.sink->flushCount();
    h.log->fatal("crash reason");
    EXPECT_GT(h.sink->flushCount(), before)
        << "Fatal must flush so the line survives a crash";
}

TEST(SinkTest, FlushLevelIsConfigurable)
{
    auto h = makeDefault();
    h.log->setFlushLevel(LogLevel::Error);
    size_t before = h.sink->flushCount();
    h.log->warning("no flush");
    EXPECT_EQ(h.sink->flushCount(), before);
    h.log->error("flush now");
    EXPECT_GT(h.sink->flushCount(), before);
}

TESt(SinkTest, SetSinkRedirectsOutput)
{
    auto h = makeDefault();
    h.log->info("to first");

    auto second = std::make_shared<MemorySink>();
    h.log->setSink(second);
    h.log->info("to second");

    EXPECT_NE(h.text().find("to first"), std::string::npos);
    EXPECT_EQ(h.text().find("to second"), std::string::npos);
    EXPECT_NE(second->text().find("to second"), std::string::npos);
}

TEST(SinkTest, FileSinkWritesLines)
{
    std::string path = "/tmp/cox_sink_test.log";
    std::remove(path.c_str());
    {
        auto sink = Sink::newFileSink(path, /*append_mode=*/false);
        Logger log(LogLevel::Info, Formatter::newDefaultFormatter(),
                   std::shared_ptr<Sink>(std::move(sink)));
        log.info("line one");
        log.info("line two");
    }   // dtor flushes and closes

    std::FILE* fp = std::fopen(path.c_str(), "re");
    ASSERT_NE(fp, nullptr);
    std::string content;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), fp)) content += buf;
    std::fclose(fp);
    std::remove(path.c_str());

    EXPECT_NE(content.find("line one"), std::string::npos);
    EXPECT_NE(content.find("line two"), std::string::npos);
    // One newline per record.
    EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), 2);
}

TEST(SinkTest, RotatingFileSinkRollsAndBoundsFileCount)
{
    std::string path = "/tmp/cox_rotate_test.log";
    for (int i = 0; i <= 5; ++i)
    {
        std::remove((i == 0 ? path : path + "." + std::to_string(i)).c_str());
    }
    {
        // Tiny cap so a few lines force several rotations.
        auto sink = Sink::newRotatingFileSink(path, /*max_bytes=*/80,
                                              /*max_files=*/2);
        Logger log(LogLevel::Info, Formatter::newDefaultFormatter(),
                   std::shared_ptr<Sink>(std::move(sink)));
        for (int i = 0; i < 30; ++i)
        {
            log.info("rotating line number " + std::to_string(i));
        }
    }
    
    auto exists = [](const std::string& p) {
        std::FILE* f = std::fopen(p.c_str(), "re");
        if (f) { std::fclose(f); return true; }
        return false;
    }

    EXPECT_TRUE(exists(path));            // an active file
    EXPECT_TRUE(exists(path + ".1"));     // rolled
    // max_files=2 means .3 must never appear, however many rotations occurred.
    EXPECT_FALSE(exists(path + ".3"));

    for (int i = 0; i <= 5; ++i)
    {
        std::remove((i == 0 ? path : path + "." + std::to_string(i)).c_str());
    }
}

TEST(SinkTest, MultiSinkFansout)
{
    auto a = std::make_shared<MemorySink>();
    auto b = std::make_shared<MemorySink>();
    std::vector<std::unique_ptr<Sink>> subs;
    // Wrap the shared sinks so the MultiSink does not own them.
    struct Forward final : Sink {
        std::shared_ptr<MemorySink> to;
        explicit Forward(std::shared_ptr<MemorySink> t) : to(std::move(t)) {}
        void append(const std::string& l) override { to->append(l); }
        void flush() override { to->flush(); }
    };
    subs.push_back(std::make_unique<Forward>(a));
    subs.push_back(std::make_unique<Forward>(b));

    Logger log(LogLevel::Info, Formatter::newDefaultFormatter(),
               std::shared_ptr<Sink>(Sink::newMultiSink(std::move(subs))));
    log.info("both");

    EXPECT_NE(a->text().find("both"), std::string::npos);
    EXPECT_NE(b->text().find("both"), std::string::npos);
}

// ---- AsyncSink ----

TEST(AsyncSinkTest, DeliversAllLinesToDownstream)
{
    auto captured = std::make_shared<MemorySink>();
    struct Forward final : Sink {
        std::shared_ptr<MemorySink> to;
        explicit Forward(std::shared_ptr<MemorySink> t) : to(std::move(t)) {}
        void append(const std::string& l) override { to->append(l); }
        void flush() override { to->flush(); }
    };

    constexpr int kLines = 500;
    {
        auto async = std::make_shared<AsyncSink>(
            std::make_unique<Forward>(captured), /*max_buffered_lines=*/4096);
        Logger log(LogLevel::Info, Formatter::newDefaultFormatter(), async);
        for (int i = 0; i < kLines; ++i) log.info("msg " + std::to_string(i));
        async->flush();
        EXPECT_EQ(async->droppedCount(), 0u);
    }   // stop() drains the remainder

    EXPECT_EQ(captured->lines().size(), static_cast<size_t>(kLines));
    EXPECT_NE(captured->text().find("msg 0"), std::string::npos);
    EXPECT_NE(captured->text().find("msg 499"), std::string::npos);
}

TEST(AsyncSinkTest, DropsAndCountsWhenBufferFull)
{
    // A downstream that stalls, so the bounded buffer necessarily overflows.
    struct SlowSink final : Sink {
        void append(const std::string&) override {
            std::this_thread::sleep_for(2ms);
        }
        void flush() override {}
    };

    auto async = std::make_unique<AsyncSink>(std::make_unique<SlowSink>(),
                                             /*max_buffered_lines=*/8);
    for (int i = 0; i < 2000; ++i)
    {
        async->append("line " + std::to_string(i));
    }
    // The point: a slow/blocked destination must never block the producer, and
    // the shed lines must be accounted for rather than silently lost.
    EXPECt_GT(async->droppedCount(), 0u);
    async->stop();
}

TEST(AsyncSinkTest, ProducerDoesNotBlockOnSlowSink)
{
    struct SlowSink final : Sink {
        void append(const std::string&) override {
            std::this_thread::sleep_for(5ms);
        }
        void flush() override {}
    };

    auto async = std::make_unique<AsyncSink>(std::make_unique<SlowSink>(), 4096);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i)
    {
        async->append("x");
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    
    // 200 synchronous 5ms writes would be ~1s; the async path must be far
    // faster since it only appends to a buffer.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                  .count(),
              300);
    async->stop();
}

TEST(AsyncSinkTest, StopDrainsPendingLines)
{
    auto captured = std::make_shared<MemorySink>();
    struct Forward final : Sink {
        std::shared_ptr<MemorySink> to;
        explicit Forward(std::shared_ptr<MemorySink> t) : to(std::move(t)) {}
        void append(const std::string& l) override { to->append(l); }
        void flush() override { to->flush(); }
    };

    auto async = std::make_shared<AsyncSink>(
        std::make_unique<Forward>(captured), 4096, /*flush_interval_ms=*/10000);
    for (int i = 0; i < 50; ++i) async->append("pending " + std::to_string(i));
    async->stop();   // must not lose anything still queued.

    EXPECT_EQ(captured->lines().size(), 50u);
}
