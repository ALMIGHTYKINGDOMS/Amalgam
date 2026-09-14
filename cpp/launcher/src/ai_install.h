#pragma once
// =============================================================================
// ai_install.h — AI model manifest parser + background download manager
// =============================================================================
//
// Reads ai-package-manifest.json, tracks per-component install state, and
// manages concurrent downloads with progress callbacks for the ImGui UI.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aml::ai_install {

// ---- Component status ------------------------------------------------------
enum class ComponentStatus {
    Unknown,      // not yet checked
    Missing,      // file absent or wrong size/hash
    Downloading,  // actively downloading
    Verifying,    // SHA-256 verification in progress
    Installed,    // present and verified
    Failed,       // download or verify failed
    Skipped,      // user chose to skip
};

// ---- Single component descriptor (from manifest) ---------------------------
struct Component {
    std::string id;
    std::string name;
    std::string filename;
    std::string destination;     // relative path under models dir
    std::string download_url;
    std::string sha256;
    int64_t     size_bytes = 0;
    bool        required   = true;

    // Live state (not from manifest)
    ComponentStatus status       = ComponentStatus::Unknown;
    float           progress     = 0.0f;   // 0..1
    int64_t         bytes_done   = 0;
    double          speed_bps    = 0.0;    // bytes/sec
    std::string     error;
};

// ---- Full manifest ---------------------------------------------------------
struct Manifest {
    int         schema_version = 0;
    std::string ai_package;
    std::string runtime_version;
    std::vector<Component> components;
    int64_t     total_bytes = 0;
};

// ---- Progress callback signature -------------------------------------------
// Called periodically during download: (component_index, bytes_done, speed_bps)
using ProgressCallback = std::function<void(int, int64_t, double)>;

// ---- State -----------------------------------------------------------------
// Thread-safe singleton that owns the manifest and download state.
class InstallManager {
public:
    ~InstallManager();

    // Load manifest from JSON file. Returns false on parse error.
    bool load_manifest(const std::wstring& manifest_path);

    // Verify file on disk for a single component (sync, fast).
    void verify_component(int index);

    // Verify all components (sync).
    void verify_all();

    // Start downloading all missing/failed components in a background thread.
    // `on_progress` is called from the worker thread (must be thread-safe).
    void start_install(const std::wstring& models_dir,
                       ProgressCallback on_progress = nullptr);

    // Verify installed components in a background thread. Hashing multi-GB
    // models must never run on the UI/render thread.
    void start_verify(ProgressCallback on_progress = nullptr);

    // Cancel active download or verification (sets a flag checked by workers).
    void cancel();

    // True while a download thread is running.
    bool is_running() const { return m_running.load(std::memory_order_relaxed); }

    // True while the manifest is being verified in the background.
    bool is_verifying() const { return m_verifying.load(std::memory_order_relaxed); }

    // True if all required components are installed.
    bool all_installed() const;

    // Thread-safe snapshots for UI and diagnostics. Callers never retain a
    // reference into mutable worker-owned state.
    Manifest manifest_snapshot() const;
    std::vector<Component> snapshot() const;

    // Models directory for the UI to display paths.
    std::wstring models_dir() const;

    // Singleton.
    static InstallManager& instance();

private:
    InstallManager() = default;

    void worker_thread(std::wstring models_dir, ProgressCallback on_progress);
    void verify_thread(ProgressCallback on_progress);
    void join_worker();
    bool download_component(const Component& comp, const std::wstring& dest);
    bool verify_file(const std::wstring& path, const Component& comp);
    std::wstring component_path(const Component& comp, const std::wstring& models_dir);

    mutable std::mutex m_mu;
    Manifest m_manifest;
    std::wstring m_models_dir;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_verifying{false};
    std::atomic<bool> m_cancel{false};
    std::mutex m_worker_mu;
    std::thread m_worker;
    std::wstring m_manifest_path;
    std::wstring m_knowledge_dir;
    bool m_loaded = false;
};

}  // namespace aml::ai_install
