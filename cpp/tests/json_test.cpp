#include "json.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

bool test_parse_basic() {
    std::string err;
    aml::Json j = aml::Json::parse(R"({"id":"1.21.11","type":"release","count":3,"ok":true,"tags":["a","b"]})", &err);
    if (!j.is(aml::Json::Type::Obj) || !err.empty()) return false;
    return j.get("id").as_str() == "1.21.11" && j.get("type").as_str() == "release" &&
           j.get("count").as_int() == 3 && j.get("ok").as_bool() == true &&
           j.get("tags").is(aml::Json::Type::Arr) && j.get("tags").size() == 2 &&
           j.get("tags").at(1).as_str() == "b";
}

bool test_parse_nested() {
    std::string err;
    aml::Json j = aml::Json::parse(R"({"downloads":{"client":{"url":"https://x/y.jar","size":123}}})", &err);
    if (err.empty() == false) return false;
    const aml::Json& client = j.get("downloads").get("client");
    return client.get("url").as_str() == "https://x/y.jar" && client.get("size").as_int() == 123;
}

bool test_parse_errors() {
    std::string err;
    aml::Json bad = aml::Json::parse("{not json", &err);
    if (err.empty()) return false;
    err.clear();
    aml::Json empty = aml::Json::parse("", &err);
    return !err.empty() && empty.is(aml::Json::Type::Null);
}

bool test_reused_error_output() {
    std::string err = "previous failure";
    aml::Json parsed = aml::Json::parse(R"({"status":"ready"})", &err);
    return err.empty() && parsed.get("status").as_str() == "ready";
}

bool test_build_dump_roundtrip() {
    aml::Json root = aml::Json::obj();
    root.set("name", aml::Json::str("Amalgam"));
    root.set("memory", aml::Json::num(4096));
    root.set("enabled", aml::Json::boolean(true));
    aml::Json list = aml::Json::arr();
    list.push(aml::Json::str("a"));
    list.push(aml::Json::num(2));
    root.set("list", list);
    std::string err;
    aml::Json reparsed = aml::Json::parse(root.dump(), &err);
    return err.empty() && reparsed.get("name").as_str() == "Amalgam" &&
           reparsed.get("memory").as_int() == 4096 && reparsed.get("enabled").as_bool() &&
           reparsed.get("list").size() == 2;
}

bool test_as_defaults() {
    aml::Json j = aml::Json::parse(R"({"only":"x"})", nullptr);
    return j.get("missing").as_str("fallback") == "fallback" &&
           j.get("missing").as_int(-7) == -7 && j.get("missing").as_bool(true) == true &&
           j.get("only").as_str() == "x";
}

bool test_unicode_escapes() {
    std::string err;
    aml::Json parsed = aml::Json::parse(
        R"({"text":"\u4e2d\u6587 \u2014 \ud83c\udf0d"})", &err);
    const std::string expected = "\xE4\xB8\xAD\xE6\x96\x87 \xE2\x80\x94 \xF0\x9F\x8C\x8D";
    return err.empty() && parsed.get("text").as_str() == expected;
}

bool test_unpaired_surrogate_rejected() {
    std::string err;
    aml::Json::parse(R"({"text":"\ud83c"})", &err);
    return err == "unpaired high surrogate";
}

bool test_malformed_literals_and_numbers_rejected() {
    for (const char* text : {"tru", "falsex", "nul", "01", "1.", "1e+"}) {
        std::string err;
        aml::Json::parse(text, &err);
        if (err.empty()) return false;
    }
    std::string nested(140, '[');
    nested.append(140, ']');
    std::string err;
    aml::Json::parse(nested, &err);
    return err == "JSON nesting limit exceeded";
}

}  // namespace

int main() {
    struct { const char* name; bool (*fn)(); } tests[] = {
        {"parse_basic", test_parse_basic},         {"parse_nested", test_parse_nested},
        {"parse_errors", test_parse_errors},       {"roundtrip", test_build_dump_roundtrip},
        {"as_defaults", test_as_defaults},         {"reused_error_output", test_reused_error_output},
        {"unicode_escapes", test_unicode_escapes}, {"unpaired_surrogate", test_unpaired_surrogate_rejected},
        {"malformed_literals", test_malformed_literals_and_numbers_rejected},
    };
    bool ok = true;
    for (const auto& t : tests) {
        if (!t.fn()) {
            std::cerr << "FAILED: " << t.name << "\n";
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
