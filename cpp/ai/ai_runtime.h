#pragma once

#include "ai_core.h"

#include <functional>
#include <string>
#include <vector>

namespace aml::ai {

// ──────────────────────────────────────────────
// Brain / Vision runtime
// ──────────────────────────────────────────────

struct GenerationOptions {
    double  temperature = 0.7;
    int     max_tokens = 2048;
    int     top_k = 40;
    float   top_p = 0.9f;
    bool    stream = false;
};

// Chat message for the runtime (system/user/assistant)
struct RuntimeMessage {
    std::string role;
    std::string content;
    // optional base64-encoded images (vision)
    std::vector<std::string> images_b64;   // data URIs
};

// Progress callback: (processed chars, total chars estimate, stage label)
using StreamCallback = std::function<bool(const std::string& partial, const std::string& stage)>;

// A local inference runtime backed exclusively by the Amalgam-installed
// native llama.cpp/libmtmd runtime.
class BrainRuntime {
public:
    virtual ~BrainRuntime() = default;

    // Load a model from the given GGUF path. Returns false + error on failure.
    virtual bool load(const std::wstring& model_path, const std::wstring& mmproj_path,
                      std::string* err) = 0;
    virtual bool loaded() const = 0;
    virtual bool unload(std::string* err) = 0;

    // Generate a completion. If stream_cb is set, call it incrementally.
    virtual bool generate(const std::vector<RuntimeMessage>& msgs,
                          const GenerationOptions& opts,
                          std::string& out_text,
                          StreamCallback stream_cb,
                          std::string* err) = 0;

    // Estimate context window in tokens.
    virtual int context_tokens() const = 0;
};

// ──────────────────────────────────────────────
// Art runtime
// ──────────────────────────────────────────────

struct ArtRequest {
    std::string prompt;
    int         width = 512;
    int         height = 512;
    int         steps = 20;
    float       cfg_scale = 7.0f;
    std::string negative_prompt;
    std::string seed;         // "" = random
};

class ArtRuntime {
public:
    virtual ~ArtRuntime() = default;

    virtual bool load(const std::wstring& model_path, std::string* err) = 0;
    virtual bool loaded() const = 0;
    virtual bool unload(std::string* err) = 0;

    // Generate an image, returning PNG bytes.
    virtual bool generate(const ArtRequest& req, std::vector<uint8_t>& out_png,
                          std::string* err) = 0;
};

// ──────────────────────────────────────────────
// Factory helpers
// ──────────────────────────────────────────────

enum class RuntimeBackend { None = 0, EmbeddedNative };

// Detect which backend is available for this machine.
RuntimeBackend detect_runtime_backend(const HardwareInfo& hw);

}  // namespace aml::ai