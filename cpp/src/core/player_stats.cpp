#include "core/player_stats.h"
#include <cstring>

namespace aml::player_stats {

static Store g_store;

Store& store() { return g_store; }

const char* effect_name(uint16_t effect_id) {
    switch (effect_id) {
        case 1: return "Speed";
        case 2: return "Slowness";
        case 3: return "Haste";
        case 4: return "Mining Fatigue";
        case 5: return "Strength";
        case 6: return "Instant Health";
        case 7: return "Instant Damage";
        case 8: return "Jump Boost";
        case 9: return "Nausea";
        case 10: return "Regeneration";
        case 11: return "Resistance";
        case 12: return "Fire Resistance";
        case 13: return "Water Breathing";
        case 14: return "Invisibility";
        case 15: return "Blindness";
        case 16: return "Night Vision";
        case 17: return "Hunger";
        case 18: return "Weakness";
        case 19: return "Poison";
        case 20: return "Wither";
        case 21: return "Health Boost";
        case 22: return "Absorption";
        case 23: return "Saturation";
        case 24: return "Glowing";
        case 25: return "Levitation";
        case 26: return "Luck";
        case 27: return "Bad Luck";
        case 28: return "Slow Falling";
        case 29: return "Conduit Power";
        case 30: return "Dolphin's Grace";
        case 31: return "Bad Omen";
        case 32: return "Hero of the Village";
        case 33: return "Darkness";
        case 34: return "Trial Omen";
        case 35: return "Raid Omen";
        case 36: return "Weaving";
        case 37: return "Infested";
        case 38: return "Oozing";
        case 39: return "Wind Charged";
        default: return nullptr;
    }
}

void Store::publish(const State& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    state_.has_data = true;
}

State Store::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void Store::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = {};
}

static uint16_t read_u16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static int16_t read_i16(const uint8_t* p) {
    int16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static float read_f32(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

bool decode_payload(State& out, const uint8_t* data, int len) {
    if (!data || len < 8) return false;

    uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    if (magic != PLY1_MAGIC) return false;

    int version = data[4];
    if (version != PLY1_VERSION) return false;

    uint8_t flags = data[5];
    int offset = 8;

    auto read_u8 = [&](int at) -> uint8_t {
        return (at < len) ? data[at] : 0;
    };

    auto read_bytes = [&](int at, void* dst, int count) {
        if (at + count <= len)
            std::memcpy(dst, data + at, count);
    };

    // Flags: bit0=hasFood, bit1=hasArmor, bit2=hasEffects, bit3=hasPing,
    //        bit4=hasAir, bit5=hasFire, bit6=hasXP, bit7=hasWorld

    if (flags & 0x01) {
        // Food
        if (offset + 9 <= len) {
            out.food_level = read_u8(offset);
            out.saturation = read_f32(data + offset + 1);
            out.exhaustion = read_f32(data + offset + 5);
            offset += 9;
        }
    }

    if (flags & 0x02) {
        // Armor
        if (offset + 5 <= len) {
            out.armor_points = read_u8(offset);
            out.armor_helm_pct = read_u8(offset + 1);
            out.armor_chest_pct = read_u8(offset + 2);
            out.armor_legs_pct = read_u8(offset + 3);
            out.armor_boots_pct = read_u8(offset + 4);
            offset += 5;
        }
    }

    if (flags & 0x04) {
        // Effects
        if (offset + 1 <= len) {
            int count = std::min(static_cast<int>(read_u8(offset)), PLY1_MAX_EFFECTS);
            offset++;
            out.effects.clear();
            for (int i = 0; i < count && offset + 5 <= len; i++) {
                PotionEffect e;
                e.effect_id = read_u16(data + offset);
                e.amplifier = read_u8(offset + 2);
                e.duration_ticks = read_u16(data + offset + 3);
                out.effects.push_back(e);
                offset += 5;
            }
        }
    }

    if (flags & 0x08) {
        // Ping
        if (offset + 4 <= len) {
            std::memcpy(&out.ping_ms, data + offset, 4);
            offset += 4;
        }
    }

    if (flags & 0x10) {
        // Air + Fire
        if (offset + 6 <= len) {
            out.air_supply = read_u16(data + offset);
            out.max_air_supply = read_u16(data + offset + 2);
            out.remaining_fire_ticks = read_i16(data + offset + 4);
            offset += 6;
        }
    }

    if (flags & 0x20) {
        // XP
        if (offset + 8 <= len) {
            out.xp_level = static_cast<int>(read_u16(data + offset));
            out.xp_progress = read_f32(data + offset + 2);
            out.experience = static_cast<int>(read_u16(data + offset + 6));
            offset += 8;
        }
    }

    if (flags & 0x40) {
        // State flags + World
        if (offset + 1 <= len) {
            out.state_flags = read_u8(offset);
            offset++;
        }
        if (offset + 4 <= len) {
            out.day_time = static_cast<int>(read_u16(data + offset));
            out.is_raining = read_u8(offset + 2) != 0;
            out.gamemode = read_u8(offset + 3);
            offset += 4;
        }
        // Biome: length-prefixed string (1 byte length + up to 31 chars)
        if (offset + 1 <= len) {
            int bLen = std::min(static_cast<int>(read_u8(offset)), 31);
            offset++;
            if (offset + bLen <= len) {
                std::memcpy(out.biome, data + offset, bLen);
                out.biome[bLen] = '\0';
                offset += bLen;
            }
        }
        // Dimension: length-prefixed string
        if (offset + 1 <= len) {
            int dLen = std::min(static_cast<int>(read_u8(offset)), 31);
            offset++;
            if (offset + dLen <= len) {
                std::memcpy(out.dimension, data + offset, dLen);
                out.dimension[dLen] = '\0';
                offset += dLen;
            }
        }
    }

    return true;
}

}  // namespace aml::player_stats
