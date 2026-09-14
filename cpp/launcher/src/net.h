#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aml::net {

using Progress = std::function<bool(uint64_t done, uint64_t total)>;

bool validate_url(const std::wstring& url, std::string* err = nullptr);

bool get(const std::wstring& url, std::vector<uint8_t>& out, std::string* err);
bool get_with_headers(const std::wstring& url, const std::vector<std::wstring>& headers,
                      std::vector<uint8_t>& out, std::string* err);
bool request(const std::wstring& url, const std::wstring& method,
             const std::vector<std::wstring>& headers, const std::vector<uint8_t>& body,
             std::vector<uint8_t>& out, std::string* err);
bool post(const std::wstring& url, const std::vector<std::wstring>& headers,
          const std::vector<uint8_t>& body, std::vector<uint8_t>& out, std::string* err);
bool put(const std::wstring& url, const std::vector<std::wstring>& headers,
         const std::vector<uint8_t>& body, std::vector<uint8_t>& out, std::string* err);
bool del(const std::wstring& url, const std::vector<std::wstring>& headers,
         std::vector<uint8_t>& out, std::string* err);
// download() with optional integrity verification: sha1 (hex) and/or exact size.
bool download(const std::wstring& url, const std::wstring& path, Progress progress, std::string* err,
              const std::string& expected_sha1 = std::string(), int64_t expected_size = -1,
              const std::string& expected_sha256 = std::string());
// true when the file exists and matches sha1 (if given) and size (if given).
bool verify_file(const std::wstring& path, const std::string& sha1, int64_t size);
std::string sha1_file(const std::wstring& path);
std::string sha256_hex(const void* data, size_t len);
std::string sha256_file(const std::wstring& path);
std::string sha1_hex(const void* data, size_t len);
bool mkdirs(const std::wstring& path);
bool file_exists(const std::wstring& path);
bool directory_exists(const std::wstring& path);
uint64_t file_size(const std::wstring& path);

std::string to_utf8(const std::wstring& w);
std::wstring to_wide(const std::string& s);
std::wstring get_local_app_data_path();

}  // namespace aml::net
