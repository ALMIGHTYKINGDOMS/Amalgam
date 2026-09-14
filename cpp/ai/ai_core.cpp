#include "ai_core.h"
#include "json.h"

#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <dxgi.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
std::string read_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool sha256_file(const std::wstring& path, std::string& result) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_length = 0;
    DWORD hash_length = 0;
    DWORD returned = 0;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                    nullptr, 0)))
        return false;
    bool ok = BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                               reinterpret_cast<PUCHAR>(&object_length),
                                               sizeof(object_length), &returned, 0)) &&
              BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                               reinterpret_cast<PUCHAR>(&hash_length),
                                               sizeof(hash_length), &returned, 0));
    std::vector<BYTE> object(object_length);
    std::vector<BYTE> digest(hash_length);
    if (ok) {
        ok = BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, object.data(), object_length,
                                              nullptr, 0, 0));
    }
    std::ifstream file(path, std::ios::binary);
    if (ok && !file) ok = false;
    std::vector<char> buffer(1024 * 1024);
    while (ok && file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count > 0) {
            ok = BCRYPT_SUCCESS(BCryptHashData(hash,
                                               reinterpret_cast<PUCHAR>(buffer.data()),
                                               static_cast<ULONG>(count), 0));
        }
    }
    if (ok && file.bad()) ok = false;
    if (ok) ok = BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), hash_length, 0));
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;
    static constexpr char hex[] = "0123456789abcdef";
    result.clear();
    result.reserve(static_cast<size_t>(hash_length) * 2);
    for (BYTE byte : digest) {
        result.push_back(hex[(byte >> 4) & 0x0f]);
        result.push_back(hex[byte & 0x0f]);
    }
    return true;
}

bool valid_sha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

}  // namespace

namespace aml::ai {

// ---------------------------------------------------------------------------
// Hardware detection
// ---------------------------------------------------------------------------

HardwareInfo detect_hardware() {
    HardwareInfo hw;
    hw.cpu_cores = static_cast<int>(std::thread::hardware_concurrency());

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        hw.system_ram_mb = static_cast<int64_t>(ms.ullTotalPhys / (1024 * 1024));
    }

    // GPU via DXGI (device name + VRAM)
    try {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                    if (desc.VendorId == 0x1414 || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter->Release(); adapter = nullptr; continue; }
                    std::string name = std::filesystem::path(desc.Description).string();
                    hw.gpu_name = name;
                    hw.vram_mb = static_cast<int64_t>(desc.DedicatedVideoMemory / (1024 * 1024));
                    adapter->Release();
                    break;
                }
                adapter->Release();
                adapter = nullptr;
            }
            if (factory) factory->Release();
        }
    } catch (...) {
    }
    return hw;
}

MemoryMode best_memory_mode(const HardwareInfo& hw) {
    if (hw.vram_mb >= 12 * 1024) return MemoryMode::GPU;
    if (hw.vram_mb >= 6 * 1024)  return MemoryMode::Balanced;
    if (hw.vram_mb >= 2 * 1024)  return MemoryMode::LowMemory;
    return MemoryMode::CPU;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

bool load_manifest(AIManifest& out, std::string* err) {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::filesystem::path exe = std::filesystem::path(self).parent_path();
    const std::filesystem::path path = exe / L"ai" / L"ai-manifest.json";
    std::string text = read_file(path.wstring());
    if (text.empty()) {
        if (err) *err = "ai-manifest.json missing or empty";
        return false;
    }
    // Parse minimal fields we care about.
    auto find_str = [&](const std::string& key) -> std::string {
        std::string pat = "\"" + key + "\"";
        size_t pos = text.find(pat);
        if (pos == std::string::npos) return {};
        pos = text.find('"', pos + pat.size());
        if (pos == std::string::npos) return {};
        size_t end = text.find('"', pos + 1);
        if (end == std::string::npos) return {};
        return text.substr(pos + 1, end - pos - 1);
    };
    auto find_int = [&](const std::string& key) -> int64_t {
        const std::string pat = "\"" + key + "\"";
        const size_t key_pos = text.find(pat);
        if (key_pos == std::string::npos) return 0;
        const size_t colon = text.find(':', key_pos + pat.size());
        if (colon == std::string::npos) return 0;
        size_t first = colon + 1;
        while (first < text.size() && (text[first] == ' ' || text[first] == '\\t' ||
                                       text[first] == '\\r' || text[first] == '\\n')) ++first;
        size_t end = first;
        if (end < text.size() && text[end] == '-') ++end;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
        if (end == first) return 0;
        try { return std::stoll(text.substr(first, end - first)); }
        catch (...) { return 0; }
    };
    auto find_model = [&](const std::string& prefix, ModelEntry& me) -> bool {
        std::string f = find_str(prefix + "_file");
        std::string h = find_str(prefix + "_sha256");
        std::string repo = find_str(prefix + "_repo");
        if (f.empty() || !valid_sha256(h)) return false;
        me.filename = f;
        me.sha256 = h;
        me.size_bytes = find_int(prefix + "_size_bytes");
        me.hf_repo = repo;
        me.hf_file = f;
        return me.size_bytes > 0;
    };

    out.schema_version = static_cast<int>(find_int("schema_version"));
    if (out.schema_version < 1) out.schema_version = 1;
    out.ai_package = find_str("ai_package");
    out.min_launcher = find_str("min_launcher");
    bool ok = true;
    ok = find_model("brain", out.brain) && ok;
    ok = find_model("vision", out.vision_projector) && ok;
    ok = find_model("art", out.art_model) && ok;
    ok = find_model("art_encoder", out.art_encoder) && ok;
    ok = find_model("decoder", out.art_decoder) && ok;
    if (!ok && err) *err = "manifest missing model entries";
    return ok;
}

bool validate_models(const AIManifest& m, const std::wstring& models_dir, std::string* err) {
    auto check = [&](const ModelEntry& me, const wchar_t* folder) -> bool {
        if (me.filename.empty()) return true;  // optional
        const std::filesystem::path filename(me.filename);
        if (filename.is_absolute() || filename.filename() != filename ||
            me.filename.find("..") != std::string::npos || !valid_sha256(me.sha256)) {
            if (err) *err = "unsafe or invalid manifest entry: " + me.filename;
            return false;
        }
        std::wstring wf;
        for (char ch : me.filename) wf.push_back(static_cast<wchar_t>(ch));
        const std::wstring p = models_dir + L"\\" + folder + L"\\" + wf;
        std::error_code ec;
        const auto size = std::filesystem::file_size(p, ec);
        if (ec) {
            if (err) *err = "missing model: " + me.filename;
            return false;
        }
        if (me.size_bytes <= 0 || size != static_cast<uintmax_t>(me.size_bytes)) {
            if (err) *err = "model size mismatch: " + me.filename;
            return false;
        }
        std::string actual_hash;
        if (!sha256_file(p, actual_hash) || actual_hash != me.sha256) {
            if (err) *err = "model SHA-256 mismatch: " + me.filename;
            return false;
        }
        return true;
    };
    bool ok = check(m.brain, L"brain") && check(m.vision_projector, L"brain") &&
              check(m.art_model, L"art") && check(m.art_encoder, L"art") &&
              check(m.art_decoder, L"art");
    return ok;
}

// ---------------------------------------------------------------------------
// Conversation store
// ---------------------------------------------------------------------------

namespace {
// Conversation storage is deliberately bounded and serialized. A malformed
// or interrupted write must never crash the launcher or erase the last good
// conversation.
constexpr size_t kMaxConversationBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxTurnTextBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxTurns = 200;
std::mutex g_conversation_mutex;
std::wstring conversation_file(const std::wstring& profile_dir) {
    return (std::filesystem::path(profile_dir) / L"ai-conversation.json").wstring();
}
}

std::vector<Turn> load_conversation_unlocked(const std::wstring& profile_dir) {
    std::vector<Turn> turns;
    if (profile_dir.empty()) return turns;

    const auto path = conversation_file(profile_dir);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxConversationBytes) return turns;

    Json root;
    std::string parse_error;
    if (!json_parse_file(path, root, &parse_error) || !root.is(Json::Type::Arr)) return turns;
    for (const Json& item : root.items()) {
        if (!item.is(Json::Type::Obj)) continue;
        Turn turn;
        turn.role = item.get("role").as_str();
        turn.text = item.get("text").as_str();
        if ((turn.role != "user" && turn.role != "assistant" && turn.role != "system") ||
            item.find("role") == nullptr || item.find("text") == nullptr ||
            turn.text.size() > kMaxTurnTextBytes) {
            continue;
        }
        turn.timestamp = item.get("ts").as_int(0);
        turn.plan_id = item.get("plan_id").as_str();
        turn.checkpointed = item.get("checkpointed").as_bool(false);
        turns.push_back(std::move(turn));
        if (turns.size() > kMaxTurns) turns.erase(turns.begin());
    }
    return turns;
}

void save_conversation_unlocked(const std::wstring& profile_dir, const std::vector<Turn>& turns) {
    if (profile_dir.empty()) return;

    Json root = Json::arr();
    const size_t first = turns.size() > kMaxTurns ? turns.size() - kMaxTurns : 0;
    for (size_t i = first; i < turns.size(); ++i) {
        Json item = Json::obj();
        item.set("role", Json::str(turns[i].role));
        item.set("text", Json::str(turns[i].text));
        item.set("ts", Json::num(static_cast<double>(turns[i].timestamp)));
        item.set("plan_id", Json::str(turns[i].plan_id));
        item.set("checkpointed", Json::boolean(turns[i].checkpointed));
        root.push(std::move(item));
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(profile_dir), ec);
    if (ec) return;
    std::string error;
    json_write_file(conversation_file(profile_dir), root, &error);
}

std::vector<Turn> load_conversation(const std::wstring& profile_dir) {
    std::lock_guard<std::mutex> lock(g_conversation_mutex);
    return load_conversation_unlocked(profile_dir);
}

void save_conversation(const std::wstring& profile_dir, const std::vector<Turn>& turns) {
    std::lock_guard<std::mutex> lock(g_conversation_mutex);
    save_conversation_unlocked(profile_dir, turns);
}

void append_turn(const std::wstring& profile_dir, const Turn& turn) {
    std::lock_guard<std::mutex> lock(g_conversation_mutex);
    if (profile_dir.empty()) return;
    auto turns = load_conversation_unlocked(profile_dir);
    turns.push_back(turn);
    save_conversation_unlocked(profile_dir, turns);
}

}  // namespace aml::ai