// =============================================================================
// ai_install.cpp — AI model manifest parser + background download manager
// =============================================================================

#include "ai_install.h"
#include "json.h"
#include "net.h"

#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace aml::ai_install {

static std::wstring default_models_dir();

InstallManager& InstallManager::instance() {
    static InstallManager s;
    return s;
}

InstallManager::~InstallManager() {
    cancel();
    join_worker();
}

void InstallManager::join_worker() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(m_worker_mu);
        if (m_worker.joinable()) worker = std::move(m_worker);
    }
    if (worker.joinable()) worker.join();
}

static std::string to_upper_hex(const std::string& hex) {
    std::string out = hex;
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

static bool safe_manifest_relative(const std::string& value) {
    if (value.empty() || value.front() == '/' || value.front() == '\\' ||
        (value.size() >= 2 && value[1] == ':')) return false;
    std::string segment;
    for (size_t i = 0; i <= value.size(); ++i) {
        const char c = i < value.size() ? value[i] : '/';
        if (c == '/' || c == '\\') {
            if (segment.empty() || segment == "." || segment == "..") return false;
            segment.clear();
        } else {
            if (static_cast<unsigned char>(c) < 32 || c == ':') return false;
            segment += c;
        }
    }
    return !segment.empty();
}

static bool https_url(const std::string& value) {
    constexpr char prefix[] = "https://";
    if (value.size() <= sizeof(prefix) - 1) return false;
    return std::equal(std::begin(prefix), std::end(prefix) - 1, value.begin(),
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

bool InstallManager::load_manifest(const std::wstring& manifest_path) {
    std::unique_lock<std::mutex> worker_lock(m_worker_mu);
    if (m_running.load(std::memory_order_acquire) ||
        m_verifying.load(std::memory_order_acquire)) return false;
    if (m_worker.joinable()) m_worker.join();
    std::lock_guard lock(m_mu);
    m_manifest = Manifest{};
    m_loaded = false;
    m_manifest_path.clear();
    m_knowledge_dir.clear();

    Json root;
    std::string err;
    if (!json_parse_file(manifest_path, root, &err)) return false;
    if (root.get("schema_version").as_int(0) != 2) return false;

    Manifest parsed;
    parsed.schema_version = 2;
    parsed.ai_package = root.get("ai_package").as_str();
    parsed.runtime_version = root.get("runtime_version").as_str();
    const Json& comps = root.get("components");
    if (!comps.isObject()) return false;
    for (const auto& item : comps.pairs()) {
        const std::string& id = item.first;
        const Json& obj = item.second;
        Component c;
        c.id = id;
        c.name = obj.get("name").as_str();
        c.filename = obj.get("filename").as_str();
        c.destination = obj.get("destination").as_str();
        c.download_url = obj.get("download_url").as_str();
        c.sha256 = obj.get("sha256").as_str();
        c.size_bytes = obj.get("size_bytes").as_int(0);
        c.required = obj.get("required").as_bool(true);
        if (c.filename.empty() || !safe_manifest_relative(c.destination) ||
            !safe_manifest_relative(c.filename) || c.size_bytes < 0 ||
            (id != "knowledge" && !https_url(c.download_url))) return false;
        parsed.total_bytes += c.size_bytes;
        parsed.components.push_back(std::move(c));
    }
    if (parsed.components.empty()) return false;
    m_manifest = std::move(parsed);
    m_manifest_path = manifest_path;
    m_knowledge_dir = (fs::path(manifest_path).parent_path() / L"knowledge").wstring();
    m_loaded = true;
    return true;
}

std::wstring InstallManager::component_path(const Component& comp,
                                            const std::wstring& models_dir) {
    const fs::path destination = fs::path(models_dir) / net::to_wide(comp.destination);
    if (comp.id == "knowledge") return destination.wstring();
    return (destination / net::to_wide(comp.filename)).wstring();
}

static std::wstring default_models_dir() {
    wchar_t local_app_data[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data, static_cast<DWORD>(std::size(local_app_data)));
    if (length == 0 || length >= std::size(local_app_data)) return {};
    return (fs::path(local_app_data) / L"Amalgam" / L"AI" / L"Models").wstring();
}

bool InstallManager::verify_file(const std::wstring& path, const Component& comp) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return false;
    const auto size = fs::file_size(path, ec);
    if (ec || (comp.size_bytes > 0 && static_cast<int64_t>(size) != comp.size_bytes)) return false;
    if (!comp.sha256.empty() && to_upper_hex(net::sha256_file(path)) != to_upper_hex(comp.sha256)) return false;
    return true;
}

void InstallManager::verify_component(int index) {
    Component component;
    std::wstring models_dir;
    {
        std::lock_guard lock(m_mu);
        if (m_models_dir.empty()) m_models_dir = default_models_dir();
        if (index < 0 || index >= static_cast<int>(m_manifest.components.size())) return;
        component = m_manifest.components[index];
        models_dir = m_models_dir;
    }

    const bool installed = component.id == "knowledge"
        ? fs::is_directory(fs::path(models_dir) / net::to_wide(component.destination))
        : verify_file(component_path(component, models_dir), component);
    {
        std::lock_guard lock(m_mu);
        if (index >= 0 && index < static_cast<int>(m_manifest.components.size())) {
            m_manifest.components[index].status = installed
                ? ComponentStatus::Installed : ComponentStatus::Missing;
        }
    }
}

void InstallManager::verify_all() {
    std::vector<Component> components;
    std::wstring models_dir;
    {
        std::lock_guard lock(m_mu);
        if (m_models_dir.empty()) m_models_dir = default_models_dir();
        components = m_manifest.components;
        models_dir = m_models_dir;
    }

    std::vector<ComponentStatus> statuses;
    statuses.reserve(components.size());
    for (const auto& component : components) {
        const bool installed = component.id == "knowledge"
            ? fs::is_directory(fs::path(models_dir) / net::to_wide(component.destination))
            : verify_file(component_path(component, models_dir), component);
        statuses.push_back(installed ? ComponentStatus::Installed : ComponentStatus::Missing);
    }
    {
        std::lock_guard lock(m_mu);
        const size_t count = std::min(statuses.size(), m_manifest.components.size());
        for (size_t i = 0; i < count; ++i) m_manifest.components[i].status = statuses[i];
    }
}

std::wstring InstallManager::models_dir() const {
    std::lock_guard lock(m_mu);
    return m_models_dir;
}

Manifest InstallManager::manifest_snapshot() const {
    std::lock_guard lock(m_mu);
    return m_manifest;
}

std::vector<Component> InstallManager::snapshot() const {
    std::lock_guard lock(m_mu);
    return m_manifest.components;
}

bool InstallManager::all_installed() const {
    std::lock_guard lock(m_mu);
    if (!m_loaded || m_manifest.components.empty()) return false;
    for (const auto& c : m_manifest.components)
        if (c.required && c.status != ComponentStatus::Installed) return false;
    return true;
}

void InstallManager::cancel() {
    m_cancel.store(true, std::memory_order_release);
}

bool InstallManager::download_component(const Component& comp, const std::wstring& dest_path) {
    fs::path dest(dest_path);
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) return false;
    if (verify_file(dest_path, comp)) return true;

    // Use the shared bounded downloader. It supports Range resume, TLS, size
    // limits, retry/backoff, and atomic .part finalization.
    return net::download(net::to_wide(comp.download_url), dest_path,
        [&](uint64_t done, uint64_t total) {
            if (m_cancel.load(std::memory_order_acquire)) return false;
            std::lock_guard lock(m_mu);
            for (auto& c : m_manifest.components) {
                if (c.id == comp.id) {
                    c.bytes_done = static_cast<int64_t>(done);
                    c.progress = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
                    break;
                }
            }
            return true;
        }, nullptr, {}, comp.size_bytes, comp.sha256);
}

void InstallManager::worker_thread(std::wstring models_dir, ProgressCallback on_progress) {
    {
        std::lock_guard lock(m_mu);
        m_models_dir = std::move(models_dir);
    }

    try {
        std::vector<Component> comps;
        std::wstring worker_models_dir;
        {
            std::lock_guard lock(m_mu);
            comps = m_manifest.components;
            worker_models_dir = m_models_dir;
        }
        for (int index = 0; index < static_cast<int>(comps.size()); ++index) {
            if (m_cancel.load(std::memory_order_acquire)) break;
            const auto& comp = comps[index];
            if (comp.id == "knowledge") {
                std::wstring knowledge_dir;
                std::wstring models_dir_snapshot;
                {
                    std::lock_guard lock(m_mu);
                    knowledge_dir = m_knowledge_dir;
                    models_dir_snapshot = m_models_dir;
                }
                const fs::path source = fs::path(knowledge_dir);
                const fs::path target = fs::path(models_dir_snapshot) / L"knowledge";
                if (fs::is_directory(source)) {
                    fs::create_directories(target);
                    for (const auto& entry : fs::directory_iterator(source)) {
                        if (m_cancel.load(std::memory_order_acquire)) break;
                        fs::copy(entry.path(), target / entry.path().filename(),
                                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                    }
                }
                {
                    std::lock_guard lock(m_mu);
                    auto& c = m_manifest.components[index];
                    const bool copied = !m_cancel.load(std::memory_order_acquire) &&
                                        fs::is_directory(target);
                    c.status = copied ? ComponentStatus::Installed : ComponentStatus::Failed;
                    c.progress = copied ? 1.0f : 0.0f;
                    if (!copied) c.error = "Cancelled";
                }
                if (on_progress) on_progress(index, 0, 0.0);
                continue;
            }
            {
                std::lock_guard lock(m_mu);
                auto& c = m_manifest.components[index];
                c.status = ComponentStatus::Downloading;
                c.progress = 0.0f;
                c.bytes_done = 0;
                c.error.clear();
            }
            const bool ok = download_component(comp, component_path(comp, worker_models_dir));
            {
                std::lock_guard lock(m_mu);
                auto& c = m_manifest.components[index];
                c.status = ok ? ComponentStatus::Installed : ComponentStatus::Failed;
                c.progress = ok ? 1.0f : c.progress;
                if (ok) c.bytes_done = c.size_bytes;
                else c.error = m_cancel.load() ? "Cancelled" : "Download failed";
            }
            if (on_progress) on_progress(index, ok ? comp.size_bytes : 0, 0.0);
        }
    } catch (const std::exception& ex) {
        std::lock_guard lock(m_mu);
        for (auto& c : m_manifest.components) {
            if (c.status == ComponentStatus::Downloading || c.status == ComponentStatus::Verifying) {
                c.status = ComponentStatus::Failed;
                c.error = ex.what();
            }
        }
    } catch (...) {
        std::lock_guard lock(m_mu);
        for (auto& c : m_manifest.components) {
            if (c.status == ComponentStatus::Downloading || c.status == ComponentStatus::Verifying) {
                c.status = ComponentStatus::Failed;
                c.error = "AI install worker failed unexpectedly";
            }
        }
    }
    m_running.store(false, std::memory_order_release);
}

void InstallManager::verify_thread(ProgressCallback on_progress) {
    try {
        std::vector<Component> comps;
        std::wstring models_dir;
        {
            std::lock_guard lock(m_mu);
            if (m_models_dir.empty()) m_models_dir = default_models_dir();
            models_dir = m_models_dir;
            comps = m_manifest.components;
        }
        for (int index = 0; index < static_cast<int>(comps.size()); ++index) {
            if (m_cancel.load(std::memory_order_acquire)) break;
            const auto& comp = comps[index];
            {
                std::lock_guard lock(m_mu);
                m_manifest.components[index].status = ComponentStatus::Verifying;
            }
            const bool installed = comp.id == "knowledge"
                ? fs::is_directory(fs::path(models_dir) / net::to_wide(comp.destination))
                : verify_file(component_path(comp, models_dir), comp);
            {
                std::lock_guard lock(m_mu);
                auto& current = m_manifest.components[index];
                current.status = installed ? ComponentStatus::Installed : ComponentStatus::Missing;
                current.progress = installed ? 1.0f : 0.0f;
                current.bytes_done = installed ? current.size_bytes : 0;
            }
            if (on_progress) on_progress(index, installed ? comp.size_bytes : 0, 0.0);
        }
    } catch (const std::exception& ex) {
        std::lock_guard lock(m_mu);
        for (auto& c : m_manifest.components) {
            if (c.status == ComponentStatus::Verifying) {
                c.status = ComponentStatus::Failed;
                c.error = ex.what();
            }
        }
    } catch (...) {
        std::lock_guard lock(m_mu);
        for (auto& c : m_manifest.components) {
            if (c.status == ComponentStatus::Verifying) {
                c.status = ComponentStatus::Failed;
                c.error = "AI verification failed unexpectedly";
            }
        }
    }
    m_verifying.store(false, std::memory_order_release);
}

void InstallManager::start_verify(ProgressCallback on_progress) {
    std::lock_guard<std::mutex> lock(m_worker_mu);
    if (m_running.load(std::memory_order_acquire) ||
        m_verifying.load(std::memory_order_acquire)) return;
    if (m_worker.joinable()) m_worker.join();
    m_cancel.store(false, std::memory_order_release);
    m_verifying.store(true, std::memory_order_release);
    try {
        m_worker = std::thread([this, on_progress]() { verify_thread(on_progress); });
    } catch (...) {
        m_verifying.store(false, std::memory_order_release);
        throw;
    }
}

void InstallManager::start_install(const std::wstring& models_dir,
                                    ProgressCallback on_progress) {
    std::lock_guard<std::mutex> worker_lock(m_worker_mu);
    if (m_running.load(std::memory_order_acquire) ||
        m_verifying.load(std::memory_order_acquire)) return;
    if (m_worker.joinable()) m_worker.join();
    {
        std::lock_guard lock(m_mu);
        m_models_dir = models_dir;
        for (auto& c : m_manifest.components) {
            c.status = ComponentStatus::Missing;
            c.progress = 0.0f;
            c.bytes_done = 0;
            c.error.clear();
        }
    }
    m_cancel.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    try {
        m_worker = std::thread([this, models_dir, on_progress]() {
            worker_thread(models_dir, on_progress);
        });
    } catch (...) {
        m_running.store(false, std::memory_order_release);
        throw;
    }
}

} // namespace aml::ai_install
