#pragma once

#include <string>

namespace aml::extract {

bool run_command(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
                 long timeout_ms, int* exit_code, std::string* err,
                 std::string* output = nullptr);

bool run_capture(const std::wstring& exe, const std::wstring& args, std::string* output,
                 std::string* err, long timeout_ms = 120000);

bool zip(const std::wstring& archive, const std::wstring& out_dir, std::string* err);
bool create_zip(const std::wstring& source_dir, const std::wstring& archive, std::string* err);

std::wstring quote(const std::wstring& s);
std::wstring parent_of(const std::wstring& path);

}  // namespace aml::extract
