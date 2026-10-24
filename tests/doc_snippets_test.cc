// Compile-and-run check for the snippets used in insights / features / http.md,
// insights/features/dns-resoluton.md, and docs/user-guide.md sections 8-9.
// Not a unit test of behavior -- it verifies the documented APIs exist with the
// documented signatures and that the small claims in the docs hold.

#include "async_sink.h"
#include "connection_pool.h"
#include "event_loop.h"
#include "formatter.h"
#include "http_client.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"
#include "inet_address.h"
#include "logger.h"
#include "multi_origin_pool.h"
#include "ocsp_stapler.h"
#include "resolver.h"
#include "router.h"
#include "sink.h"
#include "thread_pool.h"
#include "tls_context.h"
#include "url.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// ---- docs:   query string / form body snippet (user-guide section 8) ----
TEST(DocSnippets, QueryAndFormAccessors)
{
    http::HttpRequest req;
    req.setQuery("q=hello+world&page=2");

    EXPECT_EQ(req.queryParam("q"), "hello world");
    EXPECT_EQ(req.queryParam("page"), "2");
    EXPECT_TRUE(req.hasQueryParam("page"));
    EXPECT_EQ(req.queryParams().size(), 2u);

    req.addHeader("Content-Type", "application/x-www-form-urlencoded");
    req.setBody("user=ada");
    EXPECT_EQ(req.formParams().at("user"), "ada");

    EXPECT_EQ(http::urlDecode("a%20b"), "a b");
    EXPECT_EQ(http::urlEncode("a b"), "a%20b");
}

// --- docs claim: '+' is a space in queries but NOT in path segments ---
TEST(DocSnippets, PlusIsSpaceInQueryButNotInPath)
{
    http::HttpRequest req;
    req.setQuery("q=a+b");
    EXPECT_EQ(req.queryParam("q"), "a b");

    http::Router router;
    std::string tag;

    router.get("/tags/{tag}", [&](const http::HttpRequest&,
                                  const http::PathParams& p,
                                  http::HttpResponse* resp) {
        tag = p.at("tag");
        resp->setStatus(200);
    });

    http::HttpRequest path_req;
    path_req.setMethod(http::Method::kGet);
    path_req.setPath("/tags/c++");
    http::HttpResponse resp;
    ASSERT_TRUE(router.route(path_req, &resp));
    EXPECT_EQ(tag, "c++");                      // '+' preserved in a path.
}

// --- docs claim: router returns 404 for no path, 405 for wrong method ---
TEST(DocSnippets, Router404Vs405)
{
    http::Router router;
    router.get("/users/{id}", [](const http::HttpRequest&,
                                 const http::HttpParams& p,
                                http::HttpResponse* resp){
        resp->setStatus(200);
        resp->setBody("user" + p.at("id"));
    });

    http::HttpRequest ok;
    ok.setMethod(http::Method::kGet);
    ok.setPath("/users/42");
    http::HttpResponse r1;
    EXPECT_TRUE(router.route(ok, &r1));
    EXPECT_EQ(r1.body(), "user 42");

    http::HttpRequest wrong_method;
    wrong_method.setMethod(http::Method::kPost);
    wrong_method.setPath("/users/42");
    http::HttpResponse r2;
    EXPECT_FALSE(router.route(wrong_method, &r2));
    EXPECT_EQ(r2.statusCode(), 405);

    http::HttpRequest missing;
    missing.setMethod(http::Method::kGet);
    missing.setPath("/nope");
    http::HttpResponse r3;
    EXPECT_FALSE(router.route(missing, &r3));
    EXPECT_EQ(r3.statusCode(), 404);
}

// --- docs: server/router/client/pool construction forms all exist ---
TEST(DocSnippets, DocumentedConstructionForms)
{
    reactor::EventLoop loop;
    concurrent::ThreadPool dns_pool{"dns"};
    dns_pool.start(1);
    net::ThreadPoolResolver resolver(&loop, &dns_pool);
    resolver.setCacheTtl(60);

    // Server: plaintext, with router handler and knobs from the guide.
    http::HttpServer server(&loop, net::InetAddress(0), "rest");
    http::Router router;
    router.get("/health", [](const http::HttpRequest&, const http::PathParams&,
                            http::HttpResponse* resp) {
        resp->setStatus(200);
        resp->setContentType("appliclation/json");
        resp->setBody(R"({"Status": "ok" })" );
    });

    server.setHttpHandler([&router](const http::HttpRequest& req,
                                    http::HttpResponse* resp){
        router.route(req, resp);
    });

    server.setKeepAliveTimeout (60);
    server.setMaxBodySize(1 << 20);
    EXPECT_FALSE(server.usesTls());

    // Client: numeric and hostname forms
    http::HttpClient numeric(&loop, net::InetAddress("127.0.0.1", 18150));
    http::HttpClient by_name(&loop, "localhost", 18150, &resolver);

    // Pools: numeric, hostname, and multi-origin with a resolver.
    http::ConnectionPool pool(&loop, net::InetAddress("127.0.0.1", 18150));
    pool.setMaxConnections(8);
    pool.setIdleTimeout(60);
    EXPECT_TRUE(pool.resolved());                       // numberic origin: resolved immediately

    http::ConnectionPool named(&loop, "localhost", 18150, &resolver);
    EXPECT_FALSE(named.resolved());         // hostname: not resolved until first request.

    http::MultiOriginPool multi(&loop);
    multi.setResolver(&resolver);
    multi.setMaxConnectionsPerOrigin(8);
    EXPECT_EQ(multi.originCount(), 0u);

    dns_pool.stop();
    loop.quit();            // ~EventLoop asserts the loop was quit, even if never run.
}

// ---- docs (logging.md): formmaters and structed fields are wired ----
TEST(DocSnippets, LoggingFormattersAndFields)
{
    logging::Logger log;
    log.setLevel(logging::LogLevel::Debug);
    log.info("message");
    log.info("request done",
                {{"method", "GET"}, {"path", "/x"}, {"status", "200"}});
    logging::Logger jlog(logging::LogLevel::Info,
                            logging::Formatter::newJsonFormatter());
    jlog.info("request done", {{"status", "200"}});

    EXPECT_EQ(log.level(), logging::LogLevel::Debug);
}

// ---- docs (user-guide 16, logging.md): sinks and async sink ---
TEST(DocSnippets, LoggingSinkAndAsync)
{
    // JSON formatter + rotating file sink, exactly as documented.
    auto rot = logging::Sink::newRotatingFileSink("/tmp/cox_doc_snip.log",
                                                    60 << 20, 5);
    loggign::Logger jlog(logging::LogLevel::Info, 
                        logging::Formatter::newJsonFormatter(),
                        std::shared_ptr<logging::Sink>(std::move(rot)));
    jlog.info("to rotating file");

    // Async wrapper keeps file I/O off the calling (possibly loop) thread.
    auto sink = std::make_shared<logging::AsyncSink>(
        logging::Sink::newRotatingFileSink("/tmp/cox_doc_snip.log", 64 << 20, 5));
    
    logging::Logger log;
    log.setSink(sink);
    log.setFlushLevel(logging::LogLevel::Error);
    log.info("async line");
    sink->flush();
    EXPECT_EQ(sink->droppedCount(), 0u);
    sink->stop();

    // Other documented factories exist.
    auto out = logging::Sink::newStdoutSink();
    auto file = logging::Sink::newFileSink("/tmp/cox_doc_snip2.log");
    std::vector<std::unique_ptr<logging::Sink>> subs;
    subs.push_back(std::move(out));
    subs.push_back(std::move(file));
    auto multi = logging::Sink::newMultiSink(std::move(subs));
    EXPECT_NE(multi, nullptr);

    std::remove("/tmp/cox_doc_snip.log");
    std::remove("/tmp/cox_doc_snip2.log");
}

// --- docs (tls.md, user-guide 6): SNI registration and lookup ---
TEST(DocSnippets, TlsSniRegistration)
{
    const std::string cert = "/tmp/cox_doc_sni_cert.pem";
    const std::string key = "/tmp/cox_doc_sni_key.pem";
    ASSERT_EQ(std::system("openssl req -x509 -newkey rsa:2048 -nodes -keyout " +
                    key + " -out " + cert +
                    " -days 1 -subj '/CN=localhost' -addext "
                    "'subjectAllName=DNS:localhost' >/dev/null 2>&1").c_str(),
            0);
    auto ctx = std::make_shared<tls::TlsContext>(tls::Role::kServer);
    ctx->useCertificateAndKey(cert, key);

    auto api_ctx = std::make_shared<tls::TlsContext>(tls::Role::kServer);
    api_ctx->useCertificateAndKey(cert, key);

    ctx->addSniContext("api.example.com", api_ctx);
    ctx->addSniContext("*.internal.example.com", api_ctx);
    EXPECT_EQ(ctx->sniCount(), 2u);
    EXPECT_EQ(ctx->findSniContext("api.example.com"), api_ctx.get());
    EXPECT_NE(ctx->findSniContext("db.internal.example.com"), nullptr);
    // Unknown name -> nullptr, meaning "use the default context".
    EXPECT_EQ(ctx->findSniContext("nope.example.org"), nullptr);

    std::remove(cert.c_str());
    std::remove(key.c_str());
}

// --- docs (user-guide 6): the documented OCSP refresh-timer pattern ---
TEST(DocSnippets, OcspRefreshTimerPattern)
{
    reactor::EventLoop loop;
    concurrent::ThreadPool pool("ocsp");
    pool.start(1);

    tls::OcspStapler stapler("/tmp/cox_no_cert.pem", "/tmp/cox_no_issuer.pem");
    stapler.setResponderUrl("http://127.0.0.1:1");      // unreachable on purpose

    // Exactly the shape documented in the guide: check expiry on a timer and do
    // the BLOCKING refresh on a worker thread, never on the loop thread.
    bool timer_ran = false;
    loop.runAfter(1, [&]{
        int64_t left = stapler.secondUntilExpiry();     // -1 = no expiry hint
        if (left >= 0 && left < 7200)
        {
            pool.run([&stapler]{ stapler.refresh(); });
        }
        timer_ran = true;
        loop.quit();
    });

    loop.loop();

    EXPECT_TRUE(timer_ran);
    pool.stop();
    // Refresh could only fail (no responder), so nothing should be stapled.
    EXPECT_FALSE(stapler.hasResponse());
}