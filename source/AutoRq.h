#pragma once
// Auto RQ: when the local player dies on Hive, queue the next game with "/q <MODE>".
// Pure logic (no game or Windows types) so it can be tested on the host.
#include <cstdint>
#include <cstddef>
#include <string>
namespace MyClientAutoRq {
// Mode numbers are written by the HUD (Command.autoRq); 0 = off.
struct Mode { const char* name; const char* command; };
constexpr Mode kModes[] = {
    {"Sky Wars Solo", "q SKY"}, {"Sky Wars Duos", "q SKY-DUOS"}, {"Sky Wars Squads", "q SKY-SQUADS"}, {"Sky Wars Mega", "q SKY-MEGA"},
    {"Bed Wars Solo", "q BED"}, {"Bed Wars Duos", "q BED-DUOS"}, {"Bed Wars Squads", "q BED-SQUADS"}, {"Bed Wars Mega", "q BED-MEGA"},
    {"Bed Wars Duels 1v1", "q BED-DUELS"}, {"Bed Wars Duels 2v2", "q BED-DUELS-DUOS"},
};
constexpr uint32_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);
// `mode` is 1-based on the wire. Returns nullptr when off / out of range.
inline const Mode* Find(uint32_t mode) { return mode >= 1 && mode <= kModeCount ? &kModes[mode - 1] : nullptr; }
// The command as the game's command packet wants it: with the leading slash.
inline std::string Command(uint32_t mode) { const auto* m = Find(mode); return m ? std::string("/") + m->command : std::string(); }

// Fires once per death. Alive -> dead edge (dead = no longer alive or became a spectator) while the lease is on.
// After firing it stays quiet until the player has been seen alive again (next game), and never fires twice
// within `cooldownMs`. A death seen on the very first sample (e.g. joining as a spectator) is not an edge.
class Gate {
    bool aliveSeen_ = false, fired_ = false;
    uint64_t lastFire_ = 0;
public:
    static constexpr uint64_t kCooldownMs = 8000;
    void Reset() { aliveSeen_ = false; fired_ = false; }
    bool Update(bool enabled, bool dead, uint64_t now) {
        if (!dead) { aliveSeen_ = true; fired_ = false; return false; }
        if (!enabled || !aliveSeen_ || fired_) return false;
        if (lastFire_ != 0 && now - lastFire_ < kCooldownMs) return false;
        fired_ = true; aliveSeen_ = false; lastFire_ = now;
        return true;
    }
};
}
