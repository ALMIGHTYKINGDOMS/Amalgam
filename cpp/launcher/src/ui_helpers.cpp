#include "ui_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>

namespace aml::ui {

std::string format_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(*units)) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) std::snprintf(buffer, sizeof(buffer), "%llu %s",
                                  static_cast<unsigned long long>(bytes), units[unit]);
    else std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
    return buffer;
}

std::string format_download_count(int64_t downloads) {
    if (downloads < 1000) return std::to_string(std::max<int64_t>(0, downloads));
    const char* units[] = {"K", "M", "B"};
    double value = static_cast<double>(downloads);
    size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < std::size(units)) {
        value /= 1000.0;
        ++unit;
    }
    char buffer[32]{};
    const int decimals = value >= 100.0 ? 0 : value >= 10.0 ? 1 : 2;
    std::snprintf(buffer, sizeof(buffer), "%.*f%s", decimals, value, units[unit]);
    return buffer;
}

size_t next_utf8_boundary(const std::string& text, size_t offset) {
    if (offset >= text.size()) return text.size();
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    size_t bytes = 1;
    if ((lead & 0xE0) == 0xC0) bytes = 2;
    else if ((lead & 0xF0) == 0xE0) bytes = 3;
    else if ((lead & 0xF8) == 0xF0) bytes = 4;
    if (offset + bytes > text.size()) return offset + 1;
    for (size_t i = 1; i < bytes; ++i) {
        if ((static_cast<unsigned char>(text[offset + i]) & 0xC0) != 0x80) return offset + 1;
    }
    return offset + bytes;
}

std::string elide_to_width(const std::string& text, float max_width) {
    if (text.empty() || max_width <= 0.0f) return {};
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) return text;
    constexpr const char suffix[] = "...";
    const float suffix_width = ImGui::CalcTextSize(suffix).x;
    size_t cursor = 0;
    size_t accepted = 0;
    while (cursor < text.size()) {
        const size_t next = next_utf8_boundary(text, cursor);
        if (ImGui::CalcTextSize(text.c_str(), text.c_str() + next).x + suffix_width > max_width)
            break;
        accepted = next;
        cursor = next;
    }
    while (accepted > 0 && std::isspace(static_cast<unsigned char>(text[accepted - 1]))) --accepted;
    if (accepted == 0) return suffix_width <= max_width ? suffix : std::string();
    return text.substr(0, accepted) + suffix;
}

std::string humanize_error(const std::string& source) {
    if (source.empty()) return "The operation could not be completed.";
    std::string lower = source;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // Authentication providers commonly return transport details, JSON bodies,
    // and internal error identifiers in the same string.  Those are useful in
    // diagnostics, but never helpful or appropriate as the customer-facing
    // message in a sign-in or setup flow.
    if (lower.find("invalid_credentials") != std::string::npos ||
        lower.find("invalid login credentials") != std::string::npos ||
        lower.find("invalid email or password") != std::string::npos ||
        lower.find("invalid password") != std::string::npos) {
        return "That email or password does not match an Amalgam account. Check both, reset your password, or create an account.";
    }
    if (lower.find("email not confirmed") != std::string::npos ||
        lower.find("email_not_confirmed") != std::string::npos ||
        lower.find("confirm your email") != std::string::npos) {
        return "Verify this email first. Open the confirmation message we sent, then return here to sign in.";
    }
    if (lower.find("user already registered") != std::string::npos ||
        lower.find("email already registered") != std::string::npos ||
        lower.find("already been registered") != std::string::npos) {
        return "An Amalgam account already uses this email. Sign in instead, or reset its password if you need access.";
    }
    if (lower.find("password should be at least") != std::string::npos ||
        lower.find("password must be") != std::string::npos ||
        lower.find("weak password") != std::string::npos) {
        return "Choose a stronger password with at least eight characters, including a number and an uppercase letter.";
    }
    if (lower.find("invalid email") != std::string::npos ||
        lower.find("email address is invalid") != std::string::npos) {
        return "Enter a valid email address, then try again.";
    }
    if (lower.find("signup is disabled") != std::string::npos ||
        lower.find("signups not allowed") != std::string::npos) {
        return "New Amalgam accounts are not available right now. Please try again later.";
    }
    if (lower.find("supabase") != std::string::npos &&
        (lower.find("not initialized") != std::string::npos ||
         lower.find("not configured") != std::string::npos)) {
        return "Amalgam account services are not configured in this build yet. Check the launcher connection settings, then restart.";
    }
    if (lower.find("authorization_pending") != std::string::npos ||
        lower.find("the user must input their code") != std::string::npos) {
        return "Microsoft is waiting for you to finish sign-in in the browser. Enter the code, then return here.";
    }
    if (lower.find("unauthorized_client") != std::string::npos ||
        lower.find("application with identifier") != std::string::npos ||
        lower.find("minecraft approval") != std::string::npos ||
        lower.find("app not approved") != std::string::npos) {
        return "Microsoft/Xbox has not approved the Amalgam sign-in app for Minecraft yet. The integration is already configured; after approval propagates, the next sign-in retry should work without a launcher update or account change.";
    }
    if (lower.find("http status 401") != std::string::npos ||
        lower.find("http status 403") != std::string::npos) {
        return "The provider rejected the request. Check the account or provider connection, then retry.";
    }
    if (lower.find("http status 404") != std::string::npos) {
        return "The requested file or project is no longer available from the provider.";
    }
    if (lower.find("http status 429") != std::string::npos) {
        return "The provider is rate limiting requests. Wait a moment and retry.";
    }
    if (lower.find("http status 5") != std::string::npos ||
        lower.find("internet open failed") != std::string::npos ||
        lower.find("http send failed") != std::string::npos ||
        lower.find("timed out") != std::string::npos) {
        return "The online service is temporarily unavailable. Check your connection and retry.";
    }
    if (lower.find("http status 400") != std::string::npos ||
        lower.find("bad request") != std::string::npos) {
        return "The service could not accept that request. Check the details and try again.";
    }
    // Do not surface raw responses here: they can reveal internal provider
    // fields and tend to make a polished UI feel broken.  Callers retain the
    // original source for their own logs and developer diagnostics.
    return "We could not complete that request. Try again in a moment; if it keeps happening, open Developer diagnostics and include the time of the attempt.";
}

std::string compact_provider_text(const std::string& source, size_t maximum_bytes) {
    std::string out;
    out.reserve(source.size());
    bool previous_space = false;
    int consecutive_newlines = 0;
    for (unsigned char ch : source) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            if (consecutive_newlines++ < 1) out.push_back('\n');
            previous_space = false;
            continue;
        }
        consecutive_newlines = 0;
        if (ch < 0x20) continue;
        if (std::isspace(ch)) {
            if (!previous_space && !out.empty()) out.push_back(' ');
            previous_space = true;
        } else {
            out.push_back(static_cast<char>(ch));
            previous_space = false;
        }
        if (maximum_bytes > 0 && out.size() >= maximum_bytes) break;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    if (maximum_bytes > 0 && source.size() > out.size()) out += "...";
    return out;
}

std::string project_source_text(const mods::SearchResult& project, const mods::ModInfo& info) {
    return compact_provider_text(info.body.empty() ? project.description : info.body);
}

std::string format_rate(double bytes_per_second) {
    if (bytes_per_second <= 0.0) return {};
    return format_bytes(static_cast<uint64_t>(bytes_per_second)) + "/s";
}

std::string format_eta(double seconds) {
    if (seconds < 0.0) return {};
    const int total = std::max(0, static_cast<int>(seconds));
    const int minutes = total / 60;
    const int secs = total % 60;
    if (minutes > 0) return std::to_string(minutes) + "m " + std::to_string(secs) + "s left";
    return std::to_string(secs) + "s left";
}

std::string format_elapsed(uint64_t start, uint64_t end) {
    if (start == 0) return {};
    const uint64_t stop = end > 0 ? end : static_cast<uint64_t>(std::time(nullptr));
    const uint64_t seconds = stop > start ? stop - start : 0;
    const uint64_t hours = seconds / 3600;
    const uint64_t minutes = (seconds % 3600) / 60;
    const uint64_t remainder = seconds % 60;
    if (hours > 0) return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
    if (minutes > 0) return std::to_string(minutes) + "m " + std::to_string(remainder) + "s";
    return std::to_string(remainder) + "s";
}

const char* type_text(const std::string& t) {
    if (t == "release") return "RELEASE";
    if (t == "snapshot") return "SNAPSHOT";
    if (t == "old_beta" || t == "beta") return "BETA";
    if (t == "old_alpha") return "ALPHA";
    return "CUSTOM";
}

ImVec4 type_color(const std::string& t) {
    if (t == "release") return k.green;
    if (t == "snapshot") return k.blue;
    if (t == "old_beta" || t == "beta") return k.orange;
    if (t == "old_alpha") return k.red;
    return k.muted;
}

}  // namespace aml::ui
