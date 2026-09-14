#include "net.h"

#include <windows.h>
#include <wininet.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include <wincrypt.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "advapi32.lib")

namespace aml::net {

namespace {

constexpr int kConnectTimeoutMs = 30000;
constexpr int kReceiveTimeoutMs = 60000;
constexpr int kMaxAttempts = 3;

// A 4xx response is a permanent client error (bad key, missing content,
// unauthorized) — retrying cannot fix it, so we stop immediately instead of
// burning the retry budget. Transient failures (5xx, 429, timeouts, network
// drops) are retried with a short backoff delay.
bool retryable_error(const std::string& err) {
    if (err.find("http status 4") != std::string::npos) return false;
    if (err.find("http status 401") != std::string::npos) return false;
    if (err.find("http status 403") != std::string::npos) return false;
    if (err.find("http status 404") != std::string::npos) return false;
    if (err.find("http status 400") != std::string::npos) return false;
    return true;
}

// Short exponential-ish backoff between retry attempts, so a busy provider
// or rate limit gets a moment to recover instead of being hammered 3x in a row.
void retry_backoff(int attempt) {
    Sleep(static_cast<DWORD>(200u * (1u << static_cast<unsigned>(attempt))));
}
constexpr uint64_t kMaxResponseBytes = 64ull * 1024 * 1024;
constexpr uint64_t kDefaultMaxDownloadBytes = 2ull * 1024 * 1024 * 1024;
constexpr uint64_t kAbsoluteMaxDownloadBytes = 32ull * 1024 * 1024 * 1024;

bool is_loopback_host(const URL_COMPONENTSW& comp) {
    if (!comp.lpszHostName || comp.dwHostNameLength == 0) return false;
    const std::wstring host(comp.lpszHostName, comp.dwHostNameLength);
    return _wcsicmp(host.c_str(), L"localhost") == 0 ||
           _wcsicmp(host.c_str(), L"127.0.0.1") == 0 ||
           _wcsicmp(host.c_str(), L"::1") == 0 ||
           _wcsicmp(host.c_str(), L"[::1]") == 0;
}

bool url_allowed(const std::wstring& url, std::string* err) {
    URL_COMPONENTSW comp{};
    comp.dwStructSize = sizeof(comp);
    // Crack fills only the components whose buffer+length are provided; without
    // a host buffer dwHostNameLength stays 0 and the loopback check below
    // could never match.
    wchar_t host[256] = {};
    comp.lpszHostName = host;
    comp.dwHostNameLength = 256;
    if (!InternetCrackUrlW(url.c_str(), 0, 0, &comp)) {
        if (err) *err = "bad url";
        return false;
    }
    // The launcher installs executable Java archives and game assets.  Do not
    // permit an accidental downgrade to clear-text transport anywhere in the
    // shared network layer, even if a future caller forgets to validate it.
    //
    // AMALGAM_TEST_LOOPBACK_HTTP is the one documented exception: integration
    // tests point the updater at a localhost HTTP server serving a signed feed.
    // It accepts loopback addresses only; every public host still requires HTTPS.
    if (comp.nScheme == INTERNET_SCHEME_HTTP && is_loopback_host(comp)) {
        wchar_t flag[4] = {};
        if (GetEnvironmentVariableW(L"AMALGAM_TEST_LOOPBACK_HTTP", flag, 4) > 0 &&
            flag[0] != L'\0' && flag[0] != L'0') {
            return true;
        }
    }
    if (comp.nScheme != INTERNET_SCHEME_HTTPS) {
        if (err) *err = "only https urls are allowed";
        return false;
    }
    if (comp.lpszUserName && comp.lpszUserName[0]) {
        if (err) *err = "urls with embedded credentials are rejected";
        return false;
    }
    return true;
}

void apply_timeouts(HINTERNET h) {
    DWORD connect = kConnectTimeoutMs;
    DWORD receive = kReceiveTimeoutMs;
    InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &connect, sizeof(connect));
    InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &receive, sizeof(receive));
    InternetSetOptionW(h, INTERNET_OPTION_SEND_TIMEOUT, &receive, sizeof(receive));
}

HINTERNET open_internet() {
    static HINTERNET handle = InternetOpenW(L"AmalgamLauncher", INTERNET_OPEN_TYPE_PRECONFIG,
                                            nullptr, nullptr, 0);
    return handle;
}

// Forward declaration so open_url can read response bodies on HTTP errors.
bool read_all(HINTERNET h, std::vector<uint8_t>& out, std::string* err, uint64_t cap);

bool status_ok(HINTERNET h, std::string* err) {
    DWORD code = 0;
    DWORD len = sizeof(code);
    if (!HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len,
                        nullptr)) {
        if (err) *err = "http query failed";
        return false;
    }
    if (code < 200 || code >= 300) {
        if (err) *err = "http status " + std::to_string(code);
        return false;
    }
    return true;
}

HINTERNET open_url(const std::wstring& url, const std::vector<std::wstring>& headers,
                   std::string* err) {
    if (!url_allowed(url, err)) return nullptr;
    std::wstring hdr;
    if (!headers.empty()) {
        for (const auto& h : headers) {
            hdr += h;
            hdr += L"\r\n";
        }
    }
    // INTERNET_FLAG_SECURE on a plain-HTTP handle makes WinINET fail the
    // request; the loopback test seam serves HTTP, so mirror the policy check.
    const DWORD open_flags =
        INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES |
        INTERNET_FLAG_PRAGMA_NOCACHE |
        (_wcsnicmp(url.c_str(), L"https://", 8) == 0 ? INTERNET_FLAG_SECURE : 0);
    HINTERNET h = InternetOpenUrlW(open_internet(), url.c_str(), hdr.empty() ? nullptr : hdr.c_str(),
                                   static_cast<DWORD>(hdr.size()), open_flags,
                                   reinterpret_cast<DWORD_PTR>(nullptr));
    if (!h) {
        if (err) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "internet open failed, err=%lu", GetLastError());
            *err = buf;
        }
        return nullptr;
    }
    DWORD final_len = 0;
    if (!InternetQueryOptionW(h, INTERNET_OPTION_URL, nullptr, &final_len) &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER && final_len > 0) {
        std::wstring final_url(final_len / sizeof(wchar_t) + 1, L'\0');
        if (InternetQueryOptionW(h, INTERNET_OPTION_URL, final_url.data(), &final_len)) {
            final_url.resize(wcslen(final_url.c_str()));
            if (!url_allowed(final_url, err)) {
                InternetCloseHandle(h);
                return nullptr;
            }
        }
    }
    apply_timeouts(h);
    if (!status_ok(h, err)) {
        // Read the response body on HTTP errors so the provider's actual error
        // message (e.g. CurseForge "API key not valid") is surfaced to the user.
        std::vector<uint8_t> err_body;
        read_all(h, err_body, nullptr, 4096);
        if (!err_body.empty() && err) {
            std::string body_text(err_body.begin(), err_body.end());
            *err += " | " + body_text;
        }
        InternetCloseHandle(h);
        return nullptr;
    }
    return h;
}

bool read_all(HINTERNET h, std::vector<uint8_t>& out, std::string* err, uint64_t cap) {
    std::vector<uint8_t> buf(65536);
    for (;;) {
        DWORD read = 0;
        if (!InternetReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read)) {
            if (err) *err = "read failed";
            return false;
        }
        if (read == 0) break;
        if (out.size() + read > cap) {
            if (err) *err = "response exceeds size limit";
            return false;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + read);
    }
    return true;
}

}  // namespace

bool validate_url(const std::wstring& url, std::string* err) {
    return url_allowed(url, err);
}

bool get(const std::wstring& url, std::vector<uint8_t>& out, std::string* err) {
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        out.clear();
        std::string attempt_err;
        HINTERNET h = open_url(url, {}, &attempt_err);
        if (!h) {
            if (err) *err = attempt_err;
            if (!retryable_error(attempt_err)) return false;
            if (attempt + 1 < kMaxAttempts) retry_backoff(attempt);
            continue;
        }
        bool ok = read_all(h, out, &attempt_err, kMaxResponseBytes);
        InternetCloseHandle(h);
        if (ok) return true;
        if (err) *err = attempt_err;
        out.clear();
        if (!retryable_error(attempt_err)) return false;
        if (attempt + 1 < kMaxAttempts) retry_backoff(attempt);
    }
    return false;
}

bool get_with_headers(const std::wstring& url, const std::vector<std::wstring>& headers,
                      std::vector<uint8_t>& out, std::string* err) {
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        out.clear();
        std::string attempt_err;
        HINTERNET h = open_url(url, headers, &attempt_err);
        if (!h) {
            if (err) *err = attempt_err;
            if (!retryable_error(attempt_err)) return false;
            if (attempt + 1 < kMaxAttempts) retry_backoff(attempt);
            continue;
        }
        bool ok = read_all(h, out, &attempt_err, kMaxResponseBytes);
        InternetCloseHandle(h);
        if (ok) return true;
        if (err) *err = attempt_err;
        out.clear();
        if (!retryable_error(attempt_err)) return false;
        if (attempt + 1 < kMaxAttempts) retry_backoff(attempt);
    }
    return false;
}

bool request(const std::wstring& url, const std::wstring& method,
             const std::vector<std::wstring>& headers, const std::vector<uint8_t>& body,
             std::vector<uint8_t>& out, std::string* err) {
    out.clear();
    if (!url_allowed(url, err)) return false;

    URL_COMPONENTSW comp{};
    comp.dwStructSize = sizeof(comp);
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    comp.lpszHostName = host;
    comp.dwHostNameLength = 256;
    comp.lpszUrlPath = path;
    comp.dwUrlPathLength = 2048;
    if (!InternetCrackUrlW(url.c_str(), 0, ICU_DECODE, &comp)) {
        if (err) *err = "bad url";
        return false;
    }
    bool https = comp.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET conn = InternetConnectW(open_internet(), host, comp.nPort, nullptr, nullptr,
                                      INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) {
        if (err) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "connect failed, err=%lu", GetLastError());
            *err = buf;
        }
        return false;
    }
    apply_timeouts(conn);
    std::wstring full = std::wstring(path) + (comp.dwExtraInfoLength ? comp.lpszExtraInfo : L"");
    HINTERNET req = HttpOpenRequestW(conn, method.c_str(), full.empty() ? L"/" : full.c_str(), nullptr,
                                     nullptr, nullptr,
                                     (https ? INTERNET_FLAG_SECURE : 0) | INTERNET_FLAG_NO_CACHE_WRITE |
                                         INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_PRAGMA_NOCACHE,
                                     reinterpret_cast<DWORD_PTR>(nullptr));
    if (!req) {
        if (err) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "http request failed, err=%lu", GetLastError());
            *err = buf;
        }
        InternetCloseHandle(conn);
        return false;
    }
    apply_timeouts(req);
    std::wstring hdr;
    if (!headers.empty()) {
        for (const auto& hh : headers) {
            hdr += hh;
            hdr += L"\r\n";
        }
    }
    bool ok = HttpSendRequestW(req, hdr.empty() ? nullptr : hdr.c_str(),
                               static_cast<DWORD>(hdr.size()),
                               body.empty() ? nullptr : const_cast<uint8_t*>(body.data()),
                               static_cast<DWORD>(body.size()));
    if (!ok) {
        if (err) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "http send failed, err=%lu", GetLastError());
            *err = buf;
        }
        InternetCloseHandle(req);
        InternetCloseHandle(conn);
        return false;
    }
    DWORD code = 0;
    DWORD len = sizeof(code);
    if (HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, nullptr) &&
        (code < 200 || code >= 300)) {
        std::string resp;
        std::vector<uint8_t> tmp;
        read_all(req, tmp, nullptr, kMaxResponseBytes);
        resp.assign(tmp.begin(), tmp.end());
        // Keep the complete, bounded response available to callers. OAuth
        // device-code polling intentionally returns structured 400 responses
        // such as "authorization_pending"; truncating that JSON here makes it
        // impossible for the auth layer to recognise a normal waiting state.
        out = std::move(tmp);
        if (err) *err = "http status " + std::to_string(code) + ": " + resp.substr(0, 400);
        InternetCloseHandle(req);
        InternetCloseHandle(conn);
        return false;
    }
    ok = read_all(req, out, err, kMaxResponseBytes);
    InternetCloseHandle(req);
    InternetCloseHandle(conn);
    return ok;
}

bool post(const std::wstring& url, const std::vector<std::wstring>& headers,
          const std::vector<uint8_t>& body, std::vector<uint8_t>& out, std::string* err) {
    return request(url, L"POST", headers, body, out, err);
}

bool put(const std::wstring& url, const std::vector<std::wstring>& headers,
         const std::vector<uint8_t>& body, std::vector<uint8_t>& out, std::string* err) {
    return request(url, L"PUT", headers, body, out, err);
}

bool del(const std::wstring& url, const std::vector<std::wstring>& headers,
         std::vector<uint8_t>& out, std::string* err) {
    return request(url, L"DELETE", headers, {}, out, err);
}

bool download(const std::wstring& url, const std::wstring& path, Progress progress,
              std::string* err, const std::string& expected_sha1, int64_t expected_size,
              const std::string& expected_sha256) {
    if (!url_allowed(url, err)) return false;
    const std::wstring part = path + L".part";
    if (expected_size > static_cast<int64_t>(kAbsoluteMaxDownloadBytes)) {
        if (err) *err = "expected download exceeds safety limit";
        return false;
    }
    const uint64_t download_limit = expected_size >= 0
        ? (std::max)(kDefaultMaxDownloadBytes, static_cast<uint64_t>(expected_size))
        : kDefaultMaxDownloadBytes;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::string attempt_err;
        uint64_t existing = file_exists(part) ? file_size(part) : 0;
        if (existing > download_limit) {
            DeleteFileW(part.c_str());
            existing = 0;
        }
        std::vector<std::wstring> headers;
        if (existing > 0)
            headers.push_back(L"Range: bytes=" + std::to_wstring(existing) + L"-");
        HINTERNET h = open_url(url, headers, &attempt_err);
        if (!h) {
            // A stale resume offset (for example a .part already at the full
            // size from a killed run) gets HTTP 416 from the server. Discard
            // the rejected partial file and restart the download; the size and
            // hash checks below still protect the final artifact.
            if (existing > 0 && attempt_err.find("http status 416") != std::string::npos) {
                DeleteFileW(part.c_str());
                if (attempt + 1 < kMaxAttempts) retry_backoff(attempt);
                continue;
            }
            if (err) *err = attempt_err;
            if (!retryable_error(attempt_err)) return false;
            if (attempt + 1 < kMaxAttempts) retry_backoff(attempt);
            continue;
        }

        // HTTP_QUERY_FLAG_NUMBER exposes Content-Length as a 32-bit DWORD.
        // AI models exceed 4 GB, so query the header as text and parse it as
        // uint64_t to keep resume offsets and progress truthful.
        uint64_t response_length = 0;
        DWORD length_bytes = 0;
        if (!HttpQueryInfoA(h, HTTP_QUERY_CONTENT_LENGTH, nullptr, &length_bytes, nullptr) &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER && length_bytes > 0) {
            std::string raw(length_bytes + 1, '\0');
            if (HttpQueryInfoA(h, HTTP_QUERY_CONTENT_LENGTH, raw.data(), &length_bytes, nullptr)) {
                raw.resize(length_bytes);
                try {
                    response_length = std::stoull(raw);
                } catch (...) {
                    response_length = 0;
                }
            }
        }
        DWORD code = 0;
        DWORD len = sizeof(code);
        HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, nullptr);
        const bool resumed = existing > 0 && code == 206;
        if (!resumed) existing = 0;
        const uint64_t total = response_length + existing;
        if (expected_size >= 0 && total > 0 && static_cast<int64_t>(total) != expected_size) {
            if (err) *err = "content length mismatch";
            InternetCloseHandle(h);
            return false;
        }

        HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr,
                               resumed ? OPEN_ALWAYS : CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            if (err) *err = "cannot create output file";
            InternetCloseHandle(h);
            return false;
        }
        if (resumed) {
            LARGE_INTEGER end{};
            end.QuadPart = 0;
            if (!SetFilePointerEx(f, end, nullptr, FILE_END)) {
                CloseHandle(f);
                InternetCloseHandle(h);
                if (err) *err = "cannot seek partial output";
                return false;
            }
        }
        std::vector<uint8_t> buf(262144);
        uint64_t done = existing;
        bool ok = true;
        bool interrupted = false;
        for (;;) {
            DWORD read = 0;
            if (!InternetReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read)) {
                if (err) *err = "read failed";
                ok = false;
                break;
            }
            if (read == 0) break;
            if (done + read > download_limit) {
                if (err) *err = "download exceeds size limit";
                ok = false;
                break;
            }
            DWORD written = 0;
            if (!WriteFile(f, buf.data(), read, &written, nullptr) || written != read) {
                if (err) *err = "write failed";
                ok = false;
                break;
            }
            done += read;
            if (progress && !progress(done, total)) {
                interrupted = true;
                if (err && err->empty()) *err = "download interrupted";
                ok = false;
                break;
            }
        }
        CloseHandle(f);
        InternetCloseHandle(h);
        if (!ok) {
            // Keep a verified prefix so a later attempt can request the
            // remainder. Integrity/size checks below still discard invalid
            // content before activation.
            if (interrupted) return false;
            if (err && !retryable_error(*err)) return false;
            if (attempt + 1 < kMaxAttempts) retry_backoff(attempt);
            continue;
        }
        if (expected_size >= 0 && static_cast<int64_t>(done) != expected_size) {
            if (err) *err = "size mismatch after download";
            DeleteFileW(part.c_str());
            return false;
        }
        if (!expected_sha1.empty()) {
            std::string actual = sha1_file(part);
            if (actual.empty() || actual != expected_sha1) {
                if (err) *err = "sha1 mismatch after download";
                DeleteFileW(part.c_str());
                return false;
            }
        }
        if (!expected_sha256.empty()) {
            std::string actual = sha256_file(part);
            if (actual.empty() || actual != expected_sha256) {
                if (err) *err = "sha256 mismatch after download";
                DeleteFileW(part.c_str());
                return false;
            }
        }
        if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (err) *err = "finalize failed";
            DeleteFileW(part.c_str());
            return false;
        }
        return true;
    }
    if (err && err->empty()) *err = "download failed after retries";
    return false;
}

bool verify_file(const std::wstring& path, const std::string& sha1, int64_t size) {
    if (!file_exists(path) || file_size(path) == 0) return false;
    if (size >= 0 && static_cast<int64_t>(file_size(path)) != size) return false;
    if (!sha1.empty()) {
        std::string actual = sha1_file(path);
        if (actual.empty() || actual != sha1) return false;
    }
    return true;
}

std::string sha1_hex(const void* data, size_t len) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return std::string();
    }
    std::string out;
    if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
        if (CryptHashData(hash, static_cast<const BYTE*>(data), static_cast<DWORD>(len), 0)) {
            BYTE digest[20] = {0};
            DWORD digest_len = sizeof(digest);
            if (CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0)) {
                std::ostringstream ss;
                ss << std::hex << std::setfill('0');
                for (DWORD i = 0; i < digest_len; ++i) ss << std::setw(2) << static_cast<int>(digest[i]);
                out = ss.str();
            }
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return out;
}

std::string sha1_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return std::string();
    std::vector<uint8_t> buf(1 << 20);
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return std::string();
    }
    if (!CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
        CryptReleaseContext(prov, 0);
        return std::string();
    }
    for (;;) {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = f.gcount();
        if (got > 0 && !CryptHashData(hash, buf.data(), static_cast<DWORD>(got), 0)) {
            CryptDestroyHash(hash);
            CryptReleaseContext(prov, 0);
            return std::string();
        }
        if (got < static_cast<std::streamsize>(buf.size())) break;
    }
    BYTE digest[20] = {0};
    DWORD digest_len = sizeof(digest);
    std::string out;
    if (CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0)) {
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (DWORD i = 0; i < digest_len; ++i) ss << std::setw(2) << static_cast<int>(digest[i]);
        out = ss.str();
    }
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return out;
}

std::string sha256_hex(const void* data, size_t len) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return std::string();
    std::string out;
    if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        if (CryptHashData(hash, static_cast<const BYTE*>(data), static_cast<DWORD>(len), 0)) {
            BYTE digest[32] = {0};
            DWORD digest_len = sizeof(digest);
            if (CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0)) {
                std::ostringstream ss;
                ss << std::hex << std::setfill('0');
                for (DWORD i = 0; i < digest_len; ++i)
                    ss << std::setw(2) << static_cast<int>(digest[i]);
                out = ss.str();
            }
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return out;
}

std::string sha256_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return std::string();
    std::vector<uint8_t> buf(1 << 20);
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return std::string();
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(prov, 0);
        return std::string();
    }
    for (;;) {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = f.gcount();
        if (got > 0 && !CryptHashData(hash, buf.data(), static_cast<DWORD>(got), 0)) {
            CryptDestroyHash(hash);
            CryptReleaseContext(prov, 0);
            return std::string();
        }
        if (got < static_cast<std::streamsize>(buf.size())) break;
    }
    BYTE digest[32] = {0};
    DWORD digest_len = sizeof(digest);
    std::string out;
    if (CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0)) {
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (DWORD i = 0; i < digest_len; ++i)
            ss << std::setw(2) << static_cast<int>(digest[i]);
        out = ss.str();
    }
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return out;
}

bool mkdirs(const std::wstring& path) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    if (ec) return false;
    return std::filesystem::is_directory(std::filesystem::path(path), ec) && !ec;
}

bool file_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool directory_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

uint64_t file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr) <= 0)
        return std::string();
    s.resize(static_cast<size_t>(n - 1));
    return s;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n) <= 0)
        return std::wstring();
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

std::wstring get_local_app_data_path() {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                  static_cast<DWORD>(std::size(buffer)));
    return length > 0 && length < std::size(buffer) ? std::wstring(buffer, length) : L"";
}

}  // namespace aml::net
