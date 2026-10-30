// A minimal REST server example built on Cox's HTTP layer + Router
//
// Usage: rest_server [port]    (default 8080)
// Try:
//    curl http://127.0.0.1:8080/health
//    curl http://127.0.0.1:8080/users/42
//    'curl http://127.0.0.1:8080/search?q=hello&page=2'
//    curl -X POST http://127.0.0.1:8080/form -d 'name=Ada+Lovelace'
//    curl -X POST http://127.0.0.1:8080/echo -d 'hello'

#include "event_loop.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"
#include "inet_address.h"
#include "router.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace http;

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) {
    g_stop = 1;
}
}  // namespace

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    reactor::EventLoop loop;
    HttpServer server(&loop, net::InetAddress(port), "rest");

    Router router;
    router.get("/health",
               [](const HttpRequest&, const PathParams&, HttpResponse* resp) {
                   resp->setStatus(200);
                   resp->setContentType("application/json");
                   resp->setBody(R"({"status":"ok"})");
               });
    router.get("/users/{id}",
               [](const HttpRequest&, const PathParams& p, HttpResponse* resp) {
                   resp->setStatus(200);
                   resp->setContentType("application/json");
                   // NOTE: path params are echoed unescaped; fine for a demo,
                   // a real service must JSON-escape untrusted input.
                   resp->setBody(R"({"id":")" + p.at("id") +
                                 R"(","name":"user)" + p.at("id") + R"("})");
               });
    // Query-string parsing: /search?q=hello+world&page=2
    router.get("/search", [](const HttpRequest& req, const PathParams&,
                             HttpResponse* resp) {
        std::string q = req.queryParam("q");
        std::string page =
            req.hasQueryParam("page") ? req.queryParam("page") : "1";
        resp->setStatus(200);
        resp->setContentType("application/json");
        resp->setBody(R"({"q":")" + q + R"(","page":")" + page + R"("})");
    });
    // Form body parsing: -d 'name=Ada+Lovelace'
    router.post("/form", [](const HttpRequest& req, const PathParams&,
                            HttpResponse* resp) {
        auto form = req.formParams();
        auto it = form.find("name");
        resp->setStatus(200);
        resp->setContentType("text/plain");
        resp->setBody(it == form.end() ? "no name" : "hello " + it->second);
    });
    router.post("/echo", [](const HttpRequest& req, const PathParams&,
                            HttpResponse* resp) {
        resp->setStatus(200);
        resp->setContentType("text/plain");
        resp->setBody(req.body());
    });
    server.setHttpHandler(
        [&router](const HttpRequest& req, HttpResponse* resp) {
            router.route(req, resp);
        });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "REST server on :" << port << " (Ctrl-C to stop)" << std::endl;
    server.start();

    loop.runEvery(200, [&loop] {
        if (g_stop) {
            loop.quit();
        }
    });
    loop.loop();
    std::cout << "stopped" << std::endl;
    return 0;
}
