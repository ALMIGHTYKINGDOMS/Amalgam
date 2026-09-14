#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace aml {

enum ModuleId : uint8_t {
    MOD_FREECAM = 1,
    MOD_FLY = 2,
    MOD_SPEED = 3,
    MOD_NOFALL = 4,
    MOD_AUTOTOOL = 5,
    MOD_KILLAURA = 6,
    MOD_COUNT = 6
};

enum ActionKind : uint8_t {
    ACT_SET_VELOCITY = 1,
    ACT_SET_POS = 2,
    ACT_MOVE_ON_GROUND = 3,
    ACT_ATTACK = 4,
    ACT_SWAP_SLOT = 5,
    ACT_SELECT_SLOT = 6
};

static inline bool action_matches_module(uint8_t module_id, uint8_t kind) {
    switch (kind) {
        case ACT_SET_VELOCITY: return module_id == MOD_FLY || module_id == MOD_SPEED;
        case ACT_SET_POS: return module_id == MOD_FREECAM;
        case ACT_MOVE_ON_GROUND: return module_id == MOD_NOFALL;
        case ACT_ATTACK: return module_id == MOD_KILLAURA;
        case ACT_SWAP_SLOT: return module_id == MOD_AUTOTOOL;
        case ACT_SELECT_SLOT: return module_id == MOD_AUTOTOOL;
        default: return false;
    }
}

static constexpr uint32_t PIPELINE_CAPACITY = 1024;
static constexpr uint32_t ACTION_BYTES = 36;
static constexpr int SNAPSHOT_MAX_HOSTILES = 32;
static constexpr int SNAPSHOT_HEADER_BYTES = 52;
static constexpr uint32_t SNAPSHOT_V2_MAGIC = 0x324C4D41u; // "AML2"
static constexpr int SNAPSHOT_V2_HEADER_BYTES = 60;
static constexpr int SNAPSHOT_MAX_ENTITIES = 64;
static constexpr int SNAPSHOT_V2_ENTITY_BYTES = 96;

struct Action {
    uint32_t seq;
    uint8_t module_id;
    uint8_t kind;
    uint8_t flags;
    uint8_t pad;
    float x;
    float y;
    float z;
    float f;
    int32_t target_id;
    int32_t int_a;
    int32_t int_b;
};

static_assert(sizeof(Action) == ACTION_BYTES, "Action wire layout changed");
static_assert(offsetof(Action, x) == 8, "Action field layout changed");

static inline Action make_action(uint32_t seq, uint8_t module_id, uint8_t kind) {
    Action a{};
    a.seq = seq;
    a.module_id = module_id;
    a.kind = kind;
    return a;
}

struct Hostile {
    int32_t id;
    float dist_sq;
};

struct SnapshotEntity {
    int32_t id;
    int32_t kind;
    float x, y, z;
    float dist_sq;
    float health;
    uint32_t flags;
    char name[32];
    char team[24];
};

struct Snapshot {
    float px, py, pz;
    float pitch, yaw;
    float vx, vy, vz;
    float fall_distance;
    float hurt_time;
    int32_t selected_slot;
    bool on_ground;
    bool breaking;
    int32_t hostile_count;
    Hostile hostiles[SNAPSHOT_MAX_HOSTILES];
    int32_t entity_count = 0;
    SnapshotEntity entities[SNAPSHOT_MAX_ENTITIES]{};
};

static inline void snapshot_parse(Snapshot& out, const uint8_t* data, int len) {
    std::memset(&out, 0, sizeof(out));
    if (len < SNAPSHOT_HEADER_BYTES) return;
    int base = 0;
    bool v2 = false;
    if (len >= 4) {
        uint32_t magic = 0;
        std::memcpy(&magic, data, sizeof(magic));
        v2 = magic == SNAPSHOT_V2_MAGIC;
        base = v2 ? 4 : 0;
        if (v2 && len < SNAPSHOT_V2_HEADER_BYTES) return;
    }
    auto read_float = [data](size_t offset) {
        float value = 0.0f;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    };
    auto read_i32 = [data](size_t offset) {
        int32_t value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    };
    // V2 supersedes the legacy hostile block; hostiles and entities share the
    // region after the header, so never parse both.
    int v2_entity_count = 0;
    if (v2 && len >= SNAPSHOT_V2_HEADER_BYTES + SNAPSHOT_V2_ENTITY_BYTES) {
        v2_entity_count = read_i32(base + 52);
        if (v2_entity_count < 0) v2_entity_count = 0;
    }
    out.px = read_float(base + 0);
    out.py = read_float(base + 4);
    out.pz = read_float(base + 8);
    out.pitch = read_float(base + 12);
    out.yaw = read_float(base + 16);
    out.vx = read_float(base + 20);
    out.vy = read_float(base + 24);
    out.vz = read_float(base + 28);
    out.fall_distance = read_float(base + 32);
    out.hurt_time = read_float(base + 36);
    out.selected_slot = read_i32(base + 40);
    uint32_t flags = static_cast<uint32_t>(read_i32(base + 44));
    out.on_ground = (flags & 1u) != 0;
    out.breaking = (flags & 2u) != 0;
    out.hostile_count = read_i32(base + 48);
    if (out.hostile_count < 0) out.hostile_count = 0;
    if (out.hostile_count > SNAPSHOT_MAX_HOSTILES) out.hostile_count = SNAPSHOT_MAX_HOSTILES;
    if (v2_entity_count > 0) out.hostile_count = 0;
    int avail = (len - (v2 ? SNAPSHOT_V2_HEADER_BYTES : SNAPSHOT_HEADER_BYTES)) / 8;
    if (out.hostile_count > avail) out.hostile_count = avail;
    const uint8_t* p = data + (v2 ? SNAPSHOT_V2_HEADER_BYTES : SNAPSHOT_HEADER_BYTES);
    for (int k = 0; k < out.hostile_count; ++k) {
        std::memcpy(&out.hostiles[k].id, p + k * 8, 4);
        std::memcpy(&out.hostiles[k].dist_sq, p + k * 8 + 4, 4);
    }
    if (v2) {
        int entity_count = read_i32(base + 52);
        int available = (len - SNAPSHOT_V2_HEADER_BYTES) / SNAPSHOT_V2_ENTITY_BYTES;
        out.entity_count = entity_count;
        if (out.entity_count < 0) out.entity_count = 0;
        if (out.entity_count > SNAPSHOT_MAX_ENTITIES) out.entity_count = SNAPSHOT_MAX_ENTITIES;
        if (out.entity_count > available) out.entity_count = available;
        for (int k = 0; k < out.entity_count; ++k) {
            const uint8_t* e = data + SNAPSHOT_V2_HEADER_BYTES + k * SNAPSHOT_V2_ENTITY_BYTES;
            std::memcpy(&out.entities[k].id, e, 4);
            std::memcpy(&out.entities[k].kind, e + 4, 4);
            std::memcpy(&out.entities[k].x, e + 8, 4);
            std::memcpy(&out.entities[k].y, e + 12, 4);
            std::memcpy(&out.entities[k].z, e + 16, 4);
            std::memcpy(&out.entities[k].dist_sq, e + 20, 4);
            std::memcpy(&out.entities[k].health, e + 24, 4);
            std::memcpy(&out.entities[k].flags, e + 28, 4);
            std::memcpy(out.entities[k].name, e + 32, sizeof(out.entities[k].name));
            std::memcpy(out.entities[k].team, e + 68, sizeof(out.entities[k].team));
            out.entities[k].name[sizeof(out.entities[k].name) - 1] = '\0';
            out.entities[k].team[sizeof(out.entities[k].team) - 1] = '\0';
        }
    }
}

}
