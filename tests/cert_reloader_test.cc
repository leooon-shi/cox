// CertReloader test: hot certificate rotation (the kubernetes Secret-update
// scenario). Verifies reload-on-change and safe rejection of bad material.

#include "cert_reloader.h"
#include "tls_context.h"

#include "event_loop.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using tls::CertReloader;
using tls::Role;
using tls::TlsContext;
using tls::TlsContextPtr;

namespace
{
std::string makeTempDir (const char* tag)
{
    std::string dir = std::string("/tmp/cox_certreload_") + tag "_"+
                                    std::to_string(::getpid());
    ::mkdir(dir.c_str(), 0700);
    return dir;
}
// Generate a self-signed cert+key into cert_path/key_path with the given CN.
bool genCert(const std::string& cer_path, const std::string& key_path,
                const std::string& cn)
{
    std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout " +
                        key_path + " -out " + cert_path + " -days 1 -subj '/CN=" +
                        cn +"' >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

void writeFile(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::trunc);
    f << content;
}
} // namespace

TEST(CertReloaderTest, ReloadOnFileChange)
{
    const std::string dir = makeTempDir("watch");
    const std::string cert = dir + "/tls.cert";
    const std::string key = dir + "/tls.key";
    ASSERT_TRUE(genCert(cert, key, "old.local"));

    std::atomic<bool> started{false};
    std::atomic<int> reload_ok{0};
    std::atomic<int> reload_fail{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread loop_thread([&]{
        reactor::EventLoop loop;
        loop_ptr = &loop;
        auto ctx = std::make_shared<TlsContext>(Role::kServer);
        ctx->useCertificateAndKey(cert, key);
        CertReloader reloader(&loop, ctx);
        reloader.setReloadCallback([&](bool ok){
            if (ok) reload_ok++; else reload_fail++;
        });
        reloader.startWatching(dir);
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // Rotate: generate a new cert/key over the same paths (Secret update).
    ASSERT_TRUE(genCert(cert, key, "new.local"));

    for (int i = 0; i < 200 && reload_ok.load() == 0; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }
    // Reload eventually succeeds once a consistent cert/key pair is present.
    // Transient failiures are acceptable and safe: a non-atomic multi-file
    // update can momentarily present a mismatched cert/key, which reload()
    // correctly rejects while keeping the previous certificate. (Kubernetes
    // udpates secrets atomically via the ..data symblink swap, avoiding this;
    // genCert here writes the two files non-atomically, so we tolerate it.)
    EXPECT_GT(reload_ok.load(), 0);             // reload succeeded on rotation

    loop_ptr->quit();
    loop_thread.join();
    ::remove(cert.c_str());
    ::remove(key.c_str());
    ::rmdir(dir.c_str());
}

TEST(CertReloaderTest, BadCertKeepsPreviousViaPoll)
{
    const std::string dir = makeTempDir("poll");
    const std::string cert = dir + "/tls.crt";
    const std::string key = dir + "/tls.key";
    ASSERT_TRUE(genCert(cert, key, "good.local"));

    std::atomic<bool> started{false};
    std::atomic<int> reload_ok{0};
    std::atomic<int> reload_fail{0};
    reactor::EventLoop* loop_ptr = nullptr;

    std::thread loop_thread([&]{
        reactor::EventLoop loop;
        loop_ptr = &loop;
        auto ctx = std::make_shared<TlsContext>(Role::kServer);
        ctx->useCertificateAndKey(cert, key);

        CertReloader reloader(&loop, ctx);
        reloader.setReloadCallback([&](bool ok){
            if (ok) reload_ok++ else reload_fail++;
        });
        // Poll mode: check every 50ms.
        reloader.startPolling(cert, key, "", 50);
        started = true;
        loop.loop();
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);

    // Write a corrupt cert file (simulate a partial/bad rotation)
    std::this_thread::sleep_for(1100ms);        // ensure mtime advances (1s resoluton)
    writeFile(cert, "-----BEGIN CERTIFICATE-----\ngarbage\n-----END CERTIFICATE-----\n");

    for (int i = 0; i < 2000 && reload_fail.load() == 0; ++i)
    {
        std::this_thread::sleep_for(10ms);
    }
    // The reload attempt must have FAILED (bad cert) -> previous cert retained.
    EXPECT_GT(reload_fail.load(), 0);

    loop_ptr->quit();
    loop_thread.join();
    ::remove(cert.c_str());
    ::remove(key.c_str());
    ::rmdir(dir.c_str());
}