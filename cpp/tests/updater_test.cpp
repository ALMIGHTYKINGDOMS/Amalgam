// Self-updater unit tests.
//
// These tests deliberately never invoke a real launcher replacement: they
// exercise version ordering, manifest validation, staged-payload
// verification, the signature policy, RSA-SHA256 verification, and the
// generated multi-component apply/rollback plan against disposable fixtures.

#include "updater.h"

#include "extract.h"
#include "json.h"
#include "net.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
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

bool write_file(const std::wstring& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(f);
}

std::wstring path_under(const std::wstring& root, const std::string& relative) {
    return (std::filesystem::path(root) / std::filesystem::path(aml::net::to_wide(relative))).wstring();
}

std::wstring current_executable_name() {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return std::filesystem::path(self).filename().wstring();
}

enum class PayloadFixtureKind {
    Valid,
    UnsafeComponentPath,
    MalformedComponentManifest,
    UnlistedUserConfig,
};

std::vector<std::string> release_component_paths(const std::wstring& launcher_name) {
    return {
        aml::net::to_utf8(launcher_name),
        "amalgam.dll",
        "launcher.json.template",
        "prerequisites.json",
        "bridges/amalgam-fabric-test.jar",
        "bedrock/AmalgamBedrockClient.mcaddon",
        "bedrock/package-inventory.json",
        "bedrock/package-sha256.txt",
        "branding/amalgam-banner.png",
        "ai/ai-manifest.json",
        "legal/TERMS.txt",
        "tools/ai-bootstrap.cmd",
        "runtimes/ai/runtime.dll",
        "USER-GUIDE.md",
    };
}

bool write_component_manifest(const std::wstring& root,
                              const std::vector<std::string>& components) {
    aml::Json manifest = aml::Json::arr();
    for (const std::string& relative : components) {
        const std::wstring path = path_under(root, relative);
        aml::Json item = aml::Json::obj();
        item.set("name", aml::Json::str(relative));
        item.set("sha256", aml::Json::str(aml::net::sha256_file(path)));
        item.set("size", aml::Json(static_cast<int64_t>(std::filesystem::file_size(path))));
        manifest.push(std::move(item));
    }
    return write_file(path_under(root, "component-manifest.json"), manifest.dump());
}

bool write_release_hash_inventory(const std::wstring& root,
                                  const std::vector<std::string>& components) {
    std::vector<std::string> hashed = components;
    hashed.push_back("component-manifest.json");
    hashed.push_back("sbom.cdx.json");
    std::sort(hashed.begin(), hashed.end());

    std::string content;
    for (const std::string& relative : hashed) {
        const std::string hash = aml::net::sha256_file(path_under(root, relative));
        if (hash.size() != 64) return false;
        content += hash + "  " + relative + "\n";
    }
    return write_file(path_under(root, "release.sha256"), content);
}

bool create_release_payload(const std::wstring& source_root, const std::wstring& archive,
                            const std::wstring& launcher_name, PayloadFixtureKind kind,
                            std::vector<std::string>* out_components,
                            std::string* error) {
    std::error_code ec;
    std::filesystem::remove_all(source_root, ec);
    std::filesystem::create_directories(source_root, ec);
    if (ec) {
        if (error) *error = "cannot create disposable release fixture";
        return false;
    }

    std::vector<std::string> components = release_component_paths(launcher_name);
    if (kind == PayloadFixtureKind::UnsafeComponentPath) {
        components.push_back("profiles/do-not-touch.txt");
    }
    for (const std::string& relative : components) {
        if (!write_file(path_under(source_root, relative), "new release component: " + relative)) {
            if (error) *error = "cannot write disposable release component";
            return false;
        }
    }

    if (kind == PayloadFixtureKind::MalformedComponentManifest) {
        if (!write_file(path_under(source_root, "component-manifest.json"), "{}")) {
            if (error) *error = "cannot write malformed manifest fixture";
            return false;
        }
    } else if (!write_component_manifest(source_root, components)) {
        if (error) *error = "cannot write component manifest fixture";
        return false;
    }
    if (!write_file(path_under(source_root, "sbom.cdx.json"), "{}")) {
        if (error) *error = "cannot write SBOM fixture";
        return false;
    }
    if (!write_release_hash_inventory(source_root, components)) {
        if (error) *error = "cannot write release inventory fixture";
        return false;
    }
    if (kind == PayloadFixtureKind::UnlistedUserConfig &&
        !write_file(path_under(source_root, "launcher.json"), "{\"user\":true}")) {
        if (error) *error = "cannot write unsafe config fixture";
        return false;
    }
    if (!aml::extract::create_zip(source_root, archive, error)) return false;
    if (out_components) *out_components = std::move(components);
    return true;
}

struct PlannedComponent {
    std::string path;
    std::string action;
};

bool read_apply_plan(const std::wstring& path, std::vector<PlannedComponent>* out,
                     std::string* error) {
    if (!out) return false;
    out->clear();
    std::string parse_error;
    const aml::Json root = aml::Json::parse(read_file(path), &parse_error);
    if (!parse_error.empty() || !root.is(aml::Json::Type::Obj) ||
        !root.get("components").is(aml::Json::Type::Arr)) {
        if (error) *error = "generated apply plan is malformed";
        return false;
    }
    for (const aml::Json& item : root.get("components").items()) {
        if (!item.is(aml::Json::Type::Obj)) {
            if (error) *error = "generated apply plan has a malformed component";
            return false;
        }
        const std::string relative = item.get("path").as_str();
        const std::string action = item.get("action").as_str();
        if (relative.empty() || (action != "replace" && action != "delete")) {
            if (error) *error = "generated apply plan has an invalid component";
            return false;
        }
        out->push_back({relative, action});
    }
    return true;
}

bool plan_contains(const std::vector<PlannedComponent>& plan, const std::string& path,
                   const std::string& action) {
    for (const PlannedComponent& item : plan) {
        if (item.path == path && item.action == action) return true;
    }
    return false;
}

std::wstring state_marker(const std::wstring& backup, const wchar_t* kind, size_t index) {
    return backup + L"\\" + kind + L"\\entry-" + std::to_wstring(index) + L".marker";
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

// Ephemeral RSA material for signed-manifest fixtures.  The provider context
// is intentionally non-persistent so unit tests neither rely on nor leave a
// signing key on the machine.
struct SigningFixture {
    HCRYPTPROV provider = 0;
    HCRYPTKEY private_key = 0;
    std::string public_pem;

    ~SigningFixture() {
        if (private_key) CryptDestroyKey(private_key);
        if (provider) CryptReleaseContext(provider, 0);
    }

    bool create() {
        if (!CryptAcquireContextW(&provider, nullptr, MS_ENH_RSA_AES_PROV,
                                  PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
            !CryptGenKey(provider, AT_SIGNATURE, 0, &private_key)) {
            return false;
        }

        DWORD public_info_len = 0;
        if (!CryptExportPublicKeyInfo(provider, AT_SIGNATURE, X509_ASN_ENCODING,
                                      nullptr, &public_info_len) ||
            public_info_len == 0) {
            return false;
        }
        std::vector<uint8_t> public_info_bytes(public_info_len);
        if (!CryptExportPublicKeyInfo(
                provider, AT_SIGNATURE, X509_ASN_ENCODING,
                reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(public_info_bytes.data()),
                &public_info_len)) {
            return false;
        }

        DWORD der_len = 0;
        if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                                 public_info_bytes.data(), 0, nullptr, nullptr,
                                 &der_len) ||
            der_len == 0) {
            return false;
        }
        std::vector<uint8_t> der(der_len);
        if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
                                 public_info_bytes.data(), 0, nullptr, der.data(),
                                 &der_len)) {
            return false;
        }
        der.resize(der_len);
        public_pem = "-----BEGIN PUBLIC KEY-----\n" + b64_encode(der) +
                     "\n-----END PUBLIC KEY-----\n";
        return true;
    }

    bool sign(const std::string& data, std::vector<uint8_t>& signature) const {
        HCRYPTHASH hash = 0;
        bool ok = false;
        if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
            CryptHashData(hash, reinterpret_cast<const BYTE*>(data.data()),
                          static_cast<DWORD>(data.size()), 0)) {
            DWORD signature_len = 0;
            if (CryptSignHashA(hash, AT_SIGNATURE, nullptr, 0, nullptr,
                               &signature_len) &&
                signature_len > 0) {
                signature.resize(signature_len);
                if (CryptSignHashA(hash, AT_SIGNATURE, nullptr, 0, signature.data(),
                                   &signature_len)) {
                    signature.resize(signature_len);
                    ok = true;
                }
            }
        }
        if (hash) CryptDestroyHash(hash);
        return ok;
    }
};

bool make_signed_update_info(const std::wstring& archive, const SigningFixture& signer,
                             aml::updater::UpdateInfo* out) {
    if (!out) return false;
    aml::updater::UpdateInfo info;
    info.version = "9.9.9";
    info.channel = "stable";
    info.download_url = "https://updates.example.invalid/amalgam-9.9.9.zip";
    info.size = static_cast<int64_t>(std::filesystem::file_size(archive));
    info.sha256 = aml::net::sha256_file(archive);
    std::vector<uint8_t> signature;
    if (!signer.sign(aml::updater::canonical_manifest_json(info), signature)) return false;
    info.manifest_signature = b64_encode(signature);
    *out = std::move(info);
    return true;
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
    // A syntactically complete but unsigned manifest must never be trusted.
    const std::string unsigned_manifest =
        "{\"version\":\"3.0.1\",\"channel\":\"stable\","
        "\"download_url\":\"https://cdn.example.com/amalgam-3.0.1.zip\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"size\":1234,\"mandatory\":false,\"notes\":\"fixes\"}";
    aml::updater::UpdateInfo info;
    std::string err;
    check(!aml::updater::parse_manifest_text(unsigned_manifest, info, &err) &&
              err.find("required signature") != std::string::npos,
          "unsigned manifest rejected");

    check(!aml::updater::parse_manifest_text("{not json", info, &err),
          "malformed JSON rejected");
    // A host that serves its own web page instead of the feed (unpublished
    // manifest) must be rejected with that reason, not a parser error.
    check(!aml::updater::parse_manifest_text("\n  <!doctype html><html></html>", info,
                                             &err) &&
              err.find("web page") != std::string::npos,
          "HTML feed body named as a web page");
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
    check(!aml::updater::parse_manifest_text(
              "{\"version\":\"3.0.1\",\"download_url\":\"https://x/z.zip\","
              "\"size\":1234}",
              info, &err) &&
              err.find("sha256") != std::string::npos,
          "missing sha256 rejected");
    check(!aml::updater::parse_manifest_text(
              "{\"version\":\"3.0.1\",\"download_url\":\"https://x/z.zip\","
              "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
              info, &err) &&
              err.find("size") != std::string::npos,
          "missing payload size rejected");
    check(!aml::updater::parse_manifest_text(
              "{\"version\":\"3.0.1\",\"download_url\":\"https://x/z.zip\","
              "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
              "\"size\":0}",
              info, &err) &&
              err.find("size") != std::string::npos,
          "zero-sized payload rejected");
    check(!aml::updater::parse_manifest_text(
              "{\"version\":\"3.0.1\",\"download_url\":\"https://x/z.zip\","
              "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
              "\"size\":34359738369}",
              info, &err) &&
              err.find("size") != std::string::npos,
          "oversized payload rejected");
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

    // Exercise the production parsing gate with a real signed fixture.  The
    // test key is configured only for this fixture; production obtains the
    // equivalent public key from its build-time embedded value.
    const std::string signed_manifest =
        std::string("{\"version\":\"3.0.1\",\"channel\":\"stable\",") +
        "\"download_url\":\"https://cdn.example.com/amalgam-3.0.1.zip\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"size\":1234,\"min_version\":\"3.0\",\"notes\":\"fixes\","
        "\"mandatory\":false,\"manifest_signature\":\"" + sig_b64 + "\"}";
    UpdateInfo parsed;
    aml::updater::set_signing_public_key(pem);
    check(aml::updater::parse_manifest_text(signed_manifest, parsed, &err),
          "signed manifest fixture parses with configured key");
    check(parsed.version == info.version && parsed.sha256 == info.sha256 &&
              parsed.size == info.size,
          "signed manifest preserves security-relevant fields");

    aml::updater::set_signing_public_key("");
    check(!aml::updater::parse_manifest_text(signed_manifest, parsed, &err) &&
              err.find("no update-signing public key") != std::string::npos,
          "signed manifest without configured key rejected");
    aml::updater::set_signing_public_key(pem);

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

    // Avoid leaking the test-only key into independent tests.
    aml::updater::set_signing_public_key("");
}

void test_stage_verification() {
    const std::wstring root = temp_dir();
    const std::wstring exe_dir = root + L"\\exe";
    std::filesystem::create_directories(exe_dir + L"\\updates");

    SigningFixture signer;
    if (!signer.create()) {
        std::printf("FAIL: cannot generate ephemeral manifest signing key\n");
        ++g_failures;
        std::filesystem::remove_all(root);
        return;
    }

    auto sign_manifest = [&](aml::updater::UpdateInfo& manifest) {
        std::vector<uint8_t> raw_signature;
        if (!signer.sign(aml::updater::canonical_manifest_json(manifest), raw_signature)) {
            check(false, "can sign staged-manifest fixture");
            return false;
        }
        manifest.manifest_signature = b64_encode(raw_signature);
        return true;
    };

    // A cached payload that matches size + SHA-256 is reused without network.
    const std::wstring cached = exe_dir + L"\\updates\\amalgam-9.9.9.zip";
    {
        std::ofstream f(cached, std::ios::binary);
        f << "payload-bytes";
    }
    aml::updater::UpdateInfo info;
    info.version = "9.9.9";
    info.download_url = "https://invalid.invalid/never.zip";
    info.size = static_cast<int64_t>(std::filesystem::file_size(cached));
    info.sha256 = aml::net::sha256_file(cached);
    std::wstring staged;
    std::string err;
    if (!sign_manifest(info)) {
        std::filesystem::remove_all(root);
        return;
    }
    aml::updater::set_signing_public_key(signer.public_pem);
    check(aml::updater::stage_update(info, exe_dir, staged, &err),
          "cached signed payload reused without network");
    check(staged == cached, "staged path matches cache");

    // A cached payload with the wrong (but authentically signed) hash is
    // removed and not accepted. The credential-bearing URL is rejected by
    // net::download before it can perform a network request.
    info.sha256 = std::string(64, '0');
    info.download_url = "https://blocked@invalid.example/never.zip";
    if (!sign_manifest(info)) {
        aml::updater::set_signing_public_key("");
        std::filesystem::remove_all(root);
        return;
    }
    check(!aml::updater::stage_update(info, exe_dir, staged, &err),
          "hash mismatch rejected");
    check(!std::filesystem::exists(cached), "stale payload removed");

    // Restore a valid signed fixture, then prove direct callers cannot bypass
    // the parser by constructing UpdateInfo without a manifest signature.
    {
        std::ofstream f(cached, std::ios::binary);
        f << "payload-bytes";
    }
    info.download_url = "https://invalid.invalid/never.zip";
    info.sha256 = aml::net::sha256_file(cached);
    if (!sign_manifest(info)) {
        aml::updater::set_signing_public_key("");
        std::filesystem::remove_all(root);
        return;
    }
    aml::updater::UpdateInfo unsigned_info = info;
    unsigned_info.manifest_signature.clear();
    check(!aml::updater::stage_update(unsigned_info, exe_dir, staged, &err) &&
              err.find("required signature") != std::string::npos,
          "unsigned UpdateInfo cannot bypass staging policy");

    // A cryptographically signed manifest without the configured release key
    // must fail before cached data can be accepted or a download can begin.
    aml::updater::set_signing_public_key("");
    check(!aml::updater::stage_update(info, exe_dir, staged, &err),
          "signed manifest without configured key fails closed");
    check(err.find("no update-signing public key") != std::string::npos,
          "fail-closed error explains the missing key");

    std::filesystem::remove_all(root);
}

void test_full_component_apply_and_rollback_plan() {
    const std::wstring root = temp_dir();
    const std::wstring install = root + L"\\install";
    const std::wstring source = root + L"\\release-source";
    const std::wstring archive = root + L"\\release.zip";
    const std::wstring launcher_name = current_executable_name();
    std::filesystem::create_directories(install);

    // These are deliberately outside the update allowlist. Preparation and a
    // synthetic rollback must leave them byte-for-byte alone.
    check(write_file(path_under(install, "launcher.json"), "user-config"),
          "write disposable user config");
    check(write_file(path_under(install, "profiles/user-profile.txt"), "profile-data"),
          "write disposable profile data");
    check(write_file(path_under(install, "instances/user-instance.txt"), "instance-data"),
          "write disposable instance data");
    check(write_file(path_under(install, "updates/user-staging.txt"), "update-work-data"),
          "write disposable update work data");

    // A previous package component intentionally disappears from the new
    // release. The generated plan must remove it on apply and restore it on
    // rollback, rather than leaving a stale bridge behind.
    check(write_file(path_under(install, "bridges/obsolete.jar"), "old obsolete bridge"),
          "write obsolete bridge fixture");
    check(write_component_manifest(install, {"bridges/obsolete.jar"}),
          "write prior component manifest fixture");

    std::vector<std::string> release_components;
    std::string fixture_error;
    if (!create_release_payload(source, archive, launcher_name, PayloadFixtureKind::Valid,
                                &release_components, &fixture_error)) {
        check(false, "create disposable multi-file release payload");
        std::filesystem::remove_all(root);
        return;
    }

    SigningFixture signer;
    if (!signer.create()) {
        check(false, "create update signing fixture for component plan");
        std::filesystem::remove_all(root);
        return;
    }
    aml::updater::set_signing_public_key(signer.public_pem);
    aml::updater::UpdateInfo info;
    if (!make_signed_update_info(archive, signer, &info)) {
        check(false, "sign disposable multi-file update payload");
        aml::updater::set_signing_public_key("");
        std::filesystem::remove_all(root);
        return;
    }

    std::wstring helper;
    std::string error;
    const bool prepared = aml::updater::prepare_apply(info, install, archive, helper, &error);
    check(prepared, "signed multi-file release payload prepares transactionally");
    if (prepared) {
        const std::wstring plan_path = install + L"\\updates\\apply-plan.json";
        std::vector<PlannedComponent> plan;
        check(read_apply_plan(plan_path, &plan, &error), "generated component apply plan parses");
        if (!plan.empty()) {
            for (const std::string& component : release_components) {
                check(plan_contains(plan, component, "replace"),
                      ("apply plan replaces " + component).c_str());
            }
            check(plan_contains(plan, "component-manifest.json", "replace"),
                  "apply plan replaces component manifest");
            check(plan_contains(plan, "sbom.cdx.json", "replace"),
                  "apply plan replaces SBOM");
            check(plan_contains(plan, "release.sha256", "replace"),
                  "apply plan replaces release hash inventory");
            check(plan_contains(plan, "bridges/obsolete.jar", "delete"),
                  "apply plan removes stale prior bridge");
            check(!plan_contains(plan, "launcher.json", "replace"),
                  "apply plan never replaces live launcher config");
            check(!plan_contains(plan, "profiles/user-profile.txt", "replace"),
                  "apply plan never replaces profiles");
            check(!plan_contains(plan, "instances/user-instance.txt", "replace"),
                  "apply plan never replaces instances");

            const std::string helper_text = read_file(helper);
            check(helper_text.find("transactional full-release") != std::string::npos,
                  "generated helper identifies a full release transaction");
            check(helper_text.find("amalgam.dll") != std::string::npos,
                  "generated helper applies DLL component");
            check(helper_text.find("bridges\\amalgam-fabric-test.jar") != std::string::npos,
                  "generated helper applies bridge component");
            check(helper_text.find("bedrock\\AmalgamBedrockClient.mcaddon") != std::string::npos,
                  "generated helper applies Bedrock component");
            check(helper_text.find("branding\\amalgam-banner.png") != std::string::npos,
                  "generated helper applies packaged branding asset");
            check(helper_text.find("runtimes\\ai\\runtime.dll") != std::string::npos,
                  "generated helper applies packaged runtime asset");
            check(helper_text.find(".ready\\entry-") != std::string::npos &&
                      helper_text.find(".absent\\entry-") != std::string::npos,
                  "generated helper records rollback state for every component");

            // Do not execute the helper. Instead synthesize the exact backup
            // state it would create, then call the production rollback API on
            // this disposable install root. This proves every planned product
            // file is restored without touching a real installation.
            const std::wstring backup = install + L"\\updates\\last-known-good";
            std::filesystem::create_directories(backup + L"\\.ready");
            std::filesystem::create_directories(backup + L"\\.absent");
            std::filesystem::copy_file(plan_path, backup + L"\\component-plan.json",
                                       std::filesystem::copy_options::overwrite_existing);
            const std::string absent_before_update = "branding/amalgam-banner.png";
            for (size_t index = 0; index < plan.size(); ++index) {
                const PlannedComponent& component = plan[index];
                const std::wstring target = path_under(install, component.path);
                if (component.action == "delete") {
                    std::error_code remove_error;
                    std::filesystem::remove(std::filesystem::path(target), remove_error);
                    check(write_file(path_under(backup, component.path),
                                     "old component: " + component.path),
                          "write deleted-component rollback backup");
                } else {
                    check(write_file(target, "new component: " + component.path),
                          "write updated-component fixture");
                    if (component.path == absent_before_update) {
                        check(write_file(state_marker(backup, L".absent", index), "absent"),
                              "write absent-before-update marker");
                    } else {
                        check(write_file(path_under(backup, component.path),
                                         "old component: " + component.path),
                              "write replacement rollback backup");
                    }
                }
                check(write_file(state_marker(backup, L".ready", index), "ready"),
                      "write completed component snapshot marker");
            }

            std::string rollback_error;
            check(aml::updater::rollback_update(install, rollback_error),
                  "multi-component rollback restores disposable release plan");
            for (const PlannedComponent& component : plan) {
                const std::wstring target = path_under(install, component.path);
                if (component.path == absent_before_update) {
                    check(!std::filesystem::exists(std::filesystem::path(target)),
                          "rollback removes component absent before update");
                } else {
                    check(read_file(target) == "old component: " + component.path,
                          ("rollback restores " + component.path).c_str());
                }
            }
            check(read_file(path_under(install, "launcher.json")) == "user-config",
                  "rollback preserves live launcher configuration");
            check(read_file(path_under(install, "profiles/user-profile.txt")) == "profile-data",
                  "rollback preserves profiles");
            check(read_file(path_under(install, "instances/user-instance.txt")) == "instance-data",
                  "rollback preserves instances");
            check(read_file(path_under(install, "updates/user-staging.txt")) == "update-work-data",
                  "rollback preserves update working files outside its own backup");
        }
    }

    aml::updater::set_signing_public_key("");
    std::filesystem::remove_all(root);
}

void test_rejects_unsafe_release_component_payloads() {
    SigningFixture signer;
    if (!signer.create()) {
        check(false, "create update signing fixture for malformed payload tests");
        return;
    }
    aml::updater::set_signing_public_key(signer.public_pem);

    const std::vector<std::pair<PayloadFixtureKind, const char*>> cases = {
        {PayloadFixtureKind::UnsafeComponentPath, "protected component path is rejected"},
        {PayloadFixtureKind::MalformedComponentManifest, "malformed component manifest is rejected"},
        {PayloadFixtureKind::UnlistedUserConfig, "unlisted live configuration file is rejected"},
    };
    for (size_t index = 0; index < cases.size(); ++index) {
        const std::wstring root = temp_dir();
        const std::wstring install = root + L"\\install";
        const std::wstring source = root + L"\\source";
        const std::wstring archive = root + L"\\payload.zip";
        std::filesystem::create_directories(install);
        check(write_file(path_under(install, "launcher.json"), "preserved-user-config"),
              "write protected config for malformed payload fixture");

        std::string fixture_error;
        const bool created = create_release_payload(source, archive, current_executable_name(),
                                                    cases[index].first, nullptr,
                                                    &fixture_error);
        check(created, "create malformed component payload fixture");
        if (created) {
            aml::updater::UpdateInfo info;
            const bool signed_info = make_signed_update_info(archive, signer, &info);
            check(signed_info, "sign malformed component payload fixture");
            if (signed_info) {
                std::wstring helper;
                std::string error;
                check(!aml::updater::prepare_apply(info, install, archive, helper, &error),
                      cases[index].second);
                check(!error.empty(), "unsafe release payload refusal explains itself");
                check(!std::filesystem::exists(install + L"\\updates\\apply-update.bat"),
                      "unsafe release payload never emits an update helper");
                check(read_file(path_under(install, "launcher.json")) == "preserved-user-config",
                      "unsafe release payload leaves live config untouched");
            }
        }
        std::filesystem::remove_all(root);
    }

    aml::updater::set_signing_public_key("");
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
    test_full_component_apply_and_rollback_plan();
    test_rejects_unsafe_release_component_payloads();
    test_signature_verification();
    test_rollback_without_backup();

    if (g_failures == 0) {
        std::printf("updater tests passed\n");
        return 0;
    }
    std::printf("%d updater test(s) failed\n", g_failures);
    return 1;
}
