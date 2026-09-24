#pragma once

#include <string>
#include <vector>

namespace aml::extract {

bool run_command(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
                 long timeout_ms, int* exit_code, std::string* err,
                 std::string* output = nullptr);

bool run_capture(const std::wstring& exe, const std::wstring& args, std::string* output,
                 std::string* err, long timeout_ms = 120000);

bool zip(const std::wstring& archive, const std::wstring& out_dir, std::string* err);
// Extracts an archive while omitting archive-relative entries named by the
// version metadata's extract.exclude list.  Rules are validated before any
// output is promoted, and use exact-or-directory-prefix matching.
bool zip_excluding(const std::wstring& archive, const std::wstring& out_dir,
                   const std::vector<std::string>& exclusions, std::string* err);
bool create_zip(const std::wstring& source_dir, const std::wstring& archive, std::string* err);

std::wstring quote(const std::wstring& s);
// A response-file token for command lines such as Java's @argfile syntax. The
// @ prefix stays outside any Windows quotes so the child receives one argument
// whose first character is @.
std::wstring at_file_argument(const std::wstring& path);
std::wstring parent_of(const std::wstring& path);

}  // namespace aml::extract
