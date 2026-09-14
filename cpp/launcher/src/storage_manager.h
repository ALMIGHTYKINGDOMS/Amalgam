#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <ctime>
#include <cstdint>

namespace aml::storage {

// ---------------------------------------------------------------------------
// Storage Types
// ---------------------------------------------------------------------------

enum class StorageType {
    Modpack,
    Backup,
    Avatar,
    Screenshot,
    Log,
    Profile,
    Other
};

inline const char* storage_type_name(StorageType type) {
    switch (type) {
        case StorageType::Modpack: return "Modpack";
        case StorageType::Backup: return "Backup";
        case StorageType::Avatar: return "Avatar";
        case StorageType::Screenshot: return "Screenshot";
        case StorageType::Log: return "Log";
        case StorageType::Profile: return "Profile";
        default: return "Other";
    }
}

// ---------------------------------------------------------------------------
// Storage Bucket
// ---------------------------------------------------------------------------

struct StorageBucket {
    std::string name;
    std::string description;
    bool public_access = false;
    int64_t created_at = 0;
};

// ---------------------------------------------------------------------------
// Storage File Info
// ---------------------------------------------------------------------------

struct StorageFileInfo {
    std::string name;
    std::string path;
    uint64_t size_bytes = 0;
    std::string content_type;
    int64_t last_modified = 0;
    std::string etag;
    std::map<std::string, std::string> metadata;
};

// ---------------------------------------------------------------------------
// Upload Result
// ---------------------------------------------------------------------------

struct StorageUploadResult {
    bool success = false;
    std::string error;
    std::string file_path;
    uint64_t size_bytes = 0;
};

// ---------------------------------------------------------------------------
// Download Result
// ---------------------------------------------------------------------------

struct StorageDownloadResult {
    bool success = false;
    std::string error;
    std::vector<uint8_t> data;
};

// ---------------------------------------------------------------------------
// Modpack Metadata
// ---------------------------------------------------------------------------

struct ModpackMetadata {
    std::string id;
    std::string name;
    std::string version;
    std::string minecraft_version;
    std::string loader;
    std::string loader_version;
    std::string author;
    std::string description;
    std::vector<std::string> tags;
    std::vector<std::string> mods;
    std::vector<std::string> resource_packs;
    std::vector<std::string> data_packs;
    uint64_t size_bytes = 0;
    std::string sha1;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int downloads = 0;
    double rating = 0.0;
};

// ---------------------------------------------------------------------------
// Backup Metadata
// ---------------------------------------------------------------------------

struct BackupMetadata {
    std::string id;
    std::string server_id;
    std::string label;
    std::string type; // full, world, config
    int64_t timestamp = 0;
    uint64_t size_bytes = 0;
    std::string sha1;
    std::string created_by;
};

// ---------------------------------------------------------------------------
// Storage Statistics
// ---------------------------------------------------------------------------

struct StorageStats {
    int64_t total_files = 0;
    uint64_t total_size_bytes = 0;
    int buckets = 0;
    std::map<std::string, uint64_t> size_by_bucket;
};

// ---------------------------------------------------------------------------
// Storage Manager
// ---------------------------------------------------------------------------

class StorageManager {
public:
    static StorageManager& instance();
    
    // Bucket Management
    bool initialize_buckets();
    bool bucket_exists(const std::string& bucket_name) const;
    bool create_bucket(const std::string& bucket_name);
    bool delete_bucket(const std::string& bucket_name);
    std::vector<StorageBucket> get_buckets() const;
    
    // File Operations
    StorageUploadResult upload_file(
        const std::string& bucket_name, 
        const std::string& file_path, 
        const std::vector<uint8_t>& data, 
        const std::string& content_type);
    
    StorageUploadResult upload_file_from_path(
        const std::string& bucket_name, 
        const std::wstring& local_path, 
        const std::string& destination_path);
    
    StorageDownloadResult download_file(
        const std::string& bucket_name, 
        const std::string& file_path);
    
    bool download_file_to_path(
        const std::string& bucket_name, 
        const std::string& file_path, 
        const std::wstring& local_path);
    
    bool delete_file(const std::string& bucket_name, const std::string& file_path);
    bool file_exists(const std::string& bucket_name, const std::string& file_path) const;
    StorageFileInfo get_file_info(const std::string& bucket_name, const std::string& file_path) const;
    std::vector<StorageFileInfo> list_files(
        const std::string& bucket_name, 
        const std::string& prefix = "") const;
    
    // Modpack Storage
    StorageUploadResult upload_modpack(
        const std::string& modpack_id, 
        const std::vector<uint8_t>& data, 
        const ModpackMetadata& metadata);
    
    bool download_modpack(
        const std::string& modpack_id, 
        std::vector<uint8_t>* data, 
        ModpackMetadata* metadata = nullptr);
    
    bool delete_modpack(const std::string& modpack_id);
    
    // Backup Storage
    StorageUploadResult upload_backup(
        const std::string& backup_id, 
        const std::vector<uint8_t>& data, 
        const BackupMetadata& metadata);
    
    bool download_backup(
        const std::string& backup_id, 
        std::vector<uint8_t>* data, 
        BackupMetadata* metadata = nullptr);
    
    bool delete_backup(const std::string& backup_id);
    
    // Avatar Storage
    StorageUploadResult upload_avatar(
        const std::string& user_id, 
        const std::vector<uint8_t>& data, 
        const std::string& content_type);
    
    bool download_avatar(
        const std::string& user_id, 
        std::vector<uint8_t>* data, 
        std::string* content_type = nullptr);
    
    bool delete_avatar(const std::string& user_id);
    std::string get_avatar_url(const std::string& user_id) const;
    
    // Metadata Management
    bool save_modpack_metadata(const std::string& modpack_id, const ModpackMetadata& metadata);
    ModpackMetadata load_modpack_metadata(const std::string& modpack_id) const;
    bool delete_modpack_metadata(const std::string& modpack_id);
    
    bool save_backup_metadata(const std::string& backup_id, const BackupMetadata& metadata);
    BackupMetadata load_backup_metadata(const std::string& backup_id) const;
    bool delete_backup_metadata(const std::string& backup_id);
    
    // Statistics
    StorageStats get_stats() const;
    
    // Cleanup
    void cleanup_temp_files();

private:
    StorageManager();
    ~StorageManager();
    
    // Prevent copying
    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;
    
    // Utility Functions
    std::string generate_file_id() const;
    std::string get_extension_from_content_type(const std::string& content_type) const;
    std::string get_content_type_from_extension(const std::string& extension) const;
    std::string get_content_type_from_path(const std::wstring& path) const;
    
    // State
    mutable std::mutex metadata_mu_;
    std::map<std::string, ModpackMetadata> modpack_metadata_;
    std::map<std::string, BackupMetadata> backup_metadata_;
};

}  // namespace aml::storage
