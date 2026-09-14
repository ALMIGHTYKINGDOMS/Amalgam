#include "project_publishing.h"

#include "json.h"

namespace aml::publishing {

namespace {

Json strings(const std::vector<std::string>& values) {
    Json result = Json::arr();
    for (const auto& value : values) result.push(Json::str(value));
    return result;
}

Project project_from_json(const Json& value) {
    Project p;
    p.id = value.get("id").as_str();
    p.owner_id = value.get("owner_id").as_str();
    p.name = value.get("name").as_str();
    p.slug = value.get("slug").as_str();
    p.description = value.get("description").as_str();
    p.type = value.get("project_type").as_str();
    p.status = value.get("status").as_str();
    p.approved = !value.get("approved_at").as_str().empty();
    p.current_version_id = value.get("current_version_id").as_str();
    return p;
}

Version version_from_json(const Json& value) {
    Version v;
    v.id = value.get("id").as_str();
    v.project_id = value.get("project_id").as_str();
    v.version = value.get("version").as_str();
    v.changelog = value.get("changelog").as_str();
    v.artifact_path = value.get("artifact_path").as_str();
    v.artifact_sha256 = value.get("artifact_sha256").as_str();
    v.artifact_size = value.get("artifact_size").as_int();
    v.status = value.get("status").as_str();
    return v;
}

bool first_error(const SupabaseClient::DBResult& result, std::string* error) {
    if (result.success) return false;
    if (error) *error = result.error.empty() ? "publishing request failed" : result.error;
    return true;
}

} // namespace

bool create_project(SupabaseClient& client, const ProjectDraft& draft,
                    Project& out, std::string* error) {
    Json row = Json::obj();
    row.set("owner_id", Json::str(client.current_user().id));
    row.set("name", Json::str(draft.name));
    row.set("slug", Json::str(draft.slug));
    row.set("description", Json::str(draft.description));
    row.set("project_type", Json::str(draft.type));
    row.set("license", Json::str(draft.license));
    row.set("tags", strings(draft.tags));
    SupabaseClient::DBInsertOptions options;
    options.table = "projects";
    options.records.push_back(row);
    const auto result = client.insert(options);
    if (first_error(result, error) || result.data.empty()) {
        if (error && result.success) *error = "project was not returned by the database";
        return false;
    }
    out = project_from_json(result.data.front());
    return true;
}

bool list_owned_projects(SupabaseClient& client, std::vector<Project>& out,
                         std::string* error) {
    out.clear();
    SupabaseClient::DBQueryOptions options;
    options.table = "projects";
    options.eq_filters["owner_id"] = client.current_user().id;
    options.order_by = "updated_at";
    options.order_asc = false;
    const auto result = client.select(options);
    if (first_error(result, error)) return false;
    for (const auto& row : result.data) out.push_back(project_from_json(row));
    return true;
}

bool submit_for_review(SupabaseClient& client, const std::string& project_id,
                       std::string* error) {
    Json args = Json::obj();
    args.set("p_project_id", Json::str(project_id));
    return !first_error(client.rpc("submit_project_for_review", args), error);
}

bool create_version(SupabaseClient& client, const Version& version,
                    Version& out, std::string* error) {
    Json row = Json::obj();
    row.set("project_id", Json::str(version.project_id));
    row.set("version", Json::str(version.version));
    row.set("changelog", Json::str(version.changelog));
    row.set("artifact_path", Json::str(version.artifact_path));
    row.set("artifact_sha256", Json::str(version.artifact_sha256));
    row.set("artifact_size", Json::num(static_cast<double>(version.artifact_size)));
    row.set("created_by", Json::str(client.current_user().id));
    SupabaseClient::DBInsertOptions options;
    options.table = "project_versions";
    options.records.push_back(row);
    const auto result = client.insert(options);
    if (first_error(result, error) || result.data.empty()) {
        if (error && result.success) *error = "version was not returned by the database";
        return false;
    }
    out = version_from_json(result.data.front());
    return true;
}

bool publish_version(SupabaseClient& client, const std::string& project_id,
                     const std::string& version_id, std::string* error) {
    Json args = Json::obj();
    args.set("p_project_id", Json::str(project_id));
    args.set("p_version_id", Json::str(version_id));
    return !first_error(client.rpc("publish_project_version", args), error);
}

bool add_media(SupabaseClient& client, const Media& media, std::string* error) {
    Json row = Json::obj();
    row.set("project_id", Json::str(media.project_id));
    if (!media.version_id.empty()) row.set("version_id", Json::str(media.version_id));
    row.set("kind", Json::str(media.kind));
    row.set("storage_path", Json::str(media.storage_path));
    row.set("mime_type", Json::str(media.mime_type));
    row.set("size_bytes", Json::num(static_cast<double>(media.size_bytes)));
    row.set("sha256", Json::str(media.sha256));
    row.set("title", Json::str(media.title));
    SupabaseClient::DBInsertOptions options;
    options.table = "project_media";
    options.records.push_back(row);
    return !first_error(client.insert(options), error);
}

bool list_moderation_queue(SupabaseClient& client, std::vector<Project>& out,
                           std::string* error) {
    out.clear();
    SupabaseClient::DBQueryOptions options;
    options.table = "projects";
    options.eq_filters["status"] = "pending_review";
    options.order_by = "created_at";
    options.order_asc = true;
    const auto result = client.select(options);
    if (first_error(result, error)) return false;
    for (const auto& row : result.data) out.push_back(project_from_json(row));
    return true;
}

bool review_project(SupabaseClient& client, const std::string& project_id,
                    const std::string& decision, const std::string& reason,
                    std::string* error) {
    Json args = Json::obj();
    args.set("p_project_id", Json::str(project_id));
    args.set("p_decision", Json::str(decision));
    args.set("p_reason", Json::str(reason));
    return !first_error(client.rpc("review_project", args), error);
}

} // namespace aml::publishing
