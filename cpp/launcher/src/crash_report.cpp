#include "crash_report.h"
#include "net.h"

#include <windows.h>
#include <dbghelp.h>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "dbghelp.lib")

namespace {

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) {
    const std::wstring dump_dir = aml::net::get_local_app_data_path() + L"\\Amalgam\\crashes";
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t filename[64];
    std::swprintf(filename, 64, L"crash_%04d%02d%02d_%02d%02d%02d.dmp",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    const std::wstring dump_path = dump_dir + L"\\" + filename;

    HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          file, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(file);
    }

    const DWORD max_dumps = 10;
    std::vector<std::filesystem::directory_entry> entries;
    for (const auto& e : std::filesystem::directory_iterator(dump_dir)) {
        if (e.is_regular_file() && e.path().extension() == L".dmp")
            entries.push_back(e);
    }
    if (entries.size() > max_dumps) {
        std::sort(entries.begin(), entries.end(),
                  [](const std::filesystem::directory_entry& a,
                     const std::filesystem::directory_entry& b) {
                      return a.last_write_time() < b.last_write_time();
                  });
        const size_t to_remove = entries.size() - max_dumps;
        for (size_t i = 0; i < to_remove; ++i)
            std::filesystem::remove(entries[i].path(), ec);
    }

    std::wstring msg = L"Amalgam has crashed. A crash report has been saved to:\n" + dump_path;
    MessageBoxW(nullptr, msg.c_str(), L"Amalgam", MB_OK | MB_ICONERROR);

    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void aml::crash::install() {
    SetUnhandledExceptionFilter(unhandled_exception_filter);
}

void aml::crash::uninstall() {
    SetUnhandledExceptionFilter(nullptr);
}
