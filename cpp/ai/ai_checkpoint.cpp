#include "ai_checkpoint.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>

namespace aml::ai {

namespace {
std::string timestamp_str() {
    return std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string rand_hex(int len) {
    static thread_local std::mt19937 rng(std::random_device{}());
    static const char* hex = "0123456789abcdef";
    std::string s(len, '0');
    for (int i = 0; i < len; ++i) s[i] = hex[rng() & 15];
    return s;
}

bool copy_dir(const std::filesystem::path& src, const std::filesystem::path& dst,
              const std::vector<std::string>& /*filter_categories*/,
              std::string* /*err*/) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    // Simple copy of allowed files only
    for (auto it = std::filesystem::recursive_directory_iterator(
             src, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& p = it->path();
        if (it->is_directory(ec)) {
            // Skip huge directories
            std::string dirname = p.filename().string();
            std::transform(dirname.begin(), dirname.end(), dirname.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (dirname == "saves" || dirname == "worlds" || dirname == "screenshots" ||
                dirname == "libraries" || dirname == "versions" || dirname == "cache" ||
                dirname == "logs" || dirname == "crash-reports") {
                it.disable_recursion_pending();
                continue;
            }
            continue;
        }
        // Small files only
        auto sz = it->file_size(ec);
        if (sz > 50 * 1024 * 1024) continue;  // skip > 50 MB
        auto rel = std::filesystem::relative(p, src, ec);
        if (ec) continue;
        auto target = dst / rel;
        std::filesystem::create_directories(target.parent_path(), ec);
        std::filesystem::copy_file(p, target, std::filesystem::copy_options::overwrite_existing, ec);
    }
    return true;
}
}  // namespace

std::wstring checkpoint_dir(const std::wstring& profile_root) {
    return profile_root + L"\\ai-checkpoints";
}

bool create_checkpoint(const std::wstring& profile_root, const std::string& label,
                       Checkpoint& out, std::string* err) {
    auto ck_dir = checkpoint_dir(profile_root);
    std::error_code ec;
    std::filesystem::create_directories(ck_dir, ec);

    out.id = timestamp_str() + "-" + rand_hex(6);
    out.label = label;
    out.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    out.dir = ck_dir + L"\\" + std::filesystem::path(out.id).wstring();
    out.profile_id = std::filesystem::path(profile_root).filename().string();

    // Copy profile files (not worlds, not large assets) to checkpoint dir
    std::filesystem::path src(profile_root);
    std::filesystem::path dst(out.dir);
    if (!copy_dir(src, dst, {}, err)) return false;

    // Write metadata
    std::ofstream meta(dst / "checkpoint.json");
    meta << "{\"id\":\"" << out.id << "\",\"label\":\"" << out.label
         << "\",\"created\":" << out.created_at << "}\n";
    return true;
}

std::vector<Checkpoint> list_checkpoints(const std::wstring& profile_root) {
    std::vector<Checkpoint> out;
    auto ck_dir = checkpoint_dir(profile_root);
    std::error_code ec;
    for (auto& dir_entry : std::filesystem::directory_iterator(ck_dir, ec)) {
        if (ec) break;
        if (dir_entry.is_directory(ec)) {
            auto meta_file = dir_entry.path() / "checkpoint.json";
            if (!std::filesystem::exists(meta_file, ec)) continue;
            std::ifstream f(meta_file);
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string json = ss.str();

            Checkpoint cp;
            cp.dir = dir_entry.path().wstring();
            // extract id
            auto vid = json.find("\"id\"");
            if (vid != std::string::npos) {
                auto s = json.find('"', json.find(':', vid) + 1);
                auto e = json.find('"', s + 1);
                cp.id = json.substr(s + 1, e - s - 1);
            }
            auto vlabel = json.find("\"label\"");
            if (vlabel != std::string::npos) {
                auto s = json.find('"', json.find(':', vlabel) + 1);
                auto e = json.find('"', s + 1);
                cp.label = json.substr(s + 1, e - s - 1);
            }
            auto vts = json.find("\"created\"");
            if (vts != std::string::npos) {
                cp.created_at = std::stoll(json.substr(json.find(':', vts) + 1));
            }
            out.push_back(std::move(cp));
        }
    }
    std::sort(out.begin(), out.end(), [](const Checkpoint& a, const Checkpoint& b) {
        return a.created_at > b.created_at;
    });
    return out;
}

bool restore_checkpoint(const std::wstring& profile_root, const Checkpoint& cp,
                        std::string* err) {
    std::error_code ec;
    if (!std::filesystem::exists(cp.dir, ec)) {
        if (err) *err = "checkpoint directory not found";
        return false;
    }
    // Restore: copy checkpoint files back, but don't delete anything that
    // didn't exist in the checkpoint (safe partial undo).
    for (auto it = std::filesystem::recursive_directory_iterator(
             cp.dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_directory(ec)) continue;
        auto rel = std::filesystem::relative(it->path(), cp.dir, ec);
        if (ec) continue;
        auto target = std::filesystem::path(profile_root) / rel;
        std::filesystem::create_directories(target.parent_path(), ec);
        std::filesystem::copy_file(it->path(), target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    return true;
}

bool prune_checkpoints(const std::wstring& profile_root, int keep, std::string* /*err*/) {
    auto cps = list_checkpoints(profile_root);
    for (size_t i = static_cast<size_t>(keep); i < cps.size(); ++i) {
        std::error_code ec;
        std::filesystem::remove_all(cps[i].dir, ec);
    }
    return true;
}

}  // namespace aml::ai