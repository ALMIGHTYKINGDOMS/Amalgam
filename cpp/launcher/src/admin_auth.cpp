#include "admin_auth.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace aml::admin_auth {

namespace {

constexpr uint32_t kDefaultIterations = 210000;
constexpr size_t kSaltBytes = 16;
constexpr size_t kVerifierBytes = 32;
constexpr size_t kMinimumPasswordLength = 8;
constexpr char kHex[] = "0123456789abcdef";

std::string encode_hex(const uint8_t* bytes, size_t size) {
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0f]);
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool decode_hex(std::string_view encoded, std::vector<uint8_t>& out) {
    if (encoded.empty() || encoded.size() % 2 != 0) return false;
    out.resize(encoded.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int high = hex_value(encoded[i * 2]);
        const int low = hex_value(encoded[i * 2 + 1]);
        if (high < 0 || low < 0) {
            out.clear();
            return false;
        }
        out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool derive(const std::string& password, const std::vector<uint8_t>& salt,
            uint32_t iterations, std::vector<uint8_t>& verifier) {
    if (password.empty() || salt.empty() || iterations == 0) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return false;
    verifier.assign(kVerifierBytes, 0);
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()),
        iterations,
        verifier.data(),
        static_cast<ULONG>(verifier.size()),
        0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) {
        verifier.clear();
        return false;
    }
    return true;
}

bool constant_time_equal(const std::vector<uint8_t>& left,
                         const std::vector<uint8_t>& right) {
    if (left.size() != right.size()) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < left.size(); ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

}  // namespace

bool configured(const config::Config& cfg) {
    return !cfg.admin_salt.empty() && !cfg.admin_verifier.empty() &&
           cfg.admin_iterations >= 100000;
}

bool set_password(config::Config& cfg, const std::string& password, std::string* error) {
    if (password.size() < kMinimumPasswordLength) {
        if (error) *error = "Use at least 8 characters for the Admin password.";
        return false;
    }
    std::array<uint8_t, kSaltBytes> salt{};
    if (BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        if (error) *error = "Windows could not create a secure password salt.";
        return false;
    }
    std::vector<uint8_t> salt_vector(salt.begin(), salt.end());
    std::vector<uint8_t> verifier;
    if (!derive(password, salt_vector, kDefaultIterations, verifier)) {
        if (error) *error = "Windows could not derive the Admin password verifier.";
        return false;
    }
    cfg.admin_salt = encode_hex(salt.data(), salt.size());
    cfg.admin_verifier = encode_hex(verifier.data(), verifier.size());
    cfg.admin_iterations = kDefaultIterations;
    return true;
}

bool verify_password(const config::Config& cfg, const std::string& password) {
    if (!configured(cfg)) return false;
    std::vector<uint8_t> salt;
    std::vector<uint8_t> expected;
    if (!decode_hex(cfg.admin_salt, salt) || salt.size() < 8 ||
        !decode_hex(cfg.admin_verifier, expected) || expected.size() != kVerifierBytes)
        return false;
    std::vector<uint8_t> actual;
    if (!derive(password, salt, cfg.admin_iterations, actual)) return false;
    return constant_time_equal(actual, expected);
}

void clear_password(config::Config& cfg) {
    cfg.admin_salt.clear();
    cfg.admin_verifier.clear();
    cfg.admin_iterations = kDefaultIterations;
}

}  // namespace aml::admin_auth
