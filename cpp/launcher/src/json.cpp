#include "json.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace aml {

namespace {

thread_local int g_parse_depth = 0;

const Json& null_value() {
    static Json j;
    return j;
}

void skip_ws(const char*& p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_hex4(const char*& p, unsigned& code, std::string* err) {
    code = 0;
    for (int k = 0; k < 4; ++k) {
        const char h = *p;
        if (!h) {
            if (err) *err = "bad \\u escape";
            return false;
        }
        ++p;
        const int digit = hex_digit(h);
        if (digit < 0) {
            if (err) *err = "bad \\u escape";
            return false;
        }
        code = (code << 4) | static_cast<unsigned>(digit);
    }
    return true;
}

void append_utf8(std::string& out, unsigned codepoint) {
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

std::string parse_string(const char*& p, std::string* err) {
    ++p;
    std::string out;
    while (*p && *p != '"') {
        const unsigned char c = static_cast<unsigned char>(*p++);
        if (c == '\\') {
            if (!*p) {
                if (err) *err = "unterminated escape";
                return out;
            }
            switch (*p++) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned code = 0;
                    if (!parse_hex4(p, code, err)) return out;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (p[0] != '\\' || p[1] != 'u') {
                            if (err) *err = "unpaired high surrogate";
                            return out;
                        }
                        p += 2;
                        unsigned low = 0;
                        if (!parse_hex4(p, low, err)) return out;
                        if (low < 0xDC00 || low > 0xDFFF) {
                            if (err) *err = "invalid low surrogate";
                            return out;
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        if (err) *err = "unpaired low surrogate";
                        return out;
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    if (err) *err = "bad escape";
                    return out;
            }
        } else {
            if (c < 0x20) {
                if (err) *err = "unescaped control character";
                return out;
            }
            out += static_cast<char>(c);
        }
    }
    if (*p != '"') {
        if (err) *err = "unterminated string";
        return out;
    }
    ++p;
    return out;
}

Json parse_value(const char*& p, std::string* err);

Json parse_object(const char*& p, std::string* err) {
    Json j = Json::obj();
    ++p;
    skip_ws(p);
    if (*p == '}') {
        ++p;
        return j;
    }
    for (;;) {
        skip_ws(p);
        if (*p != '"') {
            if (err) *err = "expected key string";
            return j;
        }
        std::string key = parse_string(p, err);
        if (err && !err->empty()) return j;
        skip_ws(p);
        if (*p != ':') {
            if (err) *err = "expected ':'";
            return j;
        }
        ++p;
        skip_ws(p);
        Json v = parse_value(p, err);
        if (err && !err->empty()) return j;
        j.set(key, std::move(v));
        skip_ws(p);
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}') {
            ++p;
            break;
        }
        if (err) *err = "expected ',' or '}'";
        break;
    }
    return j;
}

Json parse_array(const char*& p, std::string* err) {
    Json j = Json::arr();
    ++p;
    skip_ws(p);
    if (*p == ']') {
        ++p;
        return j;
    }
    for (;;) {
        skip_ws(p);
        Json v = parse_value(p, err);
        if (err && !err->empty()) return j;
        j.push(std::move(v));
        skip_ws(p);
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == ']') {
            ++p;
            break;
        }
        if (err) *err = "expected ',' or ']'";
        break;
    }
    return j;
}

Json parse_value(const char*& p, std::string* err) {
    if (++g_parse_depth > 128) {
        --g_parse_depth;
        if (err) *err = "JSON nesting limit exceeded";
        return Json();
    }
    struct DepthGuard {
        ~DepthGuard() { --g_parse_depth; }
    } depth_guard;
    skip_ws(p);
    char c = *p;
    if (c == '{') return parse_object(p, err);
    if (c == '[') return parse_array(p, err);
    if (c == '"') return Json::str(parse_string(p, err));
    if (c == 't') {
        if (std::strncmp(p, "true", 4) != 0) {
            if (err) *err = "invalid true literal";
            return Json();
        }
        p += 4;
        return Json::boolean(true);
    }
    if (c == 'f') {
        if (std::strncmp(p, "false", 5) != 0) {
            if (err) *err = "invalid false literal";
            return Json();
        }
        p += 5;
        return Json::boolean(false);
    }
    if (c == 'n') {
        if (std::strncmp(p, "null", 4) != 0) {
            if (err) *err = "invalid null literal";
            return Json();
        }
        p += 4;
        return Json();
    }
    const char* start = p;
    if (c == '-' || (c >= '0' && c <= '9')) {
        if (*p == '-') ++p;
        if (*p == '0') {
            ++p;
        } else if (*p >= '1' && *p <= '9') {
            while (*p >= '0' && *p <= '9') ++p;
        } else {
            if (err) *err = "invalid number";
            return Json();
        }
        if (*p == '.') {
            ++p;
            if (!(*p >= '0' && *p <= '9')) {
                if (err) *err = "invalid number fraction";
                return Json();
            }
            while (*p >= '0' && *p <= '9') ++p;
        }
        if (*p == 'e' || *p == 'E') {
            ++p;
            if (*p == '+' || *p == '-') ++p;
            if (!(*p >= '0' && *p <= '9')) {
                if (err) *err = "invalid number exponent";
                return Json();
            }
            while (*p >= '0' && *p <= '9') ++p;
        }
        char* end = nullptr;
        double d = std::strtod(start, &end);
        if (end != p) {
            if (err) *err = "invalid number";
            return Json();
        }
        return Json::num(d);
    }
    if (err) *err = std::string("unexpected char '") + c + "'";
    return Json();
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

Json::Type Json::type() const {
    switch (v_.index()) {
        case 1: return Type::Bool;
        case 2: return Type::Num;
        case 3: return Type::Str;
        case 4: return Type::Arr;
        case 5: return Type::Obj;
        default: return Type::Null;
    }
}

Json::Json(Type type) {
    if (type == Type::Arr) v_ = std::vector<Json>{};
    else if (type == Type::Obj) v_ = std::vector<Pair>{};
    else if (type == Type::Bool) v_ = false;
    else if (type == Type::Num) v_ = 0.0;
}

Json::Json(bool value) : v_(value) {}
Json::Json(int value) : v_(static_cast<double>(value)) {}
Json::Json(double value) : v_(value) {}
Json::Json(int64_t value) : v_(static_cast<double>(value)) {}
Json::Json(const std::string& value) : v_(value) {}
Json::Json(const char* value) : v_(std::string(value ? value : "")) {}

bool Json::is(Type t) const { return type() == t; }

bool Json::as_bool(bool def) const {
    if (auto* b = std::get_if<bool>(&v_)) return *b;
    return def;
}

double Json::as_num(double def) const {
    if (auto* n = std::get_if<double>(&v_)) return *n;
    return def;
}

int64_t Json::as_int(int64_t def) const {
    if (auto* n = std::get_if<double>(&v_)) return static_cast<int64_t>(*n);
    return def;
}

std::string Json::as_str(const std::string& def) const {
    if (auto* s = std::get_if<std::string>(&v_)) return *s;
    return def;
}

size_t Json::size() const {
    if (auto* a = std::get_if<std::vector<Json>>(&v_)) return a->size();
    if (auto* o = std::get_if<std::vector<Pair>>(&v_)) return o->size();
    return 0;
}

const Json& Json::get(const std::string& key) const {
    const Json* j = find(key);
    return j ? *j : null_value();
}

const Json* Json::find(const std::string& key) const {
    if (auto* o = std::get_if<std::vector<Pair>>(&v_)) {
        for (const Pair& p : *o) {
            if (p.first == key) return &p.second;
        }
    }
    return nullptr;
}

const Json& Json::at(size_t i) const {
    if (auto* a = std::get_if<std::vector<Json>>(&v_)) {
        if (i < a->size()) return (*a)[i];
    }
    return null_value();
}

Json& Json::operator[](const std::string& key) {
    auto* o = std::get_if<std::vector<Pair>>(&v_);
    if (!o) {
        v_ = std::vector<Pair>{};
        o = std::get_if<std::vector<Pair>>(&v_);
    }
    for (Pair& p : *o) {
        if (p.first == key) return p.second;
    }
    o->emplace_back(key, Json{});
    return o->back().second;
}

Json& Json::operator[](size_t i) {
    auto* a = std::get_if<std::vector<Json>>(&v_);
    if (!a) {
        v_ = std::vector<Json>{};
        a = std::get_if<std::vector<Json>>(&v_);
    }
    if (i >= a->size()) a->resize(i + 1);
    return (*a)[i];
}

std::vector<std::string> Json::getMemberNames() const {
    std::vector<std::string> names;
    for (const Pair& pair : pairs()) names.push_back(pair.first);
    return names;
}

const std::vector<Json::Pair>& Json::pairs() const {
    static const std::vector<Pair> empty;
    if (auto* o = std::get_if<std::vector<Pair>>(&v_)) return *o;
    return empty;
}

const std::vector<Json>& Json::items() const {
    static const std::vector<Json> empty;
    if (auto* a = std::get_if<std::vector<Json>>(&v_)) return *a;
    return empty;
}

Json Json::obj() {
    Json j;
    j.v_ = std::vector<Pair>{};
    return j;
}
Json Json::arr() { Json j; j.v_ = std::vector<Json>{}; return j; }
Json Json::str(const std::string& v) { Json j; j.v_ = v; return j; }
Json Json::num(double v) { Json j; j.v_ = v; return j; }
Json Json::boolean(bool v) { Json j; j.v_ = v; return j; }

void Json::set(const std::string& key, Json v) {
    auto* o = std::get_if<std::vector<Pair>>(&v_);
    if (!o) {
        v_ = std::vector<Pair>{};
        o = std::get_if<std::vector<Pair>>(&v_);
    }
    for (Pair& p : *o) {
        if (p.first == key) {
            p.second = std::move(v);
            return;
        }
    }
    o->emplace_back(key, std::move(v));
}

void Json::push(Json v) {
    auto* a = std::get_if<std::vector<Json>>(&v_);
    if (!a) {
        v_ = std::vector<Json>{};
        a = std::get_if<std::vector<Json>>(&v_);
    }
    a->push_back(std::move(v));
}

bool Json::parseFromStream(const CharReaderBuilder&, std::istream& input,
                           Json* root, std::string* errors) {
    if (!root) {
        if (errors) *errors = "null JSON output";
        return false;
    }
    std::ostringstream text;
    text << input.rdbuf();
    *root = Json::parse(text.str(), errors);
    return !errors || errors->empty();
}

std::string Json::dump() const {
    switch (type()) {
        case Type::Null: return "null";
        case Type::Bool: return as_bool() ? "true" : "false";
        case Type::Num: {
            double d = as_num();
            if (d == static_cast<int64_t>(d)) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
                return buf;
            }
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            return buf;
        }
        case Type::Str: return "\"" + escape(as_str()) + "\"";
        case Type::Arr: {
            std::string s = "[";
            bool first = true;
            for (const Json& item : items()) {
                if (!first) s += ",";
                s += item.dump();
                first = false;
            }
            return s + "]";
        }
        case Type::Obj: {
            std::string s = "{";
            bool first = true;
            for (const Pair& p : pairs()) {
                if (!first) s += ",";
                s += "\"" + escape(p.first) + "\":" + p.second.dump();
                first = false;
            }
            return s + "}";
        }
    }
    return "null";
}

Json Json::parse(const std::string& text, std::string* err) {
    // Error parameters are output values. Callers commonly reuse a single
    // string across a recovery path, so an earlier failure must not cause a
    // later valid document to be reported as malformed.
    if (err) err->clear();
    const char* p = text.c_str();
    Json j = parse_value(p, err);
    skip_ws(p);
    if (err && !err->empty()) return j;
    if (*p) {
        if (err) *err = "trailing content";
    }
    return j;
}

bool json_parse_file(const std::wstring& path, Json& out, std::string* err) {
    if (err) err->clear();
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        if (err) *err = "cannot open file";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    out = Json::parse(text, err);
    if (err && !err->empty()) {
        // The primary file is corrupt or was truncated mid-write. If a
        // last-known-good backup exists (written atomically alongside the
        // primary by json_write_file), recover from it instead of failing
        // the load.
        std::ifstream bf(path + L".bak", std::ios::binary);
        if (bf.is_open()) {
            std::ostringstream bss;
            bss << bf.rdbuf();
            Json backup = Json::parse(bss.str(), err);
            if (err && err->empty()) {
                out = std::move(backup);
                return true;
            }
        }
    }
    return err == nullptr || err->empty();
}

bool json_write_file(const std::wstring& path, const Json& j, std::string* err) {
    if (err) err->clear();
    const std::string text = j.dump();

    // 1. Serialize to a unique temporary file in the same directory (same
    //    volume, so the final replacement is atomic). The destination is
    //    never touched until the temporary file is fully written, flushed
    //    and validated, so a crash mid-write cannot destroy the last good
    //    configuration.
    static std::atomic<unsigned> g_temp_counter{0};
    const std::wstring tmp =
        path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(g_temp_counter.fetch_add(1));

    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f.is_open()) {
            if (err) *err = "cannot write file";
            return false;
        }
        f << text;
        f.flush();
        f.close();
        if (!f) {
            // Disk full / write error: the previous file is untouched.
            if (err) *err = "could not finish writing file";
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    // 2. Validate the serialized output by parsing it back before it can
    //    ever become the live file.
    {
        Json check;
        std::string check_err;
        if (!json_parse_file(tmp, check, &check_err)) {
            if (err) *err = "serialized output failed validation: " + check_err;
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    // 3. Atomically replace the destination, keeping the previous contents
    //    as a last-known-good <path>.bak. ReplaceFileW performs the swap and
    //    backup in one operation; if the destination does not exist yet (or
    //    the volume does not support ReplaceFileW), fall back to an atomic
    //    move.
    const std::wstring bak = path + L".bak";
    const DWORD attrs = GetFileAttributesW(path.c_str());
    const bool existed =
        attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);

    if (existed) {
        DeleteFileW(bak.c_str());  // ReplaceFileW requires the backup to be absent
        if (ReplaceFileW(path.c_str(), tmp.c_str(), bak.c_str(),
                         REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            return true;
        }
        // Fall through to the move path on any ReplaceFileW failure; the
        // temporary file is still intact.
    }

    if (MoveFileExW(tmp.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    if (err) *err = "could not replace file (error " + std::to_string(GetLastError()) + ")";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return false;
}

}  // namespace aml
