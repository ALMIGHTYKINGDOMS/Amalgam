// Self-updater unit tests.
//
// These tests deliberately never invoke a real launcher replacement: they
// exercise version ordering, manifest validation, staged-payload
// verification, the signature policy, and RSA-SHA256 verification against a
// key pair generated at test time. The swap/rollback batch logic is
// validated on a real machine only.

#include "updater.h"

#include "net.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::wstring temp_dir() {
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    wchar_t name[MAX_PATH]{};
    GetTempFileNameW(tmp, L"amal", 0, name);
    DeleteFileW(name);
    std::filesystem::create_directories(name);
    return name;
}

std::string read_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

std::string b64_encode(const std::vector<uint8_t>& data) {
    DWORD len = 0;
    CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::string out(len, '\0');
    CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &len);
    out.resize(len);
    return out;
}

void test_version_ordering() {
    check(aml::updater::compare_versions("3.0", "3.0") == 0, "3.0 == 3.0");
    check(aml::updater::compare_versions("3.0", "3.0.1") < 0, "3.0 < 3.0.1");
    check(aml::updater::compare_versions("3.0.1", "3.0") > 0, "3.0.1 > 3.0");
    check(aml::updater::compare_versions("3.0.9", "3.1.0") < 0, "3.0.9 < 3.1.0");
    check(aml::updater::compare_versions("3.1", "3.1.0") < 0, "3.1 < 3.1.0");
    check(aml::updater::compare_versions("3.1.0", "3.1.0-beta.2") > 0,
          "release beats prerelease");
    check(aml::updater::compare_versions("3.1.0-beta.2", "3.1.0-beta.10") < 0,
          "prerelease ordering");
    check(aml::updater::compare_versions("3.0", "x.y") != 0, "unparseable not equal");
    check(aml::updater::compare_versions("3.0.1", "3.0.1") == 0, "same version");
    check(aml::updater::compare_versions("3.0.0-rc.1", "3.0.0-beta.10") > 0,
          "rc beats beta at the same numeric version");
    check(aml::updater::compare_versions("3.0.0-rc.1", "3.0.0") < 0,
          "prerelease ranks below its release");
    check(aml::updater::compare_versions("4.0.0", "3.0.0") > 0,
          "4.0.0 beats 3.0.0");
    check(aml::updater::compare_versions("3.0.0-beta.1", "3.0.0-rc.1") < 0,
          "beta.1 below rc.1");
    // Malformed versions must never be treated as newer than a real one.
    check(aml::updater::compare_versions("3.0", "3..0") != 0, "empty segment malformed");
    check(aml::updater::compare_versions("3.0", "v3.0") != 0, "v-prefix malformed");
    check(aml::updater::compare_versions("3.0", "") != 0, "empty version malformed");
}

void test_manifest_parsing() {
    const std::string valid =
        "{\"version\":\"3.0.1\",\"channel\":\"stable\","
        "\"download_url\":\"https://cdn.example.com/amalgam-3.0.1.zip\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"size\":1234,\"mandatory\":false,\"notes\":\"fixes\"}";
    aml::updater::UpdateInfo info;
    std::string err;
    check(aml::updater::parse_manifest_text(valid, info, &err),
          "valid manifest parses");
    check(info.version == "3.0.1", "manifest version read");
    check(info.size == 1234, "manifest size read");
    check(info.notes == "fixes", "manifest notes read");

    check(!aml::updater::parse_manifest_text("{not json", info, &err),
          "malformed JSON rejected");
    check(!aml::updater::parse_manifest_text("[]", info, &err),
          "non-object manifest rejected");
    check(!aml::updater::parse_manifest_text(
              "{\"download_url\":\"https://x/z.zip\"}", info, &err),
          "missing version rejected");
    check(!aml::updater::parse_manifest_text(
              "{\"version\":\"3.0.1\",\"download_url\":\"http://x/z.zip\"}", info,
              &err),
          "non-HTTPS payload URL rejected");
    check(!aml::updater::parse_manifest_text(
              "{\"version\":\"3.0.1\",\"download_url\":\"https://x/z.zip\","
              "\"sha256\":\"zzzz\"}",
              info, &err),
          "bad sha256 rejected");
}

void test_version_policy() {
    using aml::updater::UpdateInfo;
    using aml::updater::should_apply;
    std::string err;

    UpdateInfo newer;
    newer.version = "3.0.1";
    check(should_apply(newer, "3.0", "", &err), "strictly newer update applies");

    UpdateInfo same;
    same.version = "3.0";
    check(!should_apply(same, "3.0", "", &err), "same version refused (no-op)");

    UpdateInfo older;
    older.version = "2.9.9";
    check(!should_apply(older, "3.0", "", &err), "downgrade refused (anti-rollback)");

    UpdateInfo gated;
    gated.version = "3.1.0";
    gated.min_version = "3.0.5";
    check(!should_apply(gated, "3.0", "", &err), "too-old launcher refused for min_version");

    UpdateInfo gated_ok;
    gated_ok.version = "3.1.0";
    gated_ok.min_version = "2.9";
    check(should_apply(gated_ok, "3.0", "", &err), "min_version satisfied applies");

    // Channel crossing: a stable build must not silently move to beta.
    UpdateInfo beta;
    beta.version = "3.1.0";
    beta.channel = "beta";
    check(!should_apply(beta, "3.0", "stable", &err),
          "stable -> beta crossing refused");
    check(should_apply(beta, "3.0", "beta", &err),
          "beta channel update allowed on beta");
}

void test_stage_path_traversal_guard() {
    using aml::updater::UpdateInfo;
    using aml::updater::stage_update;

    // A hostile manifest version must not be able to steer the staged
    // payload outside the update directory, even when unsigned.
    UpdateInfo evil;
    evil.version = "..\\..\\evil";
    evil.download_url = "https://cdn.example.com/evil.zip";
    evil.size = 0;
    std::wstring staged;
    std::string err;
    check(!stage_update(evil, L"C:\\nonexistent", staged, &err),
          "path-traversal version refused");
    check(err.find("unsafe characters") != std::string::npos,
          "refusal explains itself");

    UpdateInfo slash;
    slash.version = "3.0.1/x";
    check(!stage_update(slash, L"C:\\nonexistent", staged, &err),
          "slash in version refused");

    // A safe version passes the character guard: any failure must come from
    // the staging directory (the exe dir is bogus), never from the guard.
    UpdateInfo ok;
    ok.version = "3.0.1-beta.2";
    ok.download_url = "https://cdn.example.com/a.zip";
    ok.size = 0;
    err.clear();
    const bool ok_result = stage_update(ok, L"C:\\nonexistent", staged, &err);
    check(!ok_result || staged.find(L"amalgam-3.0.1-beta.2.zip") != std::wstring::npos,
          "safe version accepted by the guard");
    check(ok_result || err.find("unsafe characters") == std::string::npos,
          "safe version never trips the character guard");
}

void test_manifest_signature() {
    using aml::updater::UpdateInfo;
    using aml::updater::canonical_manifest_json;
    using aml::updater::verify_manifest_signature;

    // Canonical form is deterministic and matches the release tooling
    // (tools/update-manifest.mjs) byte-for-byte.
    UpdateInfo info;
    info.version = "3.0.1";
    info.channel = "stable";
    info.download_url = "https://cdn.example.com/amalgam-3.0.1.zip";
    info.sha256 = std::string(64, 'a');
    info.size = 1234;
    info.min_version = "3.0";
    info.notes = "fixes";
    info.mandatory = false;
    const std::string canon = canonical_manifest_json(info);
    const std::string golden =
        "{\"channel\":\"stable\",\"download_url\":\"https://cdn.example.com/amalgam-3.0.1.zip\","
        "\"mandatory\":false,\"min_version\":\"3.0\",\"notes\":\"fixes\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"size\":1234,\"version\":\"3.0.1\"}";
    check(canon == golden, "canonical manifest form matches release tooling contract");

    // Generate an RSA keypair at test time and sign the canonical bytes.
    HCRYPTPROV h_prov = 0;
    HCRYPTKEY h_key = 0;
    HCRYPTHASH h_hash = 0;
    const wchar_t* container = L"amalgam-manifest-test";
    if (!CryptAcquireContextW(&h_prov, container, MS_ENH_RSA_AES_PROV,
                              PROV_RSA_AES, CRYPT_NEWKEYSET)) {
        if (!CryptAcquireContextW(&h_prov, container, MS_ENH_RSA_AES_PROV,
                                  PROV_RSA_AES, 0)) {
            std::printf("FAIL: cannot acquire crypto context\n");
            ++g_failures;
            return;
        }
    }
    if (!CryptGenKey(h_prov, AT_SIGNATURE, 0, &h_key)) {
        CryptReleaseContext(h_prov, 0);
        std::printf("FAIL: cannot generate key\n");
        ++g_failures;
        return;
    }
    DWORD info_len = 0;
    CryptExportPublicKeyInfo(h_prov, AT_SIGNATURE, X509_ASN_ENCODING, nullptr,
                             &info_len);
    std::vector<uint8_t> info_bytes(info_len);
    CryptExportPublicKeyInfo(h_prov, AT_SIGNATURE, X509_ASN_ENCODING,
                             reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(info_bytes.data()),
                             &info_len);
    DWORD der_len = 0;
    CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                        info_bytes.data(), 0, nullptr, nullptr, &der_len);
    std::vector<uint8_t> der(der_len);
    CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                        info_bytes.data(), 0, nullptr, der.data(), &der_len);
    const std::string pem =
        "-----BEGIN PUBLIC KEY-----\n" + b64_encode(der) +
        "\n-----END PUBLIC KEY-----\n";

    std::string sig_b64;
    std::vector<uint8_t> sig_raw;
    if (CryptCreateHash(h_prov, CALG_SHA_256, 0, 0, &h_hash) &&
        CryptHashData(h_hash, reinterpret_cast<const BYTE*>(canon.data()),
                      static_cast<DWORD>(canon.size()), 0)) {
        DWORD sig_len = 0;
        CryptSignHashA(h_hash, AT_SIGNATURE, nullptr, 0, nullptr, &sig_len);
        std::vector<uint8_t> sig(sig_len);
        if (CryptSignHashA(h_hash, AT_SIGNATURE, nullptr, 0, sig.data(), &sig_len)) {
            sig_raw = sig;
            sig_b64 = b64_encode(sig);
        }
    }
    if (h_hash) { CryptDestroyHash(h_hash); h_hash = 0; }
    if (h_key) { CryptDestroyKey(h_key); h_key = 0; }
    CryptReleaseContext(h_prov, 0);

    if (sig_b64.empty()) {
        std::printf("FAIL: cannot sign canonical manifest\n");
        ++g_failures;
        return;
    }

    std::string err;
    check(verify_manifest_signature(pem, canon, sig_b64, &err),
          "valid manifest signature verifies");

    // Regression: the release tooling (OpenSSL/Node) emits signatures in the
    // standard big-endian PKCS#1 order, the byte-reverse of CryptoAPI's
    // native little-endian order. Both encodings of the same signature must
    // verify, or real signed update feeds are rejected fail-closed.
    std::vector<uint8_t> reversed(sig_raw.rbegin(), sig_raw.rend());
    check(verify_manifest_signature(pem, canon, b64_encode(reversed), &err),
          "OpenSSL/Node byte-order signature verifies");

    // Mutating any security-relevant field invalidates the signature.
    UpdateInfo tampered = info;
    tampered.version = "3.0.2";
    check(!verify_manifest_signature(pem, canonical_manifest_json(tampered),
                                     sig_b64, &err),
          "version modification invalidates signature");
    tampered = info;
    tampered.download_url = "https://evil.example.com/amalgam.zip";
    check(!verify_manifest_signature(pem, canonical_manifest_json(tampered),
                                     sig_b64, &err),
          "URL modification invalidates signature");
    tampered = info;
    tampered.sha256 = std::string(64, 'b');
    check(!verify_manifest_signature(pem, canonical_manifest_json(tampered),
                                     sig_b64, &err),
          "hash modification invalidates signature");
    tampered = info;
    tampered.size = 9999;
    check(!verify_manifest_signature(pem, canonical_manifest_json(tampered),
                                     sig_b64, &err),
          "size modification invalidates signature");
    tampered = info;
    tampered.channel = "beta";
    check(!verify_manifest_signature(pem, canonical_manifest_json(tampered),
                                     sig_b64, &err),
          "channel modification invalidates signature");

    // Wrong key / garbage signature rejected.
    check(!verify_manifest_signature(
              "-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n",
              canon, sig_b64, &err),
          "wrong key rejected");
    check(!verify_manifest_signature(pem, canon, "bm90LWEtc2ln", &err),
          "bad signature rejected");
}

void test_stage_verification() {
    const std::wstring root = temp_dir();
    const std::wstring exe_dir = root + L"\\exe";
    std::filesystem::create_directories(exe_dir + L"\\updates");

    // A cached payload that matches size + SHA-256 is reused without network.
    const std::wstring cached = exe_dir + L"\\updates\\amalgam-9.9.9.zip";
    {
        std::ofstream f(cached, std::ios::binary);
        f << "payload-bytes";
    }
    aml::updater::UpdateInfo info;
    info.version = "9.9.9";
    info.download_url = "https://invalid.invalid/never.zip";
    info.size = 13;
    info.sha256 = aml::net::sha256_file(cached);
    std::wstring staged;
    std::string err;
    check(aml::updater::stage_update(info, exe_dir, staged, &err),
          "cached verified payload reused without network");
    check(staged == cached, "staged path matches cache");

    // A cached payload with the wrong hash is removed and not accepted.
    info.sha256 = std::string(64, '0');
    check(!aml::updater::stage_update(info, exe_dir, staged, &err),
          "hash mismatch rejected");
    check(!std::filesystem::exists(cached), "stale payload removed");

    // A signed manifest without a configured key fails closed (no network).
    {
        std::ofstream f(cached, std::ios::binary);
        f << "payload-bytes";
    }
    info.sha256 = aml::net::sha256_file(cached);
    info.signature = "QUJD";
    check(!aml::updater::stage_update(info, exe_dir, staged, &err),
          "signed manifest without key fails closed");
    check(err.find("no verification key") != std::string::npos,
          "fail-closed error explains the missing key");

    std::filesystem::remove_all(root);
}

// Generate an RSA keypair at test time, sign a payload, and verify it through
// the production verification path. No key material is shipped in the repo.
void test_signature_verification() {
    HCRYPTPROV h_prov = 0;
    HCRYPTKEY h_key = 0;
    HCRYPTHASH h_hash = 0;
    const wchar_t* container = L"amalgam-updater-test";

    if (!CryptAcquireContextW(&h_prov, container, MS_ENH_RSA_AES_PROV,
                              PROV_RSA_AES, CRYPT_NEWKEYSET)) {
        // Container may already exist from a previous run.
        if (!CryptAcquireContextW(&h_prov, container, MS_ENH_RSA_AES_PROV,
                                  PROV_RSA_AES, 0)) {
            std::printf("FAIL: cannot acquire crypto context\n");
            ++g_failures;
            return;
        }
    }
    if (!CryptGenKey(h_prov, AT_SIGNATURE, 0, &h_key)) {
        CryptReleaseContext(h_prov, 0);
        std::printf("FAIL: cannot generate key\n");
        ++g_failures;
        return;
    }

    // Export the public key as a PEM "BEGIN PUBLIC KEY" blob.
    DWORD info_len = 0;
    CryptExportPublicKeyInfo(h_prov, AT_SIGNATURE, X509_ASN_ENCODING, nullptr,
                             &info_len);
    std::vector<uint8_t> info_bytes(info_len);
    CryptExportPublicKeyInfo(h_prov, AT_SIGNATURE, X509_ASN_ENCODING,
                             reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(info_bytes.data()),
                             &info_len);
    DWORD der_len = 0;
    CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                        info_bytes.data(), 0, nullptr, nullptr, &der_len);
    std::vector<uint8_t> der(der_len);
    CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                        info_bytes.data(), 0, nullptr, der.data(), &der_len);
    const std::string pem =
        "-----BEGIN PUBLIC KEY-----\n" + b64_encode(der) +
        "\n-----END PUBLIC KEY-----\n";

    // Sign a payload file with RSA-SHA256.
    const std::wstring root = temp_dir();
    const std::wstring payload = root + L"\\payload.bin";
    {
        std::ofstream f(payload, std::ios::binary);
        f << "signed update payload content";
    }
    constexpr const char* kPayload = "signed update payload content";
    constexpr DWORD kPayloadLen = sizeof("signed update payload content") - 1;
    std::string sig_b64;
    if (CryptCreateHash(h_prov, CALG_SHA_256, 0, 0, &h_hash) &&
        CryptHashData(h_hash, reinterpret_cast<const BYTE*>(kPayload),
                      kPayloadLen, 0)) {
        DWORD sig_len = 0;
        CryptSignHashA(h_hash, AT_SIGNATURE, nullptr, 0, nullptr, &sig_len);
        std::vector<uint8_t> sig(sig_len);
        if (CryptSignHashA(h_hash, AT_SIGNATURE, nullptr, 0, sig.data(), &sig_len)) {
            sig_b64 = b64_encode(sig);
        }
    }
    if (h_hash) { CryptDestroyHash(h_hash); h_hash = 0; }
    if (h_key) { CryptDestroyKey(h_key); h_key = 0; }
    CryptReleaseContext(h_prov, 0);

    if (sig_b64.empty()) {
        std::filesystem::remove_all(root);
        std::printf("FAIL: cannot sign test payload\n");
        ++g_failures;
        return;
    }

    std::string err;
    if (!aml::updater::verify_payload_signature(pem, sig_b64, payload, &err)) {
        std::printf("signature verify failed: %s (last error %lu)\n", err.c_str(),
                    static_cast<unsigned long>(GetLastError()));
    }
    check(aml::updater::verify_payload_signature(pem, sig_b64, payload, &err),
          "valid signature verifies");

    // Tamper with the payload: verification must fail.
    {
        std::ofstream f(payload, std::ios::binary);
        f << "tampered update payload content";
    }
    check(!aml::updater::verify_payload_signature(pem, sig_b64, payload, &err),
          "tampered payload rejected");

    // Wrong key: verification must fail.
    check(!aml::updater::verify_payload_signature(
              "-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n",
              sig_b64, payload, &err),
          "invalid key rejected");

    std::filesystem::remove_all(root);
}

void test_rollback_without_backup() {
    std::string err_text;
    const std::wstring root = temp_dir();
    check(!aml::updater::rollback_update(root, err_text),
          "rollback without last-known-good fails safely");
    check(!err_text.empty(), "rollback failure explains itself");
    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    test_version_ordering();
    test_version_policy();
    test_stage_path_traversal_guard();
    test_manifest_signature();
    test_manifest_parsing();
    test_stage_verification();
    test_signature_verification();
    test_rollback_without_backup();

    if (g_failures == 0) {
        std::printf("updater tests passed\n");
        return 0;
    }
    std::printf("%d updater test(s) failed\n", g_failures);
    return 1;
}
