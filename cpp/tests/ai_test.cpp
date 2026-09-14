#include "ai.h"

#include <iostream>
#include <string>

int main() {
    // Test 1: chat with no model available should return meaningful error
    {
        aml::ai::ChatRequest request;
        request.messages.push_back({"user", "hello"});
        request.max_tokens = 5;
        std::string reply;
        std::string error;
        // This will either succeed (if llama-completion.exe is available)
        // or fail with a meaningful error about missing tools/models
        aml::ai::chat(request, reply, &error);
        // We just verify it doesn't crash and either reply or error is set
        if (reply.empty() && error.empty()) {
            std::cerr << "FAILED: no reply and no error from chat\n";
            return 1;
        }
    }

    // Test 2: vision with no images should return error
    {
        aml::ai::VisionRequest request;
        request.prompt = "describe";
        std::string reply;
        std::string error;
        bool ok = aml::ai::vision(request, reply, &error);
        // Either succeeds (if tools available) or fails gracefully
        if (!ok && error.empty()) {
            std::cerr << "FAILED: vision returned false with no error\n";
            return 1;
        }
    }

    // Test 3: Art either produces a valid PNG or reports a useful unavailable
    // state when the optional multi-gigabyte model set is not installed.
    {
        aml::ai::ImageRequest request;
        request.prompt = "a simple purple sword on a dark background";
        std::vector<uint8_t> png;
        std::string error;
        bool ok = aml::ai::image(request, png, &error);
        if (ok) {
            if (png.size() < 8 || png[0] != 0x89 || png[1] != 'P' ||
                png[2] != 'N' || png[3] != 'G') {
                std::cerr << "FAILED: Art returned an invalid PNG\n";
                return 1;
            }
        } else if (error.empty()) {
            std::cerr << "FAILED: Art returned false with no error\n";
            return 1;
        }
    }

    std::cout << "AI native backend test: PASSED\n";
    return 0;
}
