#include "storage_manager.h"
#include "supabase.h"
#include "net.h"
#include "json.h"

#include <windows.h>
#include <shellapi.h>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <random>

namespace aml::storage {

namespace {

struct AvatarObjectMetadata {
    std::string path;
    std::string content_type;
    int64_t updated_at = 0;
    int64_t created_at = 0;
};

std::string avatar_prefix(const std::string& user_id) {
    return "avatars/" + user_id;
}

std::string avatar_object_path(const aml::supabase::SupabaseClient::StorageItem& item) {
    std::string path = item.path.empty() ? item.name : item.path;
    if (path.empty()) return "";
    if (path.rfind("avatars/", 0) != 0) path = "avatars/" + path;
    return path;
}

bool is_avatar_object_for_user(const std::string& path, const std::string& user_id) {
    const std::string prefix = avatar_prefix(user_id);
    if (path.rfind(prefix, 0) != 0 || path.size() == prefix.size()) return false;
    return path.find('/', prefix.size()) == std::string::npos;
}

bool list_avatar_objects(aml::supabase::SupabaseClient* client,
                         const std::string& user_id,
                         std::vector<AvatarObjectMetadata>* objects,
                         std::string* error = nullptr) {
    if (objects) objects->clear();
    if (!client) {
        if (error) *error = "Avatar storage is unavailable: no storage client is configured";
        return false;
    }

    aml::supabase::SupabaseClient::StorageListOptions options;
    options.bucket = "avatars";
    options.prefix = avatar_prefix(user_id);
    options.limit = 100;
    const auto result = client->list_files(options);
    if (!result.success) {
        if (error) *error = result.error.empty()
            ? "Avatar storage metadata lookup failed" : result.error;
        return false;
    }

    for (const auto& item : result.items) {
        const std::string path = avatar_object_path(item);
        if (!is_avatar_object_for_user(path, user_id)) continue;

        bool duplicate = false;
        if (objects) {
            for (const auto& existing : *objects) {
                if (existing.path == path) {
                    duplicate = true;
                    break;
                }
            }
        }
        if (duplicate || !objects) continue;

        AvatarObjectMetadata object;
        object.path = path;
        object.content_type = item.mime_type;
        object.updated_at = item.updated_at;
        object.created_at = item.created_at;
        objects->push_back(std::move(object));
    }
    return true;
}

const AvatarObjectMetadata* newest_avatar_object(
    const std::vector<AvatarObjectMetadata>& objects) {
    const AvatarObjectMetadata* newest = nullptr;
    for (const auto& object : objects) {
        if (!newest || object.updated_at > newest->updated_at ||
            (object.updated_at == newest->updated_at &&
             object.created_at > newest->created_at) ||
            (object.updated_at == newest->updated_at &&
             object.created_at == newest->created_at && object.path > newest->path)) {
            newest = &object;
        }
    }
    return newest;
}

std::string url_encode_path(const std::string& path) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(path.size());
    for (unsigned char c : path) {
        const bool unreserved = (c >= 'A' && c <= 'Z') ||
                                (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved || c == '/') {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[c >> 4]);
            encoded.push_back(hex[c & 0x0F]);
        }
    }
    return encoded;
}

std::string uploaded_object_path(const std::string& bucket, const std::string& requested_path,
                                 const aml::supabase::SupabaseClient::StorageUploadResult& result) {
    const std::string prefix = bucket + "/";
    if (!result.path.empty()) {
        return result.path;
    }
    if (!result.full_path.empty()) {
        return result.full_path.rfind(prefix, 0) == 0
            ? result.full_path.substr(prefix.size()) : result.full_path;
    }
    return requested_path;
}

}  // namespace

// ---------------------------------------------------------------------------
// Storage Manager Implementation
// ---------------------------------------------------------------------------

StorageManager::StorageManager() {
    initialize_buckets();
}

StorageManager::~StorageManager() {
    cleanup_temp_files();
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

StorageManager& StorageManager::instance() {
    static StorageManager manager;
    return manager;
}

// ---------------------------------------------------------------------------
// Bucket Management
// ---------------------------------------------------------------------------

bool StorageManager::initialize_buckets() {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_initialized()) {
        return false;
    }
    
    // Bucket creation requires a service-role deployment operation and must
    // never be attempted from a player executable. Missing buckets are
    // reported by get_buckets() instead of making launcher startup fail.
    return true;
}

bool StorageManager::bucket_exists(const std::string& bucket_name) const {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_initialized()) {
        return false;
    }
    
    auto* client = supabase.client();
    if (!client) return false;
    aml::supabase::SupabaseClient::StorageListOptions options;
    options.bucket = bucket_name;
    options.limit = 1;
    return client->list_files(options).success;
}

bool StorageManager::create_bucket(const std::string& bucket_name) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_initialized()) {
        return false;
    }
    
    // Bucket provisioning requires the service role and belongs in deployment
    // migrations, not in a player-facing executable.
    (void)bucket_name;
    return false;
}

bool StorageManager::delete_bucket(const std::string& bucket_name) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_initialized()) {
        return false;
    }
    
    (void)bucket_name;
    return false;
}

std::vector<StorageBucket> StorageManager::get_buckets() const {
    std::vector<StorageBucket> buckets;
    
    const std::pair<const char*, const char*> definitions[] = {
        {"modpacks", "Modpack files"}, {"backups", "Server backups"},
        {"avatars", "User avatars"}, {"screenshots", "Game screenshots"},
        {"logs", "Game logs"}, {"profiles", "Profile data"}
    };
    for (const auto& definition : definitions) {
        buckets.push_back({definition.first, definition.second,
                           bucket_exists(definition.first), 0});
    }
    
    return buckets;
}

// ---------------------------------------------------------------------------
// File Upload
// ---------------------------------------------------------------------------

StorageUploadResult StorageManager::upload_file(
    const std::string& bucket_name, 
    const std::string& file_path, 
    const std::vector<uint8_t>& data, 
    const std::string& content_type) {
    
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_initialized() || !supabase.is_authenticated()) {
        return {false, "Not authenticated", "", 0};
    }
    auto* client = supabase.client();
    if (!client) {
        return {false, "Storage backend is configured without a storage client", "", 0};
    }
    
    // Generate a unique filename if not provided
    std::string actual_path = file_path;
    if (actual_path.empty()) {
        actual_path = generate_file_id() + get_extension_from_content_type(content_type);
    }
    
    // Upload to Supabase Storage
    auto result = supabase.upload_to_storage(bucket_name, actual_path, data, content_type);
    
    if (!result.success) {
        return {false, result.error.empty() ? "Remote storage upload failed" : result.error, "", 0};
    }

    const std::string uploaded_path = uploaded_object_path(bucket_name, actual_path, result);
    return {true, "", uploaded_path, static_cast<uint64_t>(data.size())};
}

StorageUploadResult StorageManager::upload_file_from_path(
    const std::string& bucket_name, 
    const std::wstring& local_path, 
    const std::string& destination_path) {
    
    // Read file data
    std::ifstream file(local_path, std::ios::binary);
    if (!file) {
        return {false, "Could not open file", "", 0};
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Read file data
    std::vector<uint8_t> data(file_size);
    if (!file.read(reinterpret_cast<char*>(data.data()), file_size)) {
        return {false, "Could not read file", "", 0};
    }
    
    // Determine content type from extension
    std::string content_type = get_content_type_from_path(local_path);
    
    // Upload
    return upload_file(bucket_name, destination_path, data, content_type);
}

// ---------------------------------------------------------------------------
// File Download
// ---------------------------------------------------------------------------

StorageDownloadResult StorageManager::download_file(
    const std::string& bucket_name, 
    const std::string& file_path) {
    
    auto& supabase = aml::supabase::SupabaseManager::instance();
    
    if (!supabase.is_initialized() || !supabase.is_authenticated()) {
        return {false, "Not authenticated", {}};
    }
    
    // Download from Supabase Storage
    auto result = supabase.download_from_storage(bucket_name, file_path);
    
    if (!result.success) {
        return {false, result.error, {}};
    }
    
    return {true, "", result.data};
}

bool StorageManager::download_file_to_path(
    const std::string& bucket_name, 
    const std::string& file_path, 
    const std::wstring& local_path) {
    
    auto result = download_file(bucket_name, file_path);
    if (!result.success) {
        return false;
    }
    
    // Create directory if it doesn't exist
    std::wstring dir = std::filesystem::path(local_path).parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
    }
    
    // Write file
    std::ofstream file(local_path, std::ios::binary);
    if (!file) {
        return false;
    }
    
    file.write(reinterpret_cast<const char*>(result.data.data()), result.data.size());
    return file.good();
}

// ---------------------------------------------------------------------------
// File Management
// ---------------------------------------------------------------------------

bool StorageManager::delete_file(const std::string& bucket_name, const std::string& file_path) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_initialized() || !supabase.is_authenticated()) {
        return false;
    }
    return supabase.client()->delete_file(bucket_name, file_path);
}

bool StorageManager::file_exists(const std::string& bucket_name, const std::string& file_path) const {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_initialized() || !supabase.is_authenticated()) {
        return false;
    }
    auto result = supabase.client()->list_files({
        bucket_name, file_path, 1, 0, "", true
    });
    return result.success && !result.items.empty();
}

StorageFileInfo StorageManager::get_file_info(const std::string& bucket_name, const std::string& file_path) const {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_initialized() || !supabase.is_authenticated()) {
        return StorageFileInfo();
    }
    auto result = supabase.client()->list_files({
        bucket_name, file_path, 1, 0, "", true
    });
    StorageFileInfo info;
    if (result.success && !result.items.empty()) {
        const auto& item = result.items[0];
        info.name = item.name;
        info.path = item.path;
        info.size_bytes = item.size;
        info.content_type = item.mime_type;
        info.last_modified = item.updated_at;
    }
    return info;
}

std::vector<StorageFileInfo> StorageManager::list_files(
    const std::string& bucket_name, 
    const std::string& prefix) const {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_initialized() || !supabase.is_authenticated()) {
        return {};
    }
    auto result = supabase.client()->list_files({
        bucket_name, prefix, 100, 0, "", true
    });
    std::vector<StorageFileInfo> files;
    for (const auto& item : result.items) {
        StorageFileInfo info;
        info.name = item.name;
        info.path = item.path;
        info.size_bytes = item.size;
        info.content_type = item.mime_type;
        info.last_modified = item.updated_at;
        files.push_back(info);
    }
    return files;
}

// ---------------------------------------------------------------------------
// Modpack Storage
// ---------------------------------------------------------------------------

StorageUploadResult StorageManager::upload_modpack(
    const std::string& modpack_id, 
    const std::vector<uint8_t>& data, 
    const ModpackMetadata& metadata) {
    
    // Upload to modpacks bucket
    std::string file_path = "modpacks/" + modpack_id + ".zip";
    auto result = upload_file("modpacks", file_path, data, "application/zip");
    
    if (!result.success) {
        return result;
    }
    
    // Save metadata
    if (!save_modpack_metadata(modpack_id, metadata)) {
        const std::string uploaded_path = result.file_path.empty() ? file_path : result.file_path;
        const bool removed = delete_file("modpacks", uploaded_path);
        result.success = false;
        result.error = removed
            ? "Modpack metadata could not be persisted; upload was rolled back"
            : "Modpack metadata could not be persisted and the uploaded object could not be rolled back";
        result.file_path.clear();
        result.size_bytes = 0;
    }
    
    return result;
}

bool StorageManager::download_modpack(
    const std::string& modpack_id, 
    std::vector<uint8_t>* data, 
    ModpackMetadata* metadata) {
    
    // Download from modpacks bucket
    std::string file_path = "modpacks/" + modpack_id + ".zip";
    auto result = download_file("modpacks", file_path);
    
    if (!result.success) {
        return false;
    }
    
    *data = result.data;
    
    // Load metadata
    if (metadata) {
        *metadata = load_modpack_metadata(modpack_id);
    }
    
    return true;
}

bool StorageManager::delete_modpack(const std::string& modpack_id) {
    // Delete from modpacks bucket
    std::string file_path = "modpacks/" + modpack_id + ".zip";
    bool success = delete_file("modpacks", file_path);
    
    if (success) {
        // Delete metadata
        return delete_modpack_metadata(modpack_id);
    }
    
    return false;
}

// ---------------------------------------------------------------------------
// Backup Storage
// ---------------------------------------------------------------------------

StorageUploadResult StorageManager::upload_backup(
    const std::string& backup_id, 
    const std::vector<uint8_t>& data, 
    const BackupMetadata& metadata) {
    
    // Upload to backups bucket
    std::string file_path = "backups/" + backup_id + ".tar.gz";
    auto result = upload_file("backups", file_path, data, "application/gzip");
    
    if (!result.success) {
        return result;
    }
    
    // Save metadata
    save_backup_metadata(backup_id, metadata);
    
    return result;
}

bool StorageManager::download_backup(
    const std::string& backup_id, 
    std::vector<uint8_t>* data, 
    BackupMetadata* metadata) {
    
    // Download from backups bucket
    std::string file_path = "backups/" + backup_id + ".tar.gz";
    auto result = download_file("backups", file_path);
    
    if (!result.success) {
        return false;
    }
    
    *data = result.data;
    
    // Load metadata
    if (metadata) {
        *metadata = load_backup_metadata(backup_id);
    }
    
    return true;
}

bool StorageManager::delete_backup(const std::string& backup_id) {
    // Delete from backups bucket
    std::string file_path = "backups/" + backup_id + ".tar.gz";
    bool success = delete_file("backups", file_path);
    
    if (success) {
        // Delete metadata
        delete_backup_metadata(backup_id);
    }
    
    return success;
}

// ---------------------------------------------------------------------------
// Avatar Storage
// ---------------------------------------------------------------------------

StorageUploadResult StorageManager::upload_avatar(
    const std::string& user_id, 
    const std::vector<uint8_t>& data, 
    const std::string& content_type) {
    
    // Generate avatar filename
    std::string file_path = "avatars/" + user_id + get_extension_from_content_type(content_type);
    return upload_file("avatars", file_path, data, content_type);
}

bool StorageManager::download_avatar(
    const std::string& user_id, 
    std::vector<uint8_t>* data, 
    std::string* content_type) {
    if (!data || user_id.empty()) return false;

    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto* client = supabase.client();
    if (!supabase.is_initialized() || !supabase.is_authenticated() || !client) {
        return false;
    }

    std::vector<AvatarObjectMetadata> objects;
    if (!list_avatar_objects(client, user_id, &objects) || objects.empty()) {
        return false;
    }

    const AvatarObjectMetadata* object = newest_avatar_object(objects);
    if (!object) return false;
    const auto result = client->download_file("avatars", object->path);
    if (!result.success) return false;

    *data = result.data;
    if (content_type) {
        if (!object->content_type.empty()) {
            *content_type = object->content_type;
        } else {
            const size_t separator = object->path.find_last_of('.');
            *content_type = separator == std::string::npos
                ? "application/octet-stream"
                : get_content_type_from_extension(object->path.substr(separator));
        }
    }
    return true;
}

bool StorageManager::delete_avatar(const std::string& user_id) {
    if (user_id.empty()) return false;

    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto* client = supabase.client();
    if (!supabase.is_initialized() || !supabase.is_authenticated() || !client) {
        return false;
    }

    std::vector<AvatarObjectMetadata> objects;
    if (!list_avatar_objects(client, user_id, &objects) || objects.empty()) {
        return false;
    }

    bool all_deleted = true;
    for (const auto& object : objects) {
        if (!client->delete_file("avatars", object.path)) {
            all_deleted = false;
        }
    }
    return all_deleted;
}

std::string StorageManager::get_avatar_url(const std::string& user_id) const {
    auto& supabase = aml::supabase::SupabaseManager::instance();

    auto* client = supabase.client();
    if (user_id.empty() || !supabase.is_initialized() ||
        !supabase.is_authenticated() || !client || supabase.get_project_url().empty()) {
        return "";
    }

    std::vector<AvatarObjectMetadata> objects;
    if (!list_avatar_objects(client, user_id, &objects) || objects.empty()) {
        return "";
    }

    const AvatarObjectMetadata* object = newest_avatar_object(objects);
    if (!object || object->path.empty()) return "";

    return supabase.get_project_url() + "/storage/v1/object/public/avatars/" +
           url_encode_path(object->path);
}

// ---------------------------------------------------------------------------
// Metadata Management
// ---------------------------------------------------------------------------

bool StorageManager::save_modpack_metadata(const std::string& modpack_id, const ModpackMetadata& metadata) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto* client = supabase.client();
    if (!client || !client->is_authenticated()) return false;
    Json row = Json::obj();
    row.set("id", Json::str(modpack_id));
    row.set("name", Json::str(metadata.name));
    row.set("version", Json::str(metadata.version));
    row.set("minecraft_version", Json::str(metadata.minecraft_version));
    row.set("loader", Json::str(metadata.loader));
    row.set("loader_version", Json::str(metadata.loader_version));
    row.set("author", Json::str(metadata.author));
    row.set("description", Json::str(metadata.description));
    row.set("size_bytes", Json::num(static_cast<double>(metadata.size_bytes)));
    row.set("sha1", Json::str(metadata.sha1));
    row.set("downloads", Json::num(metadata.downloads));
    row.set("rating", Json::num(metadata.rating));
    aml::supabase::SupabaseClient::DBInsertOptions options;
    options.table = "modpack_metadata";
    options.records.push_back(row);
    options.upsert = true;
    options.on_conflict = "id";
    if (!client->insert(options).success) return false;
    std::lock_guard<std::mutex> lock(metadata_mu_);
    modpack_metadata_[modpack_id] = metadata;
    return true;
}

ModpackMetadata StorageManager::load_modpack_metadata(const std::string& modpack_id) const {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto* client = supabase.client();
    if (client && client->is_authenticated()) {
        aml::supabase::SupabaseClient::DBQueryOptions options;
        options.table = "modpack_metadata";
        options.eq_filters["id"] = modpack_id;
        options.limit = 1;
        const auto result = client->select(options);
        if (result.success && !result.data.empty()) {
            const auto& row = result.data.front();
            ModpackMetadata metadata;
            metadata.id = row.get("id").as_str();
            metadata.name = row.get("name").as_str();
            metadata.version = row.get("version").as_str();
            metadata.minecraft_version = row.get("minecraft_version").as_str();
            metadata.loader = row.get("loader").as_str();
            metadata.loader_version = row.get("loader_version").as_str();
            metadata.author = row.get("author").as_str();
            metadata.description = row.get("description").as_str();
            metadata.size_bytes = row.get("size_bytes").asUInt64();
            metadata.sha1 = row.get("sha1").as_str();
            metadata.downloads = row.get("downloads").asInt();
            metadata.rating = row.get("rating").as_num();
            return metadata;
        }
    }
    std::lock_guard<std::mutex> lock(metadata_mu_);
    auto it = modpack_metadata_.find(modpack_id);
    if (it != modpack_metadata_.end()) {
        return it->second;
    }
    return ModpackMetadata();
}

bool StorageManager::delete_modpack_metadata(const std::string& modpack_id) {
    auto& supabase = aml::supabase::SupabaseManager::instance();
    auto* client = supabase.client();
    if (client && client->is_authenticated()) {
        aml::supabase::SupabaseClient::DBDeleteOptions options;
        options.table = "modpack_metadata";
        options.eq_filters["id"] = modpack_id;
        if (!client->remove(options).success) return false;
    }
    std::lock_guard<std::mutex> lock(metadata_mu_);
    modpack_metadata_.erase(modpack_id);
    return true;
}

bool StorageManager::save_backup_metadata(const std::string& backup_id, const BackupMetadata& metadata) {
    std::lock_guard<std::mutex> lock(metadata_mu_);
    backup_metadata_[backup_id] = metadata;
    return true;
}

BackupMetadata StorageManager::load_backup_metadata(const std::string& backup_id) const {
    std::lock_guard<std::mutex> lock(metadata_mu_);
    auto it = backup_metadata_.find(backup_id);
    if (it != backup_metadata_.end()) {
        return it->second;
    }
    return BackupMetadata();
}

bool StorageManager::delete_backup_metadata(const std::string& backup_id) {
    std::lock_guard<std::mutex> lock(metadata_mu_);
    backup_metadata_.erase(backup_id);
    return true;
}

// ---------------------------------------------------------------------------
// Utility Functions
// ---------------------------------------------------------------------------

std::string StorageManager::generate_file_id() const {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    const char* hex_chars = "0123456789abcdef";
    std::string id;
    
    for (int i = 0; i < 32; ++i) {
        id += hex_chars[dis(gen)];
    }
    
    return id;
}

std::string StorageManager::get_extension_from_content_type(const std::string& content_type) const {
    if (content_type.find("zip") != std::string::npos) return ".zip";
    if (content_type.find("gzip") != std::string::npos) return ".gz";
    if (content_type.find("tar") != std::string::npos) return ".tar";
    if (content_type.find("png") != std::string::npos) return ".png";
    if (content_type.find("jpeg") != std::string::npos) return ".jpg";
    if (content_type.find("jpg") != std::string::npos) return ".jpg";
    if (content_type.find("gif") != std::string::npos) return ".gif";
    if (content_type.find("webp") != std::string::npos) return ".webp";
    if (content_type.find("json") != std::string::npos) return ".json";
    if (content_type.find("text") != std::string::npos) return ".txt";
    if (content_type.find("log") != std::string::npos) return ".log";
    return "";
}

std::string StorageManager::get_content_type_from_extension(const std::string& extension) const {
    if (extension == ".zip") return "application/zip";
    if (extension == ".gz" || extension == ".gzip") return "application/gzip";
    if (extension == ".tar") return "application/x-tar";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".json") return "application/json";
    if (extension == ".txt") return "text/plain";
    if (extension == ".log") return "text/plain";
    return "application/octet-stream";
}

std::string StorageManager::get_content_type_from_path(const std::wstring& path) const {
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    return get_content_type_from_extension(net::to_utf8(ext));
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void StorageManager::cleanup_temp_files() {
    std::wstring temp_dir = std::filesystem::temp_directory_path().wstring() +
        L"\\amalgam_storage";
    if (std::filesystem::exists(temp_dir)) {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }
    std::lock_guard<std::mutex> lock(metadata_mu_);
    modpack_metadata_.clear();
    backup_metadata_.clear();
}

// ---------------------------------------------------------------------------
// Storage Statistics
// ---------------------------------------------------------------------------

StorageStats StorageManager::get_stats() const {
    StorageStats stats;
    auto& supabase = aml::supabase::SupabaseManager::instance();
    if (!supabase.is_initialized() || !supabase.is_authenticated()) {
        return stats;
    }
    std::vector<std::string> buckets = {"modpacks", "backups", "avatars", "screenshots", "logs", "profiles"};
    stats.buckets = static_cast<int>(buckets.size());
    uint64_t total_size = 0;
    int total_files = 0;
    for (const auto& bucket : buckets) {
        auto result = supabase.client()->list_files({bucket, "", 1000, 0, "", true});
        if (result.success) {
            total_files += static_cast<int>(result.items.size());
            for (const auto& item : result.items) {
                total_size += item.size;
            }
        }
    }
    stats.total_files = total_files;
    stats.total_size_bytes = total_size;
    return stats;
}

}  // namespace aml::storage
