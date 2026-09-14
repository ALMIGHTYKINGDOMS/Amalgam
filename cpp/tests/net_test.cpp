#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Minimal loopback HTTP server for download-policy tests. Each serve_once()
// accepts exactly one connection, hands the request to the handler, and sends
// the handler's response back before closing.
struct HttpTestServer {
    SOCKET listener = INVALID_SOCKET;

    explicit HttpTestServer(unsigned short port) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            WSACleanup();
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        int opt = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt),
                   sizeof(opt));
        if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(listener, 1) != 0) {
            closesocket(listener);
            listener = INVALID_SOCKET;
            WSACleanup();
        }
    }
    ~HttpTestServer() { close_listener(); }

    bool ok() const { return listener != INVALID_SOCKET; }

    using Handler = std::function<bool(const std::string& request, std::string* response)>;

    bool serve_once(const Handler& handler) {
        SOCKET conn = accept(listener, nullptr, nullptr);
        if (conn == INVALID_SOCKET) return false;
        std::string request;
        char buf[4096];
        for (;;) {
            int got = recv(conn, buf, sizeof(buf), 0);
            if (got <= 0) break;
            request.append(buf, static_cast<size_t>(got));
            if (request.find("\r\n\r\n") != std::string::npos) break;
        }
        std::string response;
        const bool handled = handler(request, &response);
        if (handled && !response.empty())
            send(conn, response.data(), static_cast<int>(response.size()), 0);
        shutdown(conn, SD_BOTH);
        closesocket(conn);
        return handled;
    }

    // Unblocks a thread parked in accept() so a failed test cannot hang the run.
    void close_listener() {
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
            listener = INVALID_SOCKET;
            WSACleanup();
        }
    }
};

bool test_sha1() {
    // SHA-1("abc") is a well-known vector.
    const char* abc = "abc";
    std::string hash = aml::net::sha1_hex(abc, 3);
    return hash == "a9993e364706816aba3e25717850c26c9cd0d89d";
}

bool test_sha256() {
    const char* abc = "abc";
    std::string hash = aml::net::sha256_hex(abc, 3);
    return hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
}

bool test_sha1_file_roundtrip() {
    wchar_t buffer[MAX_PATH]{};
    DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) return false;
    std::wstring dir(buffer, length);
    std::wstring path = dir + L"\\aml_net_test.bin";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "abc";
    }
    std::string hash = aml::net::sha1_file(path);
    DeleteFileW(path.c_str());
    // SHA-1("abc")
    return hash == "a9993e364706816aba3e25717850c26c9cd0d89d";
}

bool test_verify_file() {
    wchar_t buffer[MAX_PATH]{};
    DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) return false;
    std::wstring dir(buffer, length);
    std::wstring path = dir + L"\\aml_net_verify.bin";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "verify me";
    }
    int64_t size = static_cast<int64_t>(aml::net::file_size(path));
    std::string sha1 = aml::net::sha1_file(path);
    bool ok = aml::net::verify_file(path, sha1, size) &&
              !aml::net::verify_file(path, sha1, size + 1) &&
              !aml::net::verify_file(path, "0000000000000000000000000000000000000000", size) &&
              !aml::net::verify_file(path, "", size + 1234);
    DeleteFileW(path.c_str());
    return ok;
}

bool test_url_policy() {
    std::string err;
    // Only HTTPS is accepted before any network access.
    std::vector<uint8_t> out;
    if (aml::net::get(L"http://example.com/x", out, &err)) return false;
    if (aml::net::get(L"ftp://example.com/x", out, &err)) return false;
    if (aml::net::get(L"file:///C:/Windows/win.ini", out, &err)) return false;
    // Embedded credentials are rejected.
    err.clear();
    if (aml::net::download(L"http://example.com/x.jar", L"x.jar", nullptr, &err)) return false;
    if (aml::net::download(L"http://user:pass@example.com/x.jar", L"x.jar", nullptr, &err))
        return false;
    if (aml::net::post(L"http://example.com/upload", {}, {}, out, &err)) return false;
    if (aml::net::post(L"ftp://example.com/upload", {}, {}, out, &err)) return false;
    if (aml::net::post(L"http://user:pass@example.com/upload", {}, {}, out, &err)) return false;
    return true;
}

bool test_loopback_http_policy() {
    std::string err;
    std::vector<uint8_t> out;

    // Without the opt-in the loopback-HTTP exception does not exist.
    SetEnvironmentVariableW(L"AMALGAM_TEST_LOOPBACK_HTTP", nullptr);
    if (aml::net::get(L"http://127.0.0.1:9/x", out, &err)) return false;
    if (err.find("https") == std::string::npos) return false;

    // With the opt-in, loopback HTTP passes URL policy (the failure below is a
    // connection refusal, not a policy rejection) ...
    SetEnvironmentVariableW(L"AMALGAM_TEST_LOOPBACK_HTTP", L"1");
    if (aml::net::get(L"http://127.0.0.1:9/x", out, &err)) return false;
    if (err.find("https") != std::string::npos) return false;
    // ... but the exception never reaches public hosts.
    if (aml::net::get(L"http://example.com/x", out, &err)) return false;
    if (aml::net::download(L"http://example.com/x.jar", L"x.jar", nullptr, &err)) return false;
    if (aml::net::post(L"http://example.com/upload", {}, {}, out, &err)) return false;

    SetEnvironmentVariableW(L"AMALGAM_TEST_LOOPBACK_HTTP", nullptr);
    return true;
}

// A .part file already at the payload size (left by a killed run) makes the
// resume request fail with HTTP 416. The downloader must discard the stale
// partial file and complete the download from scratch instead of failing the
// launch forever; size and SHA-1 still gate the final artifact.
bool test_download_discards_poisoned_416_part() {
    wchar_t tmp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tmp) == 0) return false;
    const std::wstring path = std::wstring(tmp) + L"aml_416_test.bin";
    const std::wstring part = path + L".part";
    DeleteFileW(path.c_str());
    DeleteFileW(part.c_str());

    const std::string body = "0123456789abcdef";
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        f << "STALE BYTES THAT MUST BE DISCARDED";
    }

    HttpTestServer server(18081);
    if (!server.ok()) return false;

    std::atomic<int> connections{0};
    std::atomic<bool> handler_failed{false};
    std::atomic<bool> stop{false};
    std::string resume_request;

    auto full_body = [&body](std::string* response) {
        *response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
    };

    std::thread acceptor([&]() {
        // Connection 1: the poisoned resume request -> 416.
        ++connections;
        const bool ok1 = server.serve_once([&](const std::string& request, std::string* response) {
            resume_request = request;
            if (request.find("Range: bytes=") == std::string::npos) return false;
            *response =
                "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            return true;
        });
        if (!ok1) handler_failed = true;
        if (stop) return;
        // Connection 2 (attempt 2, after the discard): no resume header, full body.
        ++connections;
        const bool ok2 = server.serve_once([&](const std::string& request, std::string* response) {
            if (request.find("Range:") != std::string::npos) return false;
            full_body(response);
            return true;
        });
        if (!ok2) handler_failed = true;
    });

    SetEnvironmentVariableW(L"AMALGAM_TEST_LOOPBACK_HTTP", L"1");
    std::string err;
    const std::string sha1 = aml::net::sha1_hex(body.data(), body.size());
    const bool ok = aml::net::download(L"http://127.0.0.1:18081/payload.bin", path, nullptr, &err,
                                       sha1, static_cast<int64_t>(body.size()));
    SetEnvironmentVariableW(L"AMALGAM_TEST_LOOPBACK_HTTP", nullptr);
    stop = true;
    server.close_listener();
    acceptor.join();

    if (!ok) {
        std::cerr << "download failed: " << err << "\n";
        return false;
    }
    if (handler_failed || connections != 2) {
        std::cerr << "unexpected connections=" << connections
                  << " handler_failed=" << handler_failed.load() << "\n";
        return false;
    }
    if (resume_request.find("Range: bytes=") == std::string::npos) {
        std::cerr << "first attempt did not try to resume\n";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    DeleteFileW(path.c_str());
    return got == body;  // stale bytes discarded, real payload landed
}

}  // namespace

int main() {
    struct {
        const char* name;
        bool (*fn)();
    } tests[] = {{"sha1", test_sha1},
                 {"sha256", test_sha256},
                 {"sha1_file", test_sha1_file_roundtrip},
                 {"verify_file", test_verify_file},
                 {"url_policy", test_url_policy},
                 {"loopback_http_policy", test_loopback_http_policy},
                 {"download_416_recovery", test_download_discards_poisoned_416_part}};
    bool ok = true;
    for (const auto& t : tests) {
        if (!t.fn()) {
            std::cerr << "FAILED: " << t.name << "\n";
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
