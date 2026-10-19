#include "MinecraftDataProvider.h"
#include "BridgeChannel.h"
#include "FeatureChannel.h"
#include "PartyChannel.h"
#include "ResourceChannel.h"

namespace MinecraftDataProvider {
namespace {
MyClientIpc::Channel channel;
MyClientFeatures::Channel features;
MyClientParty::Channel party;
MyClientResources::Channel resources;
MyClientResources::Shared resourceSnapshot{};
ULONGLONG resourceOpenAttempt = 0;
MyClientParty::Shared partySnapshot{};
ULONGLONG partyOpenAttempt = 0;
MyClientFeatures::Telemetry featureSnapshot{};
MyClientFeatures::Server serverSnapshot{};
MyClientFeatures::Stats statsSnapshot{}, statsPrevious{}, statsRate{};
ULONGLONG statsPreviousAt = 0;
ULONGLONG featureOpenAttempt = 0;
DWORD processId = 0;
ULONGLONG lastOpenAttempt = 0;
Status status = Status::Disconnected;
bool arrowsKnown = false;
ULONGLONG inWorldAt = 0; // last time the DLL reported a local player (frame had inventory data)
uint32_t arrows = 0;
bool zoomRequested = false;
bool fullbrightRequested = false;
bool nametagRequested = false;
bool outlineRequested = false;
bool texturesRequested = false;
uint32_t autoRqMode = 0;
uint32_t texturesSeq = 0;
uint32_t outlineRgb = 0x50B4FF;
uint32_t zoomKey = 'C';
TargetBlockData target;
BlockOutlineData block;
BlockOutlineScreenData screen;

void ClearSnapshot()
{
    arrowsKnown = false;
    arrows = 0;
    target = {};
    block = {};
    screen = {};
}
}

bool Initialize(HWND window)
{
    Shutdown();
    if (!IsWindow(window)) return false;
    GetWindowThreadProcessId(window, &processId);
    return processId != 0;
}

void Shutdown()
{
    if (features.Data() && features.Lock()) { features.Data()->command = {}; features.Unlock(); }
    features.Close(); featureSnapshot = {}; serverSnapshot = {}; statsSnapshot = {}; statsPrevious = {}; statsRate = {}; statsPreviousAt = 0; featureOpenAttempt = 0;
    resources.Close(); resourceSnapshot = {}; resourceOpenAttempt = 0;
    if (party.Data() && party.Lock()) { party.Data()->config = {}; party.Unlock(); }
    party.Close(); partySnapshot = {}; partyOpenAttempt = 0;
    channel.Close();
    processId = 0;
    lastOpenAttempt = 0;
    status = Status::Disconnected;
    inWorldAt = 0;
    zoomRequested = false;
    fullbrightRequested = false;
    nametagRequested = false;
    outlineRequested = false;
    texturesRequested = false; autoRqMode = 0;
    ClearSnapshot();
}

void Tick(HWND window)
{
    DWORD currentPid = 0;
    if (!IsWindow(window) || !GetWindowThreadProcessId(window, &currentPid))
    {
        Shutdown();
        return;
    }
    if (currentPid != processId && !Initialize(window)) return;

    ClearSnapshot();
    const ULONGLONG now = GetTickCount64();
    featureSnapshot = {};
    serverSnapshot = {};
    statsSnapshot = {}; statsPrevious = {}; statsRate = {}; statsPreviousAt = 0;
    partySnapshot = {};
    resourceSnapshot = {};
    if (!resources.Data() && (!resourceOpenAttempt || now - resourceOpenAttempt >= 500)) {
        resourceOpenAttempt = now; resources.Open(processId, false);
    }
    if (resources.Data() && resources.Lock()) {
        const auto snapshot = *resources.Data();
        resources.Unlock();
        if (MyClientResources::Valid(snapshot, processId)) resourceSnapshot = snapshot;
        else resources.Close();
    }
    if (!party.Data() && (!partyOpenAttempt || now - partyOpenAttempt >= 500)) {
        partyOpenAttempt = now; party.Open(processId, false);
    }
    if (party.Data() && party.Lock()) {
        partySnapshot = *party.Data(); party.Unlock();
        if (!MyClientParty::Valid(partySnapshot, processId)) { party.Close(); partySnapshot = {}; }
    }
    if (!features.Data() && (!featureOpenAttempt || now - featureOpenAttempt >= 500)) {
        featureOpenAttempt = now;
        features.Open(processId, false);
    }
    if (features.Data() && features.Lock()) {
        const auto snapshot = *features.Data();
        features.Unlock();
        if (MyClientFeatures::Valid(snapshot, processId)) {
            featureSnapshot = snapshot.telemetry; serverSnapshot = snapshot.server; statsSnapshot = snapshot.stats;
            // Per-second call counts of the DLL's game hooks (shown in the detail screens for diagnosis).
            if (!statsPreviousAt) { statsPrevious = statsSnapshot; statsPreviousAt = now; }
            else if (now - statsPreviousAt >= 1000) {
                const double seconds = static_cast<double>(now - statsPreviousAt) / 1000.0;
                const auto rate = [&](uint32_t current, uint32_t previous) {
                    return static_cast<uint32_t>(static_cast<double>(static_cast<uint32_t>(current - previous)) / seconds + 0.5);
                };
                statsRate = {statsSnapshot.at,
                             rate(statsSnapshot.lightBuild, statsPrevious.lightBuild), rate(statsSnapshot.lightSplit, statsPrevious.lightSplit),
                             rate(statsSnapshot.lightCreate, statsPrevious.lightCreate), rate(statsSnapshot.nameTag, statsPrevious.nameTag),
                             rate(statsSnapshot.nameTagLocal, statsPrevious.nameTagLocal), rate(statsSnapshot.outline, statsPrevious.outline),
                             rate(statsSnapshot.tessColor, statsPrevious.tessColor), 0, 0, 0};
                statsPrevious = statsSnapshot; statsPreviousAt = now;
            }
        }
        else features.Close();
    }
    if (!channel.Data())
    {
        if (lastOpenAttempt && now - lastOpenAttempt < 500) return;
        lastOpenAttempt = now;
        if (!channel.Open(processId, false))
        {
            status = Status::Disconnected;
            return;
        }
    }

    const DWORD result = channel.Lock(0); // Never block the HUD render loop.
    if (result != WAIT_OBJECT_0)
    {
        if (result == WAIT_ABANDONED) channel.Unlock();
        status = Status::Stale;
        return;
    }
    const MyClientIpc::SharedData snapshot = *channel.Data();
    channel.Unlock();

    if (!MyClientIpc::HasValidHeader(snapshot, processId))
    {
        status = Status::InvalidData;
        channel.Close();
        return;
    }
    if (!snapshot.hasFrame)
    {
        status = Status::WaitingForData;
        return;
    }
    const ULONGLONG snapshotTime = GetTickCount64();
    if (snapshot.publishedAt > snapshotTime ||
        snapshotTime - snapshot.publishedAt > MyClientIpc::StaleAfterMs)
    {
        status = Status::Stale;
        return;
    }
    if (!IsValidFrame(snapshot.frame))
    {
        status = Status::InvalidData;
        return;
    }

    const auto& f = snapshot.frame;
    status = Status::Live;
    arrowsKnown = (f.flags & MYCLIENT_HAS_ARROWS) != 0;
    if (arrowsKnown) inWorldAt = GetTickCount64();
    arrows = f.arrowCount;
    if (f.flags & MYCLIENT_HAS_TARGET)
    {
        target = {true, f.blockX, f.blockY, f.blockZ};
    }
    if (f.flags & MYCLIENT_HAS_BOUNDS)
    {
        block = {true, f.minX, f.minY, f.minZ, f.maxX, f.maxY, f.maxZ};
    }
    if (f.flags & MYCLIENT_HAS_PROJECTION)
    {
        RECT clientRect{}, windowRect{};
        POINT origin{};
        if (!GetClientRect(window, &clientRect) || !GetWindowRect(window, &windowRect) ||
            !ClientToScreen(window, &origin) || clientRect.right <= 0 ||
            clientRect.bottom <= 0 || clientRect.right > 32768 || clientRect.bottom > 32768)
            return;
        screen.valid = true;
        for (int i = 0; i < 8; ++i)
        {
            screen.points[i].x = origin.x - windowRect.left +
                static_cast<LONG>(std::lround(f.points[i].x * clientRect.right));
            screen.points[i].y = origin.y - windowRect.top +
                static_cast<LONG>(std::lround(f.points[i].y * clientRect.bottom));
        }
    }
}

// True only while the DLL is publishing a local player (inside a world/server).
// The DLL publishes an empty frame on the title screen, so the flag drops there.
// A short grace period avoids flicker from a single missed frame.
bool IsInWorld()
{
    return status == Status::Live && inWorldAt &&
           GetTickCount64() - inWorldAt <= 1500;
}

bool GetArrowCount(uint32_t& count)
{
    count = arrows;
    return arrowsKnown;
}
Status GetStatus() { return status; }
const wchar_t* GetStatusText()
{
    switch (status)
    {
    case Status::Disconnected: return L"DLL: not connected";
    case Status::WaitingForData: return L"DLL: waiting for game data";
    case Status::Live: return L"Game data: receiving";
    case Status::Stale: return L"Game data: stale";
    case Status::InvalidData: return L"Game data: incompatible / invalid";
    }
    return L"Game data: unknown";
}
const TargetBlockData& GetTargetBlockData() { return target; }
const BlockOutlineData& GetBlockOutlineData() { return block; }
const BlockOutlineScreenData& GetBlockOutlineScreenData() { return screen; }
void UpdateBlockOutlineData(const BlockOutlineData& data) { block = data; }
void UpdateBlockOutlineScreenData(const BlockOutlineScreenData& data) { screen = data; }
void ClearBlockOutlineData() { block = {}; screen = {}; target = {}; }
bool GetResources(MyClientResources::Counts& counts) {
    counts = {};
    if (!MyClientResources::Fresh(resourceSnapshot.at, GetTickCount64())) return false;
    counts = resourceSnapshot.counts; return true;
}
bool GetFps(float& fps) {
    fps = 0;
    if (!MyClientFeatures::Fresh(featureSnapshot.fpsAt, GetTickCount64()) ||
        !std::isfinite(featureSnapshot.fps) || featureSnapshot.fps <= 0 || featureSnapshot.fps > 10000) return false;
    fps = featureSnapshot.fps; return true;
}
MyClientFeatures::Armor GetArmor(unsigned slot) {
    if (slot >= 4 || !MyClientFeatures::Fresh(featureSnapshot.armorAt, GetTickCount64()) ||
        !MyClientFeatures::ValidArmor(featureSnapshot.armor[slot])) return {};
    return featureSnapshot.armor[slot];
}
bool IsZoomApplied() {
    return MyClientFeatures::Fresh(featureSnapshot.zoomAt, GetTickCount64(), MyClientFeatures::CommandLeaseMs) && featureSnapshot.zoomActive == 1;
}
namespace {
// Zoom, Full Bright and Nametag share one command record, so always write them together.
void WriteCommand() {
    if (!features.Data() || !features.Lock()) return;
    if (MyClientFeatures::Valid(*features.Data(), processId))
        features.Data()->command = {GetTickCount64(), zoomRequested ? 1u : 0u, zoomKey,
                                    fullbrightRequested ? 1u : 0u, nametagRequested ? 1u : 0u,
                                    outlineRequested ? 1u : 0u, outlineRgb,
                                    texturesSeq, texturesRequested ? 1u : 0u, autoRqMode, 0};
    features.Unlock();
}
}
void SetZoomRequested(bool requested, uint32_t key) {
    zoomRequested = requested; zoomKey = key;
    WriteCommand();
}
bool IsFullbrightApplied() {
    return MyClientFeatures::Fresh(featureSnapshot.fullbrightAt, GetTickCount64(), MyClientFeatures::CommandLeaseMs) &&
           MyClientFeatures::HasFlag(featureSnapshot.fullbrightActive, MyClientFeatures::FlagFullbright);
}
bool IsFullbrightAvailable() { return MyClientFeatures::HasFlag(featureSnapshot.fullbrightAvailable, MyClientFeatures::FlagFullbright); }
// Name tag status rides on the FPS heartbeat (the DLL refreshes it every frame).
bool IsNametagApplied() {
    return MyClientFeatures::Fresh(featureSnapshot.fpsAt, GetTickCount64(), MyClientFeatures::CommandLeaseMs * 4) &&
           MyClientFeatures::HasFlag(featureSnapshot.fullbrightActive, MyClientFeatures::FlagNametag);
}
// Where Ping can be measured: the server the game is connected to (RakNet only).
bool GetPingTarget(std::string& host, int& port) {
    char text[64]; uint32_t number = 0;
    if (!MyClientFeatures::PingTarget(serverSnapshot, GetTickCount64(), text, number)) return false;
    host = text; port = static_cast<int>(number);
    return true;
}
// Calls per second of each hooked game function; false while the DLL's counters are not arriving.
bool GetHookRates(MyClientFeatures::Stats& rates) {
    if (!MyClientFeatures::Fresh(statsSnapshot.at, GetTickCount64(), 3000) || !statsPreviousAt || !statsRate.at) return false;
    rates = statsRate; return true;
}
// What the DLL says it holds for Block Outline (lease state and colour), for the detail screen.
bool GetOutlineEcho(bool& leaseFresh, uint32_t& rgb) {
    if (!MyClientFeatures::Fresh(statsSnapshot.at, GetTickCount64(), 3000)) return false;
    leaseFresh = (statsSnapshot.outlineEcho & 0x01000000u) != 0;
    rgb = statsSnapshot.outlineEcho & 0xFFFFFFu;
    return true;
}
void SetAutoRq(uint32_t mode) { autoRqMode = mode; WriteCommand(); }
// Texture-pack switching: the HUD only reports "on" and a request counter; the DLL does the reload.
void SetTextures(bool on, uint32_t sequence) {
    texturesRequested = on;
    texturesSeq = sequence;
    WriteCommand();
}
bool IsTexturesAvailable() { return MyClientFeatures::HasFlag(featureSnapshot.fullbrightAvailable, MyClientFeatures::FlagTextures); }
bool GetTextureReloads(uint32_t& count) {
    if (!MyClientFeatures::Fresh(statsSnapshot.at, GetTickCount64(), 3000)) return false;
    count = statsSnapshot.texReloads;
    return true;
}
bool IsNametagAvailable() { return MyClientFeatures::HasFlag(featureSnapshot.fullbrightAvailable, MyClientFeatures::FlagNametag); }
bool IsOutlineAvailable() { return MyClientFeatures::HasFlag(featureSnapshot.fullbrightAvailable, MyClientFeatures::FlagOutline); }
bool IsOutlineRunning() {
    return MyClientFeatures::Fresh(featureSnapshot.fpsAt, GetTickCount64(), MyClientFeatures::CommandLeaseMs * 4) &&
           MyClientFeatures::HasFlag(featureSnapshot.fullbrightActive, MyClientFeatures::FlagOutline);
}
bool IsOutlineColored() {
    return MyClientFeatures::Fresh(featureSnapshot.fpsAt, GetTickCount64(), MyClientFeatures::CommandLeaseMs * 4) &&
           MyClientFeatures::HasFlag(featureSnapshot.fullbrightActive, MyClientFeatures::FlagOutlineColored);
}
void SetOutlineRequested(bool requested, uint32_t rgb) {
    outlineRequested = requested;
    outlineRgb = rgb & 0xFFFFFFu;
    WriteCommand();
}
void SetNametagRequested(bool requested) {
    nametagRequested = requested;
    WriteCommand();
}
void SetFullbrightRequested(bool requested) {
    fullbrightRequested = requested;
    WriteCommand();
}
bool IsZoomRequested() { return zoomRequested; }
void SetPartyRequested(bool enabled, const std::vector<std::wstring>& tags) {
    MyClientParty::Config config{};
    config.at = GetTickCount64(); config.enabled = enabled ? 1u : 0u;
    // Reject the whole config on invalid entries or overflow, never silently
    // accept only a prefix of the user's registered list.
    if (tags.size() > MyClientParty::MaxPlayers) config.enabled = 0;
    else for (const auto& wide : tags) {
        std::string tag;
        for (wchar_t c : wide) { if (static_cast<uint32_t>(c) > 127) { config.enabled = 0; break; } tag += static_cast<char>(c); }
        if (!MyClientParty::ValidTag(tag)) { config.enabled = 0; break; }
        std::memcpy(config.tags[config.count++], tag.c_str(), tag.size()+1);
    }
    if (!party.Data() || !party.Lock()) return;
    if (MyClientParty::Valid(*party.Data(), processId)) party.Data()->config = config;
    party.Unlock();
}
const wchar_t* GetPartyStatusText() {
    if (!party.Data() || !MyClientParty::Fresh(partySnapshot.at, GetTickCount64(), 2000)) return L"Hive: waiting for DLL / world";
    switch (partySnapshot.status) {
    case MyClientParty::Status::Waiting: return L"Hive: waiting for world";
    case MyClientParty::Status::Disabled: return L"Hive: off / check registered tags";
    case MyClientParty::Status::OtherServer: return L"Hive: server address not confirmed";
    case MyClientParty::Status::Ready: return L"Hive: waiting for registered invitation";
    case MyClientParty::Status::Sent: return L"Hive: accept command sent (unconfirmed)";
    case MyClientParty::Status::UnknownFormat: return L"Hive: unknown invite format (ignored)";
    case MyClientParty::Status::Failed: return L"Hive: command send failed";
    }
    return L"Hive: invalid status";
}
}
