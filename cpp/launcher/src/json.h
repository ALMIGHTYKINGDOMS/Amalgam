#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace aml {

class Json {
public:
    enum class Type { Null, Bool, Num, Str, Arr, Obj };
    using Pair = std::pair<std::string, Json>;
    using Value = Json;
    static constexpr Type nullValue = Type::Null;
    static constexpr Type arrayValue = Type::Arr;
    static constexpr Type objectValue = Type::Obj;

    struct CharReaderBuilder {};
    struct StreamWriterBuilder {};

    Json() = default;
    Json(Type type);
    Json(bool value);
    Json(int value);
    Json(double value);
    Json(int64_t value);
    Json(const std::string& value);
    Json(const char* value);

    static Json parse(const std::string& text, std::string* err = nullptr);
    std::string dump() const;

    Type type() const;
    bool is(Type t) const;
    bool isNull() const { return is(Type::Null); }
    bool isArray() const { return is(Type::Arr); }
    bool isObject() const { return is(Type::Obj); }
    bool as_bool(bool def = false) const;
    double as_num(double def = 0.0) const;
    int64_t as_int(int64_t def = 0) const;
    std::string as_str(const std::string& def = std::string()) const;
    bool asBool() const { return as_bool(); }
    int asInt() const { return static_cast<int>(as_int()); }
    int64_t asInt64() const { return as_int(); }
    uint64_t asUInt64() const { return static_cast<uint64_t>(as_int()); }
    std::string asString() const { return as_str(); }
    size_t size() const;
    const Json& get(const std::string& key) const;
    const Json* find(const std::string& key) const;
    const Json& at(size_t i) const;
    const Json& at(const std::string& key) const { return get(key); }
    Json& operator[](const std::string& key);
    const Json& operator[](const std::string& key) const { return get(key); }
    Json& operator[](const char* key) { return (*this)[std::string(key ? key : "")]; }
    const Json& operator[](const char* key) const { return get(key ? key : ""); }
    Json& operator[](size_t i);
    const Json& operator[](size_t i) const { return at(i); }
    bool isMember(const std::string& key) const { return find(key) != nullptr; }
    std::vector<std::string> getMemberNames() const;
    const std::vector<Pair>& pairs() const;
    const std::vector<Json>& items() const;

    std::vector<Json>::const_iterator begin() const { return items().begin(); }
    std::vector<Json>::const_iterator end() const { return items().end(); }

    static Json obj();
    static Json arr();
    static Json str(const std::string& v);
    static Json num(double v);
    static Json boolean(bool v);

    void set(const std::string& key, Json v);
    void push(Json v);
    void append(Json v) { push(std::move(v)); }

    static bool parseFromStream(const CharReaderBuilder&, std::istream& input,
                                Json* root, std::string* errors);
    static std::string writeString(const StreamWriterBuilder&, const Json& value) {
        return value.dump();
    }
    std::string toStyledString() const { return dump(); }

private:
    using Var = std::variant<std::monostate, bool, double, std::string, std::vector<Json>,
                             std::vector<Pair>>;
    Var v_;
};

bool json_parse_file(const std::wstring& path, Json& out, std::string* err);
bool json_write_file(const std::wstring& path, const Json& j, std::string* err);

}  // namespace aml
