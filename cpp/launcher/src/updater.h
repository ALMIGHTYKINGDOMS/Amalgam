#pragma once

// Launcher self-update.
//
// The launcher never trusts an arbitrary client-provided update URL: the
// manifest endpoint is a backend-owned constant (override-able only for
// tests/debug via config, never from user input), and every payload is
// verified by exact size + SHA-256 before it is ever allowed to replace a
// live file. A signed archive must also contain a validated full-release
// component manifest and hash inventory. Replacement is staged outside the
// running executable and applied by a generated helper batch that updates
// only product-owned components, retains a last-known-good transaction plan,
// and restores every replaced component on failure.

#include <atomic>
#include <string>

namespace aml::updater {

// Update channels.
enum class Channel { Stable, Beta };

struct UpdateInfo {
    std::string version;         // e.g. "3.0.1"
    std::string channel;         // "stable" or "beta"
    std::string download_url;    // https://... backend-owned payload URL
    std::string sha256;          // hex digest of the payload (64 chars)
    std::string signature;       // optional RSA-SHA256 over the PAYLOAD bytes
    std::string manifest_signature;  // required RSA-SHA256 over the canonical
                                     // manifest fields (binds version, channel,
                                     // URL, size, hash, min_version)
    std::string min_version;     // lowest launcher version this update supports
    std::string notes;           // release notes
    int64_t size = -1;           // payload size in bytes
    bool mandatory = false;      // backend-forced update
};

// Result state surfaced to the UI.
enum class CheckState {
    Idle,       // no check performed yet
    Checking,   // background check in flight
    UpToDate,   // running the newest available version
    Available,  // an update is available (optional)
    Mandatory,  // an update is required by the backend
    Offline,    // provider unreachable / no network
    Error,      // provider reached but the check failed
};

struct UpdateStatus {
    std::atomic<CheckState> state{CheckState::Idle};
    std::string available_version;
    std::string error;
    std::string notes;
    std::atomic_bool staged{false};         // payload downloaded and verified
    std::atomic_bool signature_verified{false};  // payload signature verified
    std::atomic_bool manifest_signed{false};     // manifest carries a signature
};

// Compare dotted numeric versions ("3.0", "3.0.1", "3.1.0-beta.2").
// Returns < 0 when a < b, 0 when equal, > 0 when a > b.
int compare_versions(const std::string& a, const std::string& b);

// Version policy (anti-rollback). An update is applicable only when it is
// strictly newer than the running version AND the running version satisfies
// the manifest's minimum-version requirement. A stale or tampered manifest
// naming an older or equal version is refused, as is an update whose
// minimum-version requirement the running launcher cannot meet. When
// `current_channel` is non-empty and "stable", a beta update is refused so a
// release build is never silently moved to a prerelease channel.
bool should_apply(const UpdateInfo& info, const std::string& current_version,
                  const std::string& current_channel, std::string* err);

// Backend release-manifest endpoint. Compile-time constant; overridable
// only for tests/debug through the launcher config.
std::wstring default_manifest_url();

// Validate raw manifest text into UpdateInfo. This is the single validation
// seam: fetch_manifest() downloads the text and delegates here, and tests
// exercise the same code with crafted documents. A remote SHA-256 is NOT
// authentication on its own. Every accepted manifest must have a valid
// 64-character SHA-256, a positive bounded payload size, a valid manifest
// signature, and a configured update-signing public key. The optional payload
// signature is checked during staging when present.
bool parse_manifest_text(const std::string& text, UpdateInfo& out, std::string* err);

// Fetch and validate the update manifest. `url` is the backend endpoint,
// never user input. Returns false with a human-readable error in `err`
// (offline, HTTP failure, malformed manifest, unacceptable payload).
bool fetch_manifest(const std::wstring& url, UpdateInfo& out, std::string* err);

// Deterministic canonical JSON for the security-relevant manifest fields.
// The release tooling signs exactly these bytes, and verification recomputes
// them from the parsed fields, so version/channel/URL/size/hash are all bound
// by the signature — not just the payload hash.
std::string canonical_manifest_json(const UpdateInfo& info);

// Verify an RSA-SHA256 signature over an in-memory canonical manifest.
bool verify_manifest_signature(const std::string& pem_public_key,
                               const std::string& canonical_json,
                               const std::string& base64_signature,
                               std::string* err);

// Configure the payload/manifest-signing public key (PEM, "BEGIN PUBLIC
// KEY"). The private key must never exist in the repository; signing is a
// release-engineering step. Production initializes this from the build-time
// embedded PEM. Until a key is configured, every remote manifest is rejected
// (fail closed). This setter also permits isolated unit-test fixtures.
void set_signing_public_key(const std::string& pem);
bool has_signing_key();

// Configure the verification key from the build-time embedded PEM (see
// AMALGAM_UPDATER_PUBLIC_KEY_FILE). No-op when the release build was
// configured without a key, leaving all remote manifests fail closed.
void init_from_embedded_key();

// Verify an RSA-SHA256 signature over a payload file. `base64_signature` is
// the base64 signature from the manifest; `payload_path` is the staged file.
// Returns true only when the signature matches the file content.
bool verify_payload_signature(const std::string& pem_public_key,
                              const std::string& base64_signature,
                              const std::wstring& payload_path,
                              std::string* err);

// Revalidate the authenticated manifest, then download the payload into the
// staging directory next to the executable and verify exact size + SHA-256.
// Never touches the running executable. On success `staged_path` is the
// verified archive path.
bool stage_update(const UpdateInfo& info, const std::wstring& exe_dir,
                  std::wstring& staged_path, std::string* err);

// Prepare a full release transaction for a staged update: revalidate the
// authenticated archive, extract it to staging, validate its component
// manifest/hash inventory, and generate an all-component helper plan. The
// helper updates only the declared product files (EXE, DLL, bridges, Bedrock,
// packaged assets and release metadata); it never addresses live config,
// profiles, instances, managed Java, or the update workspace. Returns the
// helper batch path on success.
bool prepare_apply(const UpdateInfo& info, const std::wstring& exe_dir,
                   const std::wstring& staged_path, std::wstring& helper_path,
                   std::string* err);

// Restore every product component recorded by the most recent completed
// transactional backup. A legacy executable-only backup remains supported for
// upgrades created by older launcher builds.
bool rollback_update(const std::wstring& exe_dir, std::string& error);

}  // namespace aml::updater
