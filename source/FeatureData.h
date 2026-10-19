#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <type_traits>

// Optional extension to bridge ABI 2. Older HUD/DLL pairs continue exchanging
// arrow/target data; feature commands and snapshots use a separate v1 mapping.
namespace MyClientFeatures {
constexpr uint32_t Magic = 0x4d434631;
constexpr uint32_t Version = 6;
constexpr uint64_t StaleMs = 2000, CommandLeaseMs = 250;
enum ArmorState : uint32_t { Unknown, Empty, NoDurability, Durable };
struct Armor { uint32_t state, remaining, maximum; };
// `nametag` used to be the unused `reserved` field: same size, so older DLLs just ignore it.
// outline / outlineRgb: Block Outline colour override (outlineRgb = 0xRRGGBB). Added with ABI version 3.
// texReload: a counter the HUD bumps each time it has rewritten the active texture-pack list; the DLL
// makes the game re-read the list when it changes (and only while texturesOn == 1). Added with version 5.
// autoRq: Auto RQ mode (0 = off, 1..10 = AutoRq.h kModes). Added with version 6.
struct Command { uint64_t at; uint32_t zoom, key, fullbright, nametag, outline, outlineRgb, texReload, texturesOn, autoRq, reserved; };
// fullbrightAvailable / fullbrightActive carry one bit per light/name feature so the layout
// (and the ABI version) stay the same; an older DLL only ever sets bit 0.
constexpr uint32_t FlagFullbright = 1, FlagNametag = 2, FlagOutline = 4, FlagOutlineColored = 8, FlagTextures = 16;
// Available: bit FlagOutline = the outline hook is installed.
// Active:    FlagOutline = the game ran the outline code recently, FlagOutlineColored = a colour call was overridden.
inline bool HasFlag(uint32_t value, uint32_t flag) { return (value & flag) != 0; }
inline uint32_t WithFlag(uint32_t value, uint32_t flag, bool on) { return on ? (value | flag) : (value & ~flag); }
struct Telemetry {
    uint64_t fpsAt, armorAt, zoomAt, fullbrightAt;
    float fps;
    uint32_t zoomActive;
    uint32_t fullbrightActive;    // bit 1 (FlagFullbright): game is using the boosted value; bit 2 (FlagNametag): own name tag forced.
    uint32_t fullbrightAvailable; // bit 1 / bit 2: that game hook was resolved and installed.
    Armor armor[4]; // Head, torso, legs, feet, in SDK slot order.
};
// Connected server, published by the DLL about every 2 s so the HUD can measure Ping itself.
// host is the resolved IP when the game knows it (otherwise the typed address), NUL-terminated.
// transport: 0 = RakNet (UDP, can be pinged), anything else is not pingable. Added with ABI version 4.
struct Server { uint64_t at; uint32_t port, transport; char host[64]; };
// How often the game ran each hooked function (cumulative since the DLL started). The HUD shows the
// per-second difference in the detail screens, so "the game never calls this" is visible on screen.
// outlineEcho: what the DLL currently holds for Block Outline: 0x01000000 = lease is fresh, low 24 bits = RGB.
struct Stats { uint64_t at; uint32_t lightBuild, lightSplit, lightCreate, nameTag, nameTagLocal, outline, tessColor, outlineEcho, texReloads, reserved; };
struct Shared {
    uint32_t magic, version, size, pid;
    Command command;
    Telemetry telemetry;
    Server server;
    Stats stats;
};
static_assert(sizeof(Armor) == 12 && sizeof(Command) == 48 && sizeof(Telemetry) == 96 &&
              sizeof(Server) == 80 && sizeof(Stats) == 48 && sizeof(Shared) == 288);
static_assert(std::is_trivially_copyable<Shared>::value);
inline bool Fresh(uint64_t at, uint64_t now, uint64_t timeout = StaleMs) {
    return at != 0 && at <= now && now - at <= timeout;
}
inline bool Valid(const Shared& data, uint32_t pid) {
    return data.magic == Magic && data.version == Version && data.size == sizeof(Shared) && data.pid == pid;
}
inline bool ValidHostChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == ':';
}
// DLL side: copies at most 63 characters; anything else in `host` is ignored by the HUD.
inline void SetServer(Server& server, const char* host, uint32_t port, uint32_t transport, uint64_t now) {
    server = {};
    server.at = now; server.port = port; server.transport = transport;
    for (size_t i = 0; host && host[i] && i < sizeof(server.host) - 1; ++i) server.host[i] = host[i];
}
// HUD side: the pingable endpoint, or false (stale, no server, not RakNet, odd characters).
inline bool PingTarget(const Server& server, uint64_t now, char (&host)[64], uint32_t& port) {
    if (!Fresh(server.at, now, 6000) || server.transport != 0 || server.port == 0 || server.port > 65535) return false;
    size_t length = 0;
    while (length < sizeof(server.host) && server.host[length]) ++length;
    if (length == 0 || length >= sizeof(server.host)) return false;
    for (size_t i = 0; i < length; ++i) if (!ValidHostChar(server.host[i])) return false;
    for (size_t i = 0; i < length; ++i) host[i] = server.host[i];
    host[length] = '\0'; port = server.port; return true;
}
inline bool ValidArmor(const Armor& armor) {
    return armor.state <= Durable && (armor.state == Durable
        ? armor.maximum > 0 && armor.maximum <= 65535 && armor.remaining <= armor.maximum
        : armor.remaining == 0 && armor.maximum == 0);
}
inline Armor MakeArmor(bool occupied, int maximum, int damage) {
    if (!occupied) return {Empty, 0, 0};
    if (maximum < 0 || damage < 0 || maximum > 65535 || damage > maximum) return {};
    if (maximum == 0) return {NoDurability, 0, 0};
    return {Durable, static_cast<uint32_t>(maximum - damage), static_cast<uint32_t>(maximum)};
}
inline bool WantsZoom(const Command& command, uint64_t now, bool focused, bool playable, bool keyDown) {
    return command.zoom == 1 && command.key >= 1 && command.key <= 254 && command.key != 27 &&
        command.key != 'G' && Fresh(command.at, now, CommandLeaseMs) && focused && playable && keyDown;
}
inline bool WantsFullbright(const Command& command, uint64_t now) {
    // A lease, like Zoom: if the HUD stops refreshing, the game returns to normal.
    return command.fullbright == 1 && Fresh(command.at, now, CommandLeaseMs);
}
// True once per HUD request. The first live command only records the counter (so attaching to a game
// never triggers a reload); after that every change of texReload is one request.
inline bool TextureReloadRequested(const Command& command, uint64_t now, uint32_t& seen, bool& primed) {
    if (command.texturesOn != 1 || !Fresh(command.at, now, CommandLeaseMs)) return false;
    if (!primed) { primed = true; seen = command.texReload; return false; }
    if (command.texReload == seen) return false;
    seen = command.texReload;
    return true;
}
inline bool WantsOutline(const Command& command, uint64_t now) {
    return command.outline == 1 && Fresh(command.at, now, CommandLeaseMs);
}
inline bool WantsNametag(const Command& command, uint64_t now) {
    // Same lease as Full Bright: if the HUD stops refreshing, the game returns to normal.
    return command.nametag == 1 && Fresh(command.at, now, CommandLeaseMs);
}
inline float FullbrightScale(float original, bool active) {
    // Raise the game's own computed value only; never lower it, never write settings.
    if (!active) return original;
    if (!std::isfinite(original)) return 1.f;
    return std::max(original, 1.f);
}
inline float ZoomFov(float original, bool active) {
    // Operate on the computed value, preserving settings and other FOV effects.
    return active && std::isfinite(original) && original > 1.f && original < 180.f
        ? std::max(1.f, static_cast<float>(std::atan(std::tan(original * 3.141592653589793 / 360.0) / 3.0) * 360.0 / 3.141592653589793)) : original;
}
class FrameRate {
    uint64_t start_ = 0, last_ = 0, samples_ = 0, measuredAt_ = 0;
    float value_ = 0;
public:
    void Reset() { start_ = last_ = samples_ = measuredAt_ = 0; value_ = 0; }
    void Frame(uint64_t now) {
        if (!last_ || now < last_ || now - last_ > StaleMs) { Reset(); start_ = last_ = now; return; }
        last_ = now; ++samples_;
        if (now - start_ >= 1000) {
            value_ = static_cast<float>(samples_ * 1000.0 / (now - start_));
            measuredAt_ = now; start_ = now; samples_ = 0;
        }
    }
    bool Read(uint64_t now, float& value, uint64_t& at) const {
        if (!Fresh(measuredAt_, now) || !Fresh(last_, now) || !std::isfinite(value_) || value_ <= 0 || value_ > 10000) return false;
        value = value_; at = measuredAt_; return true;
    }
};
}
