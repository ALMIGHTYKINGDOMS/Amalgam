#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace aml::player_stats {

// PLY1 wire format constants
constexpr uint32_t PLY1_MAGIC = 0x314C5950;  // "PLY1" little-endian
constexpr int PLY1_VERSION = 1;
constexpr int PLY1_MAX_EFFECTS = 16;

struct PotionEffect {
    uint16_t effect_id = 0;
    uint8_t amplifier = 0;
    uint16_t duration_ticks = 0;
};

// Returns a vanilla effect name for registry ids used by the bridge. Custom
// effects return nullptr and are rendered with their numeric registry id.
const char* effect_name(uint16_t effect_id);

struct State {
    // Armor (0-20 total, per-slot durability %)
    int armor_points = 0;
    int armor_helm_pct = 0;
    int armor_chest_pct = 0;
    int armor_legs_pct = 0;
    int armor_boots_pct = 0;

    // Food
    int food_level = 0;
    float saturation = 0.0f;
    float exhaustion = 0.0f;

    // Effects
    std::vector<PotionEffect> effects;

    // Player stats
    int ping_ms = 0;
    int air_supply = 0;
    int max_air_supply = 0;
    int remaining_fire_ticks = 0;
    int xp_level = 0;
    float xp_progress = 0.0f;
    int experience = 0;

    // State flags (bitfield)
    uint8_t state_flags = 0;  // bit0=swimming, bit1=sprinting, bit2=sneaking, bit3=flying, bit4=climbing

    // World
    int day_time = 0;       // ticks (0-24000)
    bool is_raining = false;
    int gamemode = 0;       // 0=survival, 1=creative, 2=adventure, 3=spectator
    char biome[32] = {};    // null-terminated biome name
    char dimension[32] = {}; // null-terminated dimension name

    bool has_data = false;  // true once first PLY1 payload is received
};

class Store {
public:
    void publish(const State& state);
    State snapshot() const;
    void clear();

private:
    mutable std::mutex mutex_;
    State state_{};
};

Store& store();

// Decode a PLY1 binary payload into a State. Returns true on success.
bool decode_payload(State& out, const uint8_t* data, int len);

}  // namespace aml::player_stats
