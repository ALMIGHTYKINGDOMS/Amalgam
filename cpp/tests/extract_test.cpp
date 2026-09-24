#include "extract.h"

#include <iostream>
#include <string>

int main() {
    // quote(): plain path unchanged
    {
        std::wstring r = aml::extract::quote(L"C:\\foo\\bar.txt");
        if (r != L"C:\\foo\\bar.txt") {
            std::cerr << "quote() altered plain path\n";
            return 1;
        }
    }
    // quote(): path with space gets quoted
    {
        std::wstring r = aml::extract::quote(L"C:\\my folder\\bar.txt");
        if (r != L"\"C:\\my folder\\bar.txt\"") {
            std::cerr << "quote() did not quote path with space\n";
            return 1;
        }
    }
    // quote(): path with inner quote gets escaped
    {
        std::wstring r = aml::extract::quote(L"C:\\foo\\\"bar\".txt");
        if (r.find(L"\\\"") == std::wstring::npos) {
            std::cerr << "quote() did not escape inner quote\n";
            return 1;
        }
    }
    // at_file_argument(): Java-style response files keep the @ prefix outside
    // the quoted path, so Windows passes one argument whose first byte is @.
    {
        std::wstring r = aml::extract::at_file_argument(L"C:\\Server Folder\\win_args.txt");
        if (r != L"@\"C:\\Server Folder\\win_args.txt\"") {
            std::cerr << "at_file_argument() did not preserve a spaced @ path\n";
            return 1;
        }
    }
    {
        std::wstring r = aml::extract::at_file_argument(L"C:\\Server\\win_args.txt");
        if (r != L"@C:\\Server\\win_args.txt") {
            std::cerr << "at_file_argument() changed a plain @ path\n";
            return 1;
        }
    }
    // parent_of(): normal path
    {
        std::wstring r = aml::extract::parent_of(L"C:\\foo\\bar\\baz.txt");
        if (r != L"C:\\foo\\bar") {
            std::cerr << "parent_of() failed for normal path\n";
            return 1;
        }
    }
    // parent_of(): no separator returns empty
    {
        std::wstring r = aml::extract::parent_of(L"file.txt");
        if (!r.empty()) {
            std::cerr << "parent_of() should return empty for no separator\n";
            return 1;
        }
    }
    // parent_of(): single-level path
    {
        std::wstring r = aml::extract::parent_of(L"C:\\bar.txt");
        if (r != L"C:") {
            std::cerr << "parent_of() failed for single-level path\n";
            return 1;
        }
    }
    // run_command(): echo succeeds
    {
        int exit_code = -1;
        std::string err;
        bool ok = aml::extract::run_command(L"C:\\Windows\\System32\\cmd.exe", L"/c echo ok", L"", 5000, &exit_code, &err);
        if (!ok || exit_code != 0) {
            std::cerr << "run_command() echo failed: " << err << "\n";
            return 1;
        }
    }
    // run_command(): nonexistent exe fails
    {
        int exit_code = -1;
        std::string err;
        bool ok = aml::extract::run_command(L"C:\\Windows\\System32\\nonexistent_xyz.exe", L"", L"", 5000, &exit_code, &err);
        if (ok) {
            std::cerr << "run_command() should fail for nonexistent exe\n";
            return 1;
        }
    }
    // run_command(): timeout kills process (use ping which has no stdin dependency)
    {
        int exit_code = -1;
        std::string err;
        bool ok = aml::extract::run_command(L"C:\\Windows\\System32\\cmd.exe",
            L"/c ping -n 11 127.0.0.1 >nul", L"", 500, &exit_code, &err);
        if (ok) {
            std::cerr << "run_command() should fail on timeout\n";
            return 1;
        }
    }
    // run_capture(): capture stdout
    {
        std::string output;
        std::string err;
        bool ok = aml::extract::run_capture(L"C:\\Windows\\System32\\cmd.exe",
            L"/c echo hello world", &output, &err, 5000);
        if (!ok) {
            std::cerr << "run_capture() failed: " << err << "\n";
            return 1;
        }
        if (output.find("hello world") == std::string::npos) {
            std::cerr << "run_capture() did not capture expected output\n";
            return 1;
        }
    }
    // run_capture(): timeout returns failure
    {
        std::string output;
        std::string err;
        bool ok = aml::extract::run_capture(L"C:\\Windows\\System32\\cmd.exe",
            L"/c ping -n 11 127.0.0.1 >nul", &output, &err, 500);
        if (ok) {
            std::cerr << "run_capture() should fail on timeout\n";
            return 1;
        }
    }
    std::cout << "extract_test passed\n";
    return 0;
}
