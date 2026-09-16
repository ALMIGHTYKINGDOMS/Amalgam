#include "updater.h"

#include "extract.h"
#include "json.h"
#include "net.h"

// Generated at configure time when AMALGAM_UPDATER_PUBLIC_KEY_FILE is set;
// otherwise this header does not exist and signed manifests fail closed.
#if __has_include("updater_public_key.h")
#include "updater_public_key.h"
#define AMALGAM_HAS_EMBEDDED_UPDATER_KEY 1
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10 API surface (CryptVerifyDetachedSignature)
#endif

#include <windows.h>
#include <wincrypt.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <vector>

namespace aml::updater {

namespace {

// Payload-signing public key (PEM). Configured at runtime by release
// engineering; never stored in the repository.
std::mutex g_key_mu;
std::string g_signing_public_key;

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool is_https_url(const std::string& url) {
    return url.rfind("https://", 0) == 0;
}

// Split "3.0.1-beta.2" into leading numeric segments and a prerelease tag.
struct ParsedVersion {
    std::vector<int> nums;
    std::string prerelease;  // e.g. "beta.2" or "" for a release build
};

bool parse_version(const std::string& text, ParsedVersion& out) {
    std::string s = text;
    const size_t dash = s.find('-');
    if (dash != std::string::npos) {
        out.prerelease = s.substr(dash + 1);
        s = s.substr(0, dash);
    }
    std::istringstream stream(s);
    std::string part;
    while (std::getline(stream, part, '.')) {
        if (part.empty()) return false;
        int value = 0;
        for (char c : part) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            value = value * 10 + (c - '0');
            if (value > 1000000) return false;  // sane bound
        }
        out.nums.push_back(value);
    }
    if (out.nums.empty()) return false;
    return true;
}

}  // namespace

int compare_versions(const std::string& a, const std::string& b) {
    ParsedVersion pa, pb;
    if (!parse_version(a, pa) || !parse_version(b, pb)) {
        // Fall back to plain string comparison for unparseable versions so a
        // malformed manifest can never be treated as "up to date".
        const int cmp = a.compare(b);
        return cmp < 0 ? -1 : cmp > 0 ? 1 : 0;
    }
    const size_t common = pa.nums.size() < pb.nums.size() ? pa.nums.size() : pb.nums.size();
    for (size_t i = 0; i < common; ++i) {
        if (pa.nums[i] != pb.nums[i]) return pa.nums[i] < pb.nums[i] ? -1 : 1;
    }
    if (pa.nums.size() != pb.nums.size()) return pa.nums.size() < pb.nums.size() ? -1 : 1;
    // Same numeric version: a release build beats a prerelease build.
    const bool a_pre = !pa.prerelease.empty();
    const bool b_pre = !pb.prerelease.empty();
    if (a_pre != b_pre) return a_pre ? -1 : 1;
    if (a_pre) {
        // Semver-style prerelease ordering: "beta.2" < "beta.10" because the
        // trailing identifiers are compared numerically.
        std::istringstream as(pa.prerelease);
        std::istringstream bs(pb.prerelease);
        std::string aseg, bseg;
        while (std::getline(as, aseg, '.')) {
            if (!std::getline(bs, bseg, '.')) return 1;  // a has more segments
            const bool a_num = !aseg.empty() &&
                std::all_of(aseg.begin(), aseg.end(), [](char c) {
                    return std::isdigit(static_cast<unsigned char>(c));
                });
            const bool b_num = !bseg.empty() &&
                std::all_of(bseg.begin(), bseg.end(), [](char c) {
                    return std::isdigit(static_cast<unsigned char>(c));
                });
            if (a_num && b_num) {
                const long av = std::strtol(aseg.c_str(), nullptr, 10);
                const long bv = std::strtol(bseg.c_str(), nullptr, 10);
                if (av != bv) return av < bv ? -1 : 1;
            } else {
                const int cmp = aseg.compare(bseg);
                if (cmp != 0) return cmp < 0 ? -1 : 1;
            }
        }
        if (std::getline(bs, bseg, '.')) return -1;  // b has more segments
        return 0;
    }
    return 0;
}

bool should_apply(const UpdateInfo& info, const std::string& current_version,
                  const std::string& current_channel, std::string* err) {
    if (err) err->clear();
    // Anti-rollback: never apply a version that is not strictly newer.
    if (compare_versions(info.version, current_version) <= 0) {
        if (err) {
            *err = "refusing an update that is not newer than the running version";
        }
        return false;
    }
    if (!info.min_version.empty() &&
        compare_versions(current_version, info.min_version) < 0) {
        if (err) {
            *err = "the running launcher version is too old for this update";
        }
        return false;
    }
    // Channel crossing guard: a stable build must not silently move to a
    // prerelease channel via an update.
    if (current_channel == "stable" && info.channel == "beta") {
        if (err) {
            *err = "refusing to move from the stable channel to beta";
        }
        return false;
    }
    return true;
}

std::string canonical_manifest_json(const UpdateInfo& info) {
    // Fixed key order shared with the release tooling (tools/update-manifest.mjs).
    // Json::dump is compact and preserves insertion order, matching
    // JSON.stringify for the same object; this must stay byte-identical.
    Json j = Json::obj();
    j.set("channel", Json::str(info.channel));
    j.set("download_url", Json::str(info.download_url));
    j.set("mandatory", Json::boolean(info.mandatory));
    j.set("min_version", Json::str(info.min_version));
    j.set("notes", Json::str(info.notes));
    j.set("sha256", Json::str(info.sha256));
    j.set("size", Json(static_cast<int64_t>(info.size)));
    j.set("version", Json::str(info.version));
    return j.dump();
}

std::wstring default_manifest_url() {
    // Backend-owned release manifest; the schema below must be preserved.
    return L"https://amalgam-mc.com/releases/launcher/manifest.json";
}

bool parse_manifest_text(const std::string& text, UpdateInfo& out, std::string* err) {
    if (err) err->clear();

    // A host that answers with its own web page (unpublished feed, error page,
    // captive portal) is the common case of a non-JSON body. Naming that says
    // why no update can be verified; the JSON parser's "unexpected char '<'"
    // does not. Still fails closed either way.
    size_t first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (first != std::string::npos && text[first] == '<') {
        if (err)
            *err =
                "release server returned a web page instead of a signed update "
                "manifest";
        return false;
    }

    Json root;
    std::string parse_error;
    root = Json::parse(text, &parse_error);
    if (!parse_error.empty()) {
        if (err) *err = "update manifest is malformed: " + parse_error;
        return false;
    }
    if (!root.is(Json::Type::Obj)) {
        if (err) *err = "update manifest is not an object";
        return false;
    }

    UpdateInfo info;
    info.version = root.get("version").as_str();
    info.channel = root.get("channel").as_str();
    info.download_url = root.get("download_url").as_str();
    info.sha256 = root.get("sha256").as_str();
    info.signature = root.get("signature").as_str();
    info.manifest_signature = root.get("manifest_signature").as_str();
    info.min_version = root.get("min_version").as_str();
    info.notes = root.get("notes").as_str();
    info.size = root.get("size").as_int();
    info.mandatory = root.get("mandatory").as_bool();

    if (info.version.empty() || info.download_url.empty()) {
        if (err) *err = "update manifest is missing version or download_url";
        return false;
    }
    if (!is_https_url(info.download_url)) {
        if (err) *err = "update payload URL must be HTTPS";
        return false;
    }
    if (!info.sha256.empty() && !is_hex64(info.sha256)) {
        if (err) *err = "update manifest sha256 is not a 64-character hex digest";
        return false;
    }

    // Manifest signature policy: the signature binds the canonical manifest
    // fields (version, channel, URL, size, hash, min_version), so mutating any
    // of them invalidates it. Fail closed when a signed manifest arrives
    // without a configured verification key.
    if (!info.manifest_signature.empty()) {
        std::string key;
        {
            std::lock_guard<std::mutex> lock(g_key_mu);
            key = g_signing_public_key;
        }
        if (key.empty()) {
            if (err) {
                *err = "update manifest is signed but no verification key is configured";
            }
            return false;
        }
        if (!verify_manifest_signature(key, canonical_manifest_json(info),
                                       info.manifest_signature, err)) {
            return false;
        }
    }

    out = std::move(info);
    return true;
}

bool fetch_manifest(const std::wstring& url, UpdateInfo& out, std::string* err) {
    if (err) err->clear();
    if (!net::validate_url(url, err)) return false;

    std::vector<uint8_t> body;
    if (!net::get(url, body, err)) {
        if (err && err->empty()) *err = "update manifest request failed";
        return false;
    }

    return parse_manifest_text(std::string(body.begin(), body.end()), out, err);
}

void set_signing_public_key(const std::string& pem) {
    std::lock_guard<std::mutex> lock(g_key_mu);
    g_signing_public_key = pem;
}

void init_from_embedded_key() {
#ifdef AMALGAM_HAS_EMBEDDED_UPDATER_KEY
    if constexpr (sizeof(aml::updater::detail::kUpdaterPublicKeyPem) > 8) {
        set_signing_public_key(aml::updater::detail::kUpdaterPublicKeyPem);
    }
#endif
}

bool has_signing_key() {
    std::lock_guard<std::mutex> lock(g_key_mu);
    return !g_signing_public_key.empty();
}

namespace {

using Feed = std::function<bool(HCRYPTHASH)>;

// Core RSA-SHA256 verification shared by the payload and manifest paths:
// decode the base64 signature, import the PEM public key, create an SHA-256
// hash, feed it through `feed`, then verify the signature over it.
bool verify_rsa_sha256(const std::string& pem_public_key,
                       const std::string& base64_signature,
                       const Feed& feed, const char* what, std::string* err) {
    if (err) err->clear();

    // Decode the base64 signature.
    std::vector<uint8_t> sig;
    DWORD decoded_len = 0;
    if (!CryptStringToBinaryA(base64_signature.c_str(),
                              static_cast<DWORD>(base64_signature.size()),
                              CRYPT_STRING_BASE64, nullptr, &decoded_len,
                              nullptr, nullptr) ||
        decoded_len == 0) {
        if (err) *err = std::string(what) + " signature is not valid base64";
        return false;
    }
    sig.resize(decoded_len);
    if (!CryptStringToBinaryA(base64_signature.c_str(),
                              static_cast<DWORD>(base64_signature.size()),
                              CRYPT_STRING_BASE64, sig.data(), &decoded_len,
                              nullptr, nullptr)) {
        if (err) *err = std::string(what) + " signature is not valid base64";
        return false;
    }
    sig.resize(decoded_len);

    // Decode the PEM public key into a CERT_PUBLIC_KEY_INFO.
    std::vector<uint8_t> der;
    DWORD der_len = 0;
    if (!CryptStringToBinaryA(pem_public_key.c_str(),
                              static_cast<DWORD>(pem_public_key.size()),
                              CRYPT_STRING_BASE64HEADER, nullptr, &der_len,
                              nullptr, nullptr) ||
        der_len == 0) {
        if (err) *err = "update signing key is not a valid PEM public key";
        return false;
    }
    der.resize(der_len);
    if (!CryptStringToBinaryA(pem_public_key.c_str(),
                              static_cast<DWORD>(pem_public_key.size()),
                              CRYPT_STRING_BASE64HEADER, der.data(), &der_len,
                              nullptr, nullptr)) {
        if (err) *err = "update signing key is not a valid PEM public key";
        return false;
    }
    der.resize(der_len);

    DWORD pub_len = 0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                             der.data(), der_len, 0, nullptr, nullptr,
                             &pub_len) ||
        pub_len == 0) {
        if (err) *err = "update signing key is not a valid RSA public key";
        return false;
    }
    std::vector<uint8_t> pub_buf(pub_len);
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                             der.data(), der_len, 0, nullptr, pub_buf.data(),
                             &pub_len)) {
        if (err) *err = "update signing key is not a valid RSA public key";
        return false;
    }
    CERT_PUBLIC_KEY_INFO* pub_info =
        reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(pub_buf.data());

    HCRYPTPROV h_prov = 0;
    HCRYPTKEY h_key = 0;
    HCRYPTHASH h_hash = 0;
    bool ok = false;
    if (!CryptAcquireContextW(&h_prov, nullptr, MS_ENH_RSA_AES_PROV, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT)) {
        if (err && err->empty()) {
            *err = "signature verify: could not acquire provider (error " +
                   std::to_string(GetLastError()) + ")";
        }
    } else if (!CryptImportPublicKeyInfo(h_prov, X509_ASN_ENCODING, pub_info,
                                         &h_key)) {
        if (err && err->empty()) {
            *err = "signature verify: could not import public key (error " +
                   std::to_string(GetLastError()) + ")";
        }
    } else if (!CryptCreateHash(h_prov, CALG_SHA_256, 0, 0, &h_hash)) {
        if (err && err->empty()) {
            *err = "signature verify: could not create hash (error " +
                   std::to_string(GetLastError()) + ")";
        }
    } else if (!feed(h_hash)) {
        if (err && err->empty()) {
            *err = "could not feed data for signature verification";
        }
    } else {
        // CryptVerifySignature expects the historic little-endian signature
        // byte order, while OpenSSL/Node (the release tooling) emits the
        // standard big-endian PKCS#1 order. Both encodings prove possession
        // of the same private key, so accept either: verify as-is first,
        // then retry with the byte order flipped.
        if (CryptVerifySignature(h_hash, sig.data(),
                                 static_cast<DWORD>(sig.size()),
                                 h_key, nullptr, 0)) {
            ok = true;
        } else {
            std::vector<uint8_t> flipped(sig.rbegin(), sig.rend());
            if (CryptVerifySignature(h_hash, flipped.data(),
                                     static_cast<DWORD>(flipped.size()),
                                     h_key, nullptr, 0)) {
                ok = true;
            }
        }
        if (!ok && err && err->empty()) {
            *err = std::string(what) + " signature verification failed";
        }
    }

    if (h_hash) CryptDestroyHash(h_hash);
    if (h_key) CryptDestroyKey(h_key);
    if (h_prov) CryptReleaseContext(h_prov, 0);
    return ok;
}

}  // namespace

bool verify_payload_signature(const std::string& pem_public_key,
                              const std::string& base64_signature,
                              const std::wstring& payload_path,
                              std::string* err) {
    return verify_rsa_sha256(
        pem_public_key, base64_signature,
        [&](HCRYPTHASH h) {
            // Hash the payload file content in bounded chunks.
            std::ifstream file(payload_path, std::ios::binary);
            if (!file.is_open()) return false;
            char buffer[64 * 1024];
            bool ok = true;
            while (file.good() && !file.eof()) {
                file.read(buffer, sizeof(buffer));
                const std::streamsize got = file.gcount();
                if (got > 0 &&
                    !CryptHashData(h, reinterpret_cast<const BYTE*>(buffer),
                                   static_cast<DWORD>(got), 0)) {
                    ok = false;
                    break;
                }
            }
            return ok && (file.eof() || file.good());
        },
        "update payload", err);
}

bool verify_manifest_signature(const std::string& pem_public_key,
                               const std::string& canonical_json,
                               const std::string& base64_signature,
                               std::string* err) {
    return verify_rsa_sha256(
        pem_public_key, base64_signature,
        [&](HCRYPTHASH h) {
            return CryptHashData(h,
                                 reinterpret_cast<const BYTE*>(canonical_json.data()),
                                 static_cast<DWORD>(canonical_json.size()), 0) != 0;
        },
        "update manifest", err);
}

namespace {

// Signature policy for a staged payload. A manifest that carries a
// signature is accepted only when a verification key is configured AND the
// payload verifies against it. An unsigned manifest is accepted (HTTPS +
// SHA-256 only) and reported as unverified by the caller. A remotely
// supplied SHA-256 is never treated as authentication on its own.
bool signature_policy_ok(const UpdateInfo& info, const std::wstring& payload_path,
                         std::string* err) {
    if (info.signature.empty()) return true;
    std::string key;
    {
        std::lock_guard<std::mutex> lock(g_key_mu);
        key = g_signing_public_key;
    }
    if (key.empty()) {
        if (err) *err =
            "update manifest is signed but no verification key is configured";
        return false;
    }
    return verify_payload_signature(key, info.signature, payload_path, err);
}

}  // namespace

bool stage_update(const UpdateInfo& info, const std::wstring& exe_dir,
                  std::wstring& staged_path, std::string* err) {
    if (err) err->clear();

    // The staged filename embeds the manifest version. Defense in depth:
    // even in unsigned dev builds, a hostile version string must never be
    // able to steer the payload outside the staging directory.
    for (char c : info.version) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) ||
                        c == '.' || c == '-' || c == '_';
        if (!ok) {
            if (err) *err = "manifest version contains unsafe characters";
            return false;
        }
    }

    const std::wstring updates_dir = exe_dir + L"\\updates";
    if (!net::mkdirs(updates_dir)) {
        if (err) *err = "could not create update staging directory";
        return false;
    }

    const std::wstring target = updates_dir + L"\\amalgam-" +
                                net::to_wide(info.version) + L".zip";

    // Reuse a previously staged payload when it already passes verification.
    if (net::file_exists(target)) {
        const bool size_ok = info.size < 0 || net::file_size(target) ==
                                                   static_cast<uint64_t>(info.size);
        const bool hash_ok = info.sha256.empty() ||
                             net::sha256_file(target) == info.sha256;
        if (size_ok && hash_ok) {
            std::string sig_err;
            if (!signature_policy_ok(info, target, &sig_err)) {
                if (err) *err = sig_err;
                return false;
            }
            staged_path = target;
            return true;
        }
        // Stale or tampered staging: remove it and re-download.
        DeleteFileW(target.c_str());
    }

    // Verify size + SHA-256 during download; a mismatch deletes the payload.
    if (!net::download(net::to_wide(info.download_url), target, nullptr, err,
                       std::string(), info.size, info.sha256)) {
        return false;
    }

    // Enforce the signature policy over the freshly downloaded payload.
    {
        std::string sig_err;
        if (!signature_policy_ok(info, target, &sig_err)) {
            if (err) *err = sig_err;
            DeleteFileW(target.c_str());
            return false;
        }
    }
    staged_path = target;
    return true;
}

bool prepare_apply(const UpdateInfo& info, const std::wstring& exe_dir,
                   const std::wstring& staged_path, std::wstring& helper_path,
                   std::string* err) {
    if (err) err->clear();

    // Current executable path (this process).
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe_path = self;
    const size_t slash = exe_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        if (err) *err = "cannot determine current executable path";
        return false;
    }
    const std::wstring exe_name = exe_path.substr(slash + 1);

    // Extract the verified payload into its own staging directory.
    const std::wstring stage_dir = exe_dir + L"\\updates\\staged-" +
                                   net::to_wide(info.version);
    std::error_code ec;
    std::filesystem::remove_all(stage_dir, ec);
    if (!net::mkdirs(stage_dir)) {
        if (err) *err = "could not create extraction directory";
        return false;
    }
    if (!extract::zip(staged_path, stage_dir, err)) {
        return false;
    }

    // The payload must contain the launcher executable at its root.
    const std::wstring new_exe = stage_dir + L"\\" + exe_name;
    if (!net::file_exists(new_exe)) {
        if (err) {
            *err = "update payload does not contain " +
                   net::to_utf8(exe_name);
        }
        return false;
    }

    // Generate the helper batch. Flow:
    //   1. wait for the running launcher to exit
    //   2. copy the current executable to a last-known-good backup
    //   3. replace the executable with the staged one
    //   4. relaunch the new build
    //   5. on any failure, restore the backup and relaunch it
    const std::wstring batch = exe_dir + L"\\updates\\apply-update.bat";
    const std::wstring backup = exe_dir + L"\\updates\\" + exe_name + L".last-good";
    std::wstring cmd;
    cmd += L"@echo off\r\n";
    cmd += L"rem Amalgam self-update helper (generated; payload hash was verified by the launcher)\r\n";
    cmd += L":wait\r\n";
    cmd += L"tasklist /FI \"IMAGENAME eq " + exe_name + L"\" 2>nul | find /I \"" + exe_name + L"\" >nul\r\n";
    cmd += L"if %errorlevel%==0 ( timeout /t 1 /nobreak >nul & goto wait )\r\n";
    cmd += L"copy /Y \"" + exe_dir + L"\\" + exe_name + L"\" \"" + backup + L"\" >nul\r\n";
    cmd += L"copy /Y \"" + new_exe + L"\" \"" + exe_dir + L"\\" + exe_name + L"\" >nul\r\n";
    cmd += L"if not exist \"" + exe_dir + L"\\" + exe_name + L"\" goto fail\r\n";
    cmd += L"start \"\" \"" + exe_dir + L"\\" + exe_name + L"\"\r\n";
    cmd += L"exit /b 0\r\n";
    cmd += L":fail\r\n";
    cmd += L"copy /Y \"" + backup + L"\" \"" + exe_dir + L"\\" + exe_name + L"\" >nul\r\n";
    cmd += L"start \"\" \"" + exe_dir + L"\\" + exe_name + L"\"\r\n";
    cmd += L"exit /b 1\r\n";

    std::ofstream out(batch, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (err) *err = "could not write update helper batch";
        return false;
    }
    out << net::to_utf8(cmd);
    out.flush();
    out.close();
    if (!out) {
        if (err) *err = "could not finish writing update helper batch";
        return false;
    }

    helper_path = batch;
    return true;
}

bool rollback_update(const std::wstring& exe_dir, std::string& error) {
    error.clear();

    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe_path = self;
    const size_t slash = exe_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        error = "cannot determine current executable path";
        return false;
    }
    const std::wstring exe_name = exe_path.substr(slash + 1);
    const std::wstring backup = exe_dir + L"\\updates\\" + exe_name + L".last-good";

    if (!net::file_exists(backup)) {
        error = "no last-known-good build is available";
        return false;
    }
    if (!CopyFileW(backup.c_str(), exe_path.c_str(), FALSE)) {
        error = "could not restore last-known-good build (error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

}  // namespace aml::updater
