#include "core/protocol.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

void put32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

void put_i32(std::vector<uint8_t>& data, size_t offset, int32_t value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

void put_float(std::vector<uint8_t>& data, size_t offset, float value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

bool near(float a, float b) { return std::fabs(a - b) < 0.0001f; }

bool test_v1() {
    std::vector<uint8_t> bytes(52 + 8, 0);
    put_float(bytes, 0, 10.0f);
    put_float(bytes, 4, 20.0f);
    put_i32(bytes, 40, 4);
    put_i32(bytes, 48, 1);
    put_i32(bytes, 52, 42);
    put_float(bytes, 56, 9.0f);
    aml::Snapshot parsed;
    aml::snapshot_parse(parsed, bytes.data(), static_cast<int>(bytes.size()));
    return near(parsed.px, 10.0f) && parsed.selected_slot == 4 && parsed.hostile_count == 1 &&
           parsed.hostiles[0].id == 42 && near(parsed.hostiles[0].dist_sq, 9.0f);
}

bool test_v2() {
    std::vector<uint8_t> bytes(aml::SNAPSHOT_V2_HEADER_BYTES + aml::SNAPSHOT_V2_ENTITY_BYTES, 0);
    put32(bytes, 0, aml::SNAPSHOT_V2_MAGIC);
    put_float(bytes, 4, 1.0f);
    put_float(bytes, 8, 2.0f);
    put_i32(bytes, 52, 1);
    put_i32(bytes, 56, 1);
    size_t entity = aml::SNAPSHOT_V2_HEADER_BYTES;
    put_i32(bytes, entity, 99);
    put_i32(bytes, entity + 4, 2);
    put_float(bytes, entity + 8, 3.0f);
    put_float(bytes, entity + 12, 4.0f);
    put_float(bytes, entity + 16, 5.0f);
    put_float(bytes, entity + 20, 25.0f);
    put_float(bytes, entity + 24, 18.0f);
    put_i32(bytes, entity + 28, 1);
    const char name[] = "Player";
    std::memcpy(bytes.data() + entity + 32, name, sizeof(name));
    aml::Snapshot parsed;
    aml::snapshot_parse(parsed, bytes.data(), static_cast<int>(bytes.size()));
    return parsed.entity_count == 1 && parsed.entities[0].id == 99 &&
           parsed.entities[0].kind == 2 && near(parsed.entities[0].x, 3.0f) &&
           near(parsed.entities[0].health, 18.0f) && std::strcmp(parsed.entities[0].name, "Player") == 0;
}

bool test_negative_counts() {
    std::vector<uint8_t> bytes(aml::SNAPSHOT_V2_HEADER_BYTES, 0);
    put32(bytes, 0, aml::SNAPSHOT_V2_MAGIC);
    put_i32(bytes, 52, -1);
    aml::Snapshot parsed;
    aml::snapshot_parse(parsed, bytes.data(), static_cast<int>(bytes.size()));
    return parsed.hostile_count == 0 && parsed.entity_count == 0;
}

bool test_v2_zero_entity_hostile() {
    std::vector<uint8_t> bytes(aml::SNAPSHOT_V2_HEADER_BYTES + 8, 0);
    put32(bytes, 0, aml::SNAPSHOT_V2_MAGIC);
    put_i32(bytes, 52, 1);
    put_i32(bytes, 56, 0);
    put_i32(bytes, 60, 123);
    put_float(bytes, 64, 16.0f);
    aml::Snapshot parsed;
    aml::snapshot_parse(parsed, bytes.data(), static_cast<int>(bytes.size()));
    return parsed.entity_count == 0 && parsed.hostile_count == 1 &&
           parsed.hostiles[0].id == 123 && near(parsed.hostiles[0].dist_sq, 16.0f);
}

}  // namespace

int main() {
    if (!test_v1() || !test_v2() || !test_negative_counts() || !test_v2_zero_entity_hostile()) {
        std::cerr << "protocol golden test failed\n";
        return 1;
    }
    return 0;
}
