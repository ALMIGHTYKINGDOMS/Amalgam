#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace aml::ai {

// AI requests always use the models and runtimes installed by Amalgam. There
// is deliberately no provider URL or API-key field in this runtime interface.
struct ChatMsg {
    std::string role;  // "system" | "user" | "assistant"
    std::string text;
};

struct ChatRequest {
    std::vector<ChatMsg> messages;
    double temperature = 0.7;
    int max_tokens = 2048;
};

struct VisionItem {
    std::string mime;  // e.g. "image/png"
    std::vector<uint8_t> data;
    std::string text;
};

struct VisionRequest {
    std::string prompt;
    std::vector<VisionItem> items;
};

struct ImageRequest {
    std::string prompt;
    std::string size = "1024x1024";
};

struct LocalRuntimeStatus {
    bool brain_runtime = false;
    bool brain_model = false;
    bool vision_runtime = false;
    bool vision_projector = false;
    bool art_runtime = false;
    bool art_model = false;
    bool art_encoder = false;
    bool art_decoder = false;

    bool brain_ready() const { return brain_runtime && brain_model; }
    bool vision_ready() const { return vision_runtime && brain_model && vision_projector; }
    bool art_ready() const { return art_runtime && art_model && art_encoder && art_decoder; }
};

// Inspect every locally installed runtime/model component without starting a
// model process.  Callers use this for truthful diagnostics and setup UI.
LocalRuntimeStatus local_runtime_status();
// Returns whether the bundled brain runtime and model are discoverable.
bool local_runtime_available(std::string* err = nullptr);
// Returns the assistant text reply from the bundled brain runtime.
bool chat(const ChatRequest& req, std::string& reply, std::string* err);
// Streaming chat: calls on_chunk(text) as tokens arrive. Returns full reply.
// on_chunk is called from the worker thread -- it must be thread-safe.
typedef void (*StreamCallback)(const std::string& chunk, void* user_data);
bool chat_stream(const ChatRequest& req, StreamCallback on_chunk, void* user_data,
                 const std::atomic_bool* cancel_requested,
                 std::string& reply, std::string* err);
// sends prompt + images, returns text description.  The optional cancellation
// flag is checked while the native child process is running.
bool vision(const VisionRequest& req, std::string& reply, std::string* err,
            const std::atomic_bool* cancel_requested = nullptr);
// returns raw PNG bytes.  The optional cancellation flag lets the UI stop a
// long art process during shutdown or when the user presses Cancel.
bool image(const ImageRequest& req, std::vector<uint8_t>& png, std::string* err,
           const std::atomic_bool* cancel_requested = nullptr);

}  // namespace aml::ai