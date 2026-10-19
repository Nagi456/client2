#include "FeatureAdapter.h"
#include "FeatureChannel.h"
#include "PartyAdapter.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/GameRenderer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/ptexture/BaseLightData.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/renderer/ShaderColor.h"
#include "mc/client/renderer/ptexture/BaseLightTextureImageBuilder.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/Item.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace {
std::atomic<bool> active{false};
std::mutex featureMutex;
MyClientFeatures::Channel channel;
MyClientFeatures::FrameRate frames;
bool renderHooked = false, fovHooked = false, brightHooked = false, nametagHooked = false;
bool outlineHooked = false, tessColorHooked = false;
// Texture packs: MinecraftGame::_loadLastGlobalResourcePacksFromFile(bool) re-reads
// global_resource_packs.json and reloads the textures. The HUD rewrites that file, then bumps a counter.
using TextureReloadFn = void (*)(void* game, bool waitForUiReload);
TextureReloadFn textureReload = nullptr;
std::atomic<bool> textureReloadPending{false};
std::atomic<uint32_t> autoRqModeValue{0};          // 0 = off; refreshed once per frame from the HUD's command lease
std::atomic<uint32_t> statTextureReloads{0};
uint32_t textureSeen = 0;
bool texturePrimed = false;
std::atomic<bool> outlineWanted{false};             // refreshed once per frame from the HUD's command lease
std::atomic<uint32_t> outlineRgbValue{0x50B4FF};    // 0xRRGGBB chosen in the HUD
std::atomic<ULONGLONG> outlineRanAt{0}, outlineColoredAt{0};
thread_local bool insideOutline = false;
// Diagnostics: how often the game ran each hooked function (shown by the HUD).
std::atomic<uint32_t> statLightBuild{0}, statLightSplit{0}, statLightCreate{0}, statNameTag{0}, statNameTagLocal{0},
                      statOutline{0}, statTessColor{0};            // true only while the game runs its own outline code
std::atomic<bool> nametagWanted{false};            // refreshed once per frame from the HUD's command lease
std::atomic<ULONGLONG> nametagAppliedAt{0};        // last time the hook really answered for the local player

// Full Bright: the light-map builders read BaseLightData::mNightvisionActive/Scale (the game's
// own night-vision state). While the HUD lease is fresh we present night vision to the builder
// for that single call and restore the original values afterwards, so no game state, effect or
// options file is changed. Declarations verified against the pinned SDK headers
// (BaseLightTextureImageBuilder::$buildImage / $buildImageSplit). If a hook cannot be
// installed, only Full Bright is reported UNAVAILABLE; Zoom, FPS and armor keep working.
// Not hooked: TheEndLightTextureImageBuilder overrides buildImage (MCNAPI, no exported symbol),
// so Full Bright does not apply in The End.

// One bit per optional game hook (see FeatureData.h): bit 1 Full Bright, bit 2 Nametag.
uint32_t AvailabilityBits()
{
    return MyClientFeatures::WithFlag(
        MyClientFeatures::WithFlag(0u, MyClientFeatures::FlagFullbright, brightHooked),
        MyClientFeatures::FlagNametag, nametagHooked)
        | (outlineHooked ? MyClientFeatures::FlagOutline : 0u)
        | (textureReload ? MyClientFeatures::FlagTextures : 0u);
}

// Every Reset() wipes telemetry, so re-stamp what must survive it.
void ResetChannel()
{
    channel.Reset(GetCurrentProcessId());
    channel.Data()->telemetry.fullbrightAvailable = AvailabilityBits();
}
using DamageFn = short (*)(ItemStackBase const*);
DamageFn damageValue = nullptr;

void PublishFps()
{
    // Connected server (for the HUD's Ping): looked up at most every 2 s, outside the feature lock.
    static ULONGLONG lastServerLookup = 0;   // render thread only
    std::string serverHost;
    uint32_t serverPort = 0, serverTransport = 1;
    bool serverLookedUp = false;
    const auto lookupNow = GetTickCount64();
    if (active.load() && lookupNow - lastServerLookup >= 2000) {
        lastServerLookup = lookupNow;
        serverLookedUp = true;
        if (!MyClientGame::CurrentServer(serverHost, serverPort, serverTransport)) {
            serverHost.clear(); serverPort = 0; serverTransport = 1;
        }
    }
    std::lock_guard guard(featureMutex);
    if (!active.load()) return;
    const auto now = GetTickCount64();
    frames.Frame(now);
    if (!channel.Data() || !channel.Lock()) return;
    if (!MyClientFeatures::Valid(*channel.Data(), GetCurrentProcessId())) ResetChannel();
    auto& data = channel.Data()->telemetry;
    data.fpsAt = 0; data.fps = 0;
    frames.Read(now, data.fps, data.fpsAt);
    // Name tag: the lease is read here (once per frame) so the per-actor hook stays lock-free.
    const bool wantNametag = MyClientFeatures::WantsNametag(channel.Data()->command, now);
    nametagWanted.store(wantNametag);
    const auto appliedAt = nametagAppliedAt.load();
    const bool appliedNow = wantNametag && appliedAt != 0 && appliedAt <= now && now - appliedAt < 1000;
    data.fullbrightActive = MyClientFeatures::WithFlag(data.fullbrightActive, MyClientFeatures::FlagNametag, appliedNow);
    // Texture packs: one request per change of the HUD's counter (never on the first sight of it).
    if (MyClientFeatures::TextureReloadRequested(channel.Data()->command, now, textureSeen, texturePrimed))
        textureReloadPending.store(true);
    autoRqModeValue.store(MyClientFeatures::Fresh(channel.Data()->command.at, now, MyClientFeatures::CommandLeaseMs * 4) ? channel.Data()->command.autoRq : 0u);
    // Block Outline colour: same once-per-frame lease read, plus two status bits for the HUD.
    const bool wantOutline = MyClientFeatures::WantsOutline(channel.Data()->command, now);
    outlineWanted.store(wantOutline);
    outlineRgbValue.store(channel.Data()->command.outlineRgb & 0xFFFFFFu);
    const auto ranAt = outlineRanAt.load();
    const auto coloredAt = outlineColoredAt.load();
    data.fullbrightActive = MyClientFeatures::WithFlag(data.fullbrightActive, MyClientFeatures::FlagOutline,
        wantOutline && ranAt != 0 && ranAt <= now && now - ranAt < 1000);
    data.fullbrightActive = MyClientFeatures::WithFlag(data.fullbrightActive, MyClientFeatures::FlagOutlineColored,
        wantOutline && coloredAt != 0 && coloredAt <= now && now - coloredAt < 1000);
    channel.Data()->stats = MyClientFeatures::Stats{
        now,
        statLightBuild.load(std::memory_order_relaxed), statLightSplit.load(std::memory_order_relaxed),
        statLightCreate.load(std::memory_order_relaxed), statNameTag.load(std::memory_order_relaxed),
        statNameTagLocal.load(std::memory_order_relaxed), statOutline.load(std::memory_order_relaxed),
        statTessColor.load(std::memory_order_relaxed),
        (wantOutline ? 0x01000000u : 0u) | (outlineRgbValue.load(std::memory_order_relaxed) & 0xFFFFFFu),
        statTextureReloads.load(std::memory_order_relaxed), 0};
    if (serverLookedUp)
        MyClientFeatures::SetServer(channel.Data()->server, serverHost.c_str(), serverPort, serverTransport, now);
    channel.Unlock();
}

// Runs after a frame has been rendered, on the game's render/main thread. IMinecraftGame is the first
// base of MinecraftGame, so the interface reference is also the MinecraftGame address.
void RunPendingTextureReload()
{
    if (!textureReloadPending.exchange(false) || !textureReload) return;
    try {
        const auto client = ll::service::getClientInstance();
        if (!client) return;
        void* game = &client->getMinecraftGame_DEPRECATED();
        textureReload(game, false);
        const auto count = statTextureReloads.fetch_add(1) + 1;
        ll::mod::NativeMod::current()->getLogger().info("Texture packs were reloaded (request #{}).", count);
    } catch (...) {}
}

LL_TYPE_INSTANCE_HOOK(FrameHook, ll::memory::HookPriority::Normal, GameRenderer,
                      ll::memory::SymbolView{"?renderCurrentFrame@GameRenderer@@QEAAXM@Z"}, void, float alpha)
{
    origin(alpha);
    if (active.load()) { PublishFps(); RunPendingTextureReload(); }
}

LL_TYPE_INSTANCE_HOOK(FovHook, ll::memory::HookPriority::Normal, LevelRendererPlayer,
                      ll::memory::SymbolView{"?getFov@LevelRendererPlayer@@QEBAMM_N@Z"}, float, float alpha, bool variable)
{
    const float original = origin(alpha, variable);
    if (!active.load()) return original;
    bool playable = false;
    try {
        const auto client = ll::service::getClientInstance();
        // Never affect a secondary/split-screen camera or an inventory/chat menu.
        playable = client && &mClientInstance == &*client && client->getLocalPlayer() &&
            client->isInWorldAndNotShowingAnyMenuScreens();
    } catch (...) { playable = false; }
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    std::lock_guard guard(featureMutex);
    if (!active.load() || !channel.Data() || !channel.Lock()) return original;
    if (!MyClientFeatures::Valid(*channel.Data(), GetCurrentProcessId())) ResetChannel();
    const auto command = channel.Data()->command;
    const auto now = GetTickCount64();
    const bool keyDown = command.key > 0 && command.key < 255 && (GetAsyncKeyState(static_cast<int>(command.key)) & 0x8000);
    const bool zoom = MyClientFeatures::WantsZoom(command, now, foregroundPid == GetCurrentProcessId(), playable, keyDown);
    const float result = MyClientFeatures::ZoomFov(original, zoom);
    channel.Data()->telemetry.zoomAt = now;
    channel.Data()->telemetry.zoomActive = result != original ? 1u : 0u;
    channel.Unlock();
    return result;
}

// ---- Block Outline colour -------------------------------------------------------------------
// The game draws the black block selection outline in LevelRendererPlayer::_renderOutlineSelection.
// While the HUD lease is fresh we (1) mark "inside the outline code" for this thread, (2) set the
// game's global ShaderColor to the chosen colour, and (3) replace the RGB of Tessellator::color()
// calls made meanwhile. Alpha is left as the game chose it. Everything is restored afterwards.
// None of this is confirmed against the game binary: the access level of the member changes its
// mangled name (three variants are tried), and the game may colour the outline some other way.
bool OverrideOutlineColor(float& r, float& g, float& b)
{
    if (!insideOutline) return false;
    statTessColor.fetch_add(1, std::memory_order_relaxed);
    const uint32_t rgb = outlineRgbValue.load(std::memory_order_relaxed);
    r = static_cast<float>((rgb >> 16) & 0xFFu) / 255.f;
    g = static_cast<float>((rgb >> 8) & 0xFFu) / 255.f;
    b = static_cast<float>(rgb & 0xFFu) / 255.f;
    outlineColoredAt.store(GetTickCount64(), std::memory_order_relaxed);
    return true;
}

struct OutlineScope {
    ShaderColor* shader = nullptr;
    mce::Color savedColor{};
    bool savedDirty = false;
    bool wasInside = false;
    OutlineScope() {
        outlineRanAt.store(GetTickCount64(), std::memory_order_relaxed);
        wasInside = insideOutline;
        insideOutline = true;
        try {
            const auto client = ll::service::getClientInstance();
            if (client) {
                shader = &client->getShaderColor();
                savedColor = shader->color.get();
                savedDirty = shader->dirty;
                float r = 0, g = 0, b = 0;
                OverrideOutlineColor(r, g, b);
                auto& c = shader->color.get();
                c.r = r; c.g = g; c.b = b;          // alpha stays as it was
                shader->dirty = true;
            }
        } catch (...) { shader = nullptr; }
    }
    ~OutlineScope() {
        insideOutline = wasInside;
        try {
            if (shader) {
                shader->color.get() = savedColor;
                shader->dirty = true;               // make the engine re-upload the original colour
            }
        } catch (...) {}
    }
};

template <class Call>
void RunOutline(Call&& call)
{
    statOutline.fetch_add(1, std::memory_order_relaxed);
    if (!outlineWanted.load(std::memory_order_relaxed)) { call(); return; }
    OutlineScope scope;
    call();
}

#define MYCLIENT_OUTLINE_SYMBOL(ACCESS) \
    "?_renderOutlineSelection@LevelRendererPlayer@@" ACCESS "EBAXAEAVBaseActorRenderContext@@AEBVBlock@@AEAVBlockSource@@AEBVBlockPos@@@Z"

constexpr const char* OutlineSymbolPublic    = MYCLIENT_OUTLINE_SYMBOL("Q");
constexpr const char* OutlineSymbolPrivate   = MYCLIENT_OUTLINE_SYMBOL("A");
constexpr const char* OutlineSymbolProtected = MYCLIENT_OUTLINE_SYMBOL("I");

LL_TYPE_INSTANCE_HOOK(OutlineHookPublic, ll::memory::HookPriority::Normal, LevelRendererPlayer,
                      ll::memory::SymbolView{OutlineSymbolPublic}, void,
                      BaseActorRenderContext& renderContext, Block const& block, BlockSource& region, BlockPos const& pos)
{
    RunOutline([&] { origin(renderContext, block, region, pos); });
}
LL_TYPE_INSTANCE_HOOK(OutlineHookPrivate, ll::memory::HookPriority::Normal, LevelRendererPlayer,
                      ll::memory::SymbolView{OutlineSymbolPrivate}, void,
                      BaseActorRenderContext& renderContext, Block const& block, BlockSource& region, BlockPos const& pos)
{
    RunOutline([&] { origin(renderContext, block, region, pos); });
}
LL_TYPE_INSTANCE_HOOK(OutlineHookProtected, ll::memory::HookPriority::Normal, LevelRendererPlayer,
                      ll::memory::SymbolView{OutlineSymbolProtected}, void,
                      BaseActorRenderContext& renderContext, Block const& block, BlockSource& region, BlockPos const& pos)
{
    RunOutline([&] { origin(renderContext, block, region, pos); });
}

constexpr const char* TessColorFloatSymbol = "?color@Tessellator@@QEAAXMMMM@Z";
constexpr const char* TessColorObjectSymbol = "?color@Tessellator@@QEAAXAEBVColor@mce@@@Z";

LL_TYPE_INSTANCE_HOOK(TessColorFloatHook, ll::memory::HookPriority::Normal, Tessellator,
                      ll::memory::SymbolView{TessColorFloatSymbol}, void, float r, float g, float b, float a)
{
    OverrideOutlineColor(r, g, b);
    origin(r, g, b, a);
}
LL_TYPE_INSTANCE_HOOK(TessColorObjectHook, ll::memory::HookPriority::Normal, Tessellator,
                      ll::memory::SymbolView{TessColorObjectSymbol}, void, mce::Color const& color)
{
    float r = color.r, g = color.g, b = color.b;
    if (OverrideOutlineColor(r, g, b)) {
        const mce::Color replaced(r, g, b, color.a);
        origin(replaced);
    } else {
        origin(color);
    }
}

// Nametag: Actor::canShowNameTag() (virtual, declared in the SDK; Player does not override it) is
// the game's own "may this actor show a name tag" question. The local player normally gets "no"
// in third-person view. While the HUD lease is fresh we answer "yes" for the local player ONLY,
// leaving every other actor and the original answer untouched. NOT confirmed against the game
// binary: if the symbol is missing the feature is reported UNAVAILABLE, and if the game also
// skips the local player elsewhere, the HUD stays at "WAITING" because the hook is never reached.
constexpr const char* NameTagSymbol = "?canShowNameTag@Actor@@UEBA_NXZ";

LL_TYPE_INSTANCE_HOOK(NameTagHook, ll::memory::HookPriority::Normal, Actor,
                      ll::memory::SymbolView{NameTagSymbol}, bool)
{
    const bool original = origin();
    statNameTag.fetch_add(1, std::memory_order_relaxed);
    if (!active.load(std::memory_order_relaxed)) return original;
    try {
        const auto client = ll::service::getClientInstance();
        const Actor* local = client ? static_cast<const Actor*>(client->getLocalPlayer()) : nullptr;
        if (local && local == static_cast<const Actor*>(this)) {
            // Diagnostics: does the game ever ask about the local player at all?
            statNameTagLocal.fetch_add(1, std::memory_order_relaxed);
            if (!original && nametagWanted.load(std::memory_order_relaxed)) {
                nametagAppliedAt.store(GetTickCount64());
                return true;
            }
        }
    } catch (...) {}
    return original;
}

// Mangled from the SDK declarations (virtual public member, MSVC x64). NOT yet confirmed
// against the game binary: if either name is absent in a game build, that hook fails to
// install and Full Bright is reported UNAVAILABLE for it (nothing else is affected).
constexpr const char* BuildImageSymbol =
    "?buildImage@BaseLightTextureImageBuilder@@UEAA_NAEBVBaseLightData@@PEAUImage@mce@@IMM_N@Z";
constexpr const char* BuildImageSplitSymbol =
    "?buildImageSplit@BaseLightTextureImageBuilder@@UEAA_NAEBVBaseLightData@@PEAUImage@mce@@IM_N@Z";

// Returns true (and fills the saved values) when this call should see night vision.
bool BeginFullbright(BaseLightData& light, bool& savedActive, float& savedScale)
{
    if (!active.load()) return false;
    std::lock_guard guard(featureMutex);
    if (!active.load() || !channel.Data() || !channel.Lock()) return false;
    if (!MyClientFeatures::Valid(*channel.Data(), GetCurrentProcessId())) ResetChannel();
    const auto now = GetTickCount64();
    const bool wanted = MyClientFeatures::WantsFullbright(channel.Data()->command, now);
    channel.Data()->telemetry.fullbrightAt = now;
    channel.Data()->telemetry.fullbrightActive = MyClientFeatures::WithFlag(
        channel.Data()->telemetry.fullbrightActive, MyClientFeatures::FlagFullbright, wanted);
    channel.Unlock();
    if (!wanted) return false;
    savedActive = light.mNightvisionActive;
    savedScale = light.mNightvisionScale;
    light.mNightvisionScale = MyClientFeatures::FullbrightScale(savedScale, true);
    light.mNightvisionActive = true;
    return true;
}

LL_TYPE_INSTANCE_HOOK(LightHook, ll::memory::HookPriority::Normal, BaseLightTextureImageBuilder,
                      ll::memory::SymbolView{BuildImageSymbol}, bool,
                      BaseLightData const& lightData, mce::Image* image, uint length, float a,
                      float ambientBoost, bool clampToMinimum)
{
    statLightBuild.fetch_add(1, std::memory_order_relaxed);
    auto& light = const_cast<BaseLightData&>(lightData);
    bool savedActive = false; float savedScale = 0.f;
    const bool applied = BeginFullbright(light, savedActive, savedScale);
    const bool result = origin(lightData, image, length, a, ambientBoost, clampToMinimum);
    if (applied) { light.mNightvisionActive = savedActive; light.mNightvisionScale = savedScale; }
    return result;
}

LL_TYPE_INSTANCE_HOOK(LightSplitHook, ll::memory::HookPriority::Normal, BaseLightTextureImageBuilder,
                      ll::memory::SymbolView{BuildImageSplitSymbol}, bool,
                      BaseLightData const& lightData, mce::Image* image, uint length, float a,
                      bool clampToMinimum)
{
    statLightSplit.fetch_add(1, std::memory_order_relaxed);
    auto& light = const_cast<BaseLightData&>(lightData);
    bool savedActive = false; float savedScale = 0.f;
    const bool applied = BeginFullbright(light, savedActive, savedScale);
    const bool result = origin(lightData, image, length, a, clampToMinimum);
    if (applied) { light.mNightvisionActive = savedActive; light.mNightvisionScale = savedScale; }
    return result;
}

// The game builds a fresh BaseLightData for the light map and only rebuilds the map when that data
// changed. Boosting the data HERE (and leaving it boosted) makes the game see "night vision" as
// part of the light state, so it rebuilds the light map by itself when the switch is toggled.
// The two buildImage hooks above only change values while a map is being built, which can be rare.
// std::unique_ptr<BaseLightData> is returned by value (hidden return pointer), hence "?AV?$unique_ptr...".
constexpr const char* LightCreateSymbol =
    "?createBaseLightTextureData@BaseLightTextureImageBuilder@@UEAA?AV?$unique_ptr@VBaseLightData@@U?$default_delete@VBaseLightData@@@std@@@std@@PEAVIClientInstance@@AEBVBaseLightData@@@Z";

LL_TYPE_INSTANCE_HOOK(LightCreateHook, ll::memory::HookPriority::Normal, BaseLightTextureImageBuilder,
                      ll::memory::SymbolView{LightCreateSymbol}, std::unique_ptr<BaseLightData>,
                      IClientInstance* client, BaseLightData const& currentData)
{
    statLightCreate.fetch_add(1, std::memory_order_relaxed);
    auto created = origin(client, currentData);
    if (created) {
        bool savedActive = false; float savedScale = 0.f;
        BeginFullbright(*created, savedActive, savedScale);   // intentionally NOT restored
    }
    return created;
}
}

namespace MyClientGame {
bool EnableFeatures()
{
    // Hooks remain installed until process exit. Disabled detours call origin;
    // the DLL cannot be dynamically unloaded, avoiding in-flight trampolines.
    auto& log = ll::mod::NativeMod::current()->getLogger();
    // Every game hook is installed on its own and its result is written to the game log, so one
    // missing symbol no longer switches every other feature off (and the log says which one failed).
    if (!damageValue) damageValue = reinterpret_cast<DamageFn>(ll::memory::SymbolView{"?getDamageValue@ItemStackBase@@QEBAFXZ"}.resolve());
    if (!damageValue) log.warn("Armor durability unavailable: symbol ?getDamageValue@ItemStackBase@@QEBAFXZ was not found.");
    // Manual registration checks errors; the macro's automatic-use counter is unused here.
    (void)FrameHook::_AutoHookCount; (void)FovHook::_AutoHookCount; (void)LightHook::_AutoHookCount; (void)LightSplitHook::_AutoHookCount; (void)LightCreateHook::_AutoHookCount;
    (void)NameTagHook::_AutoHookCount;
    (void)OutlineHookPublic::_AutoHookCount; (void)OutlineHookPrivate::_AutoHookCount; (void)OutlineHookProtected::_AutoHookCount;
    (void)TessColorFloatHook::_AutoHookCount; (void)TessColorObjectHook::_AutoHookCount;
    if (!renderHooked) {
        renderHooked = FrameHook::hook() == 0;
        if (!renderHooked) log.error("Per-frame hook failed. FPS, the Zoom lease and Nametag need it.");
    }
    if (!fovHooked) {
        fovHooked = FovHook::hook() == 0;
        if (!fovHooked) log.error("Zoom hook failed (LevelRendererPlayer::getFov).");
    }
    // Optional hooks: failure only makes that feature report UNAVAILABLE.
    if (!brightHooked) {
        // Either light-map path is enough to report Full Bright as available.
        const bool a = ll::memory::SymbolView{BuildImageSymbol}.resolve() && LightHook::hook() == 0;
        const bool b = ll::memory::SymbolView{BuildImageSplitSymbol}.resolve() && LightSplitHook::hook() == 0;
        const bool c = ll::memory::SymbolView{LightCreateSymbol}.resolve() && LightCreateHook::hook() == 0;
        if (!c) log.warn("Full Bright: createBaseLightTextureData hook not found; only the buildImage hooks are used.");
        brightHooked = a || b || c;
        if (!brightHooked) log.error("Full Bright hook failed (BaseLightTextureImageBuilder::buildImage / buildImageSplit / createBaseLightTextureData).");
    }
    if (!nametagHooked) {
        nametagHooked = ll::memory::SymbolView{NameTagSymbol}.resolve() && NameTagHook::hook() == 0;
        if (!nametagHooked) log.error("Nametag hook failed (Actor::canShowNameTag).");
    }
    if (!outlineHooked) {
        // The member's access level (public/private/protected) changes its mangled name: try each.
        if (ll::memory::SymbolView{OutlineSymbolPublic}.resolve() && OutlineHookPublic::hook() == 0) outlineHooked = true;
        else if (ll::memory::SymbolView{OutlineSymbolPrivate}.resolve() && OutlineHookPrivate::hook() == 0) outlineHooked = true;
        else if (ll::memory::SymbolView{OutlineSymbolProtected}.resolve() && OutlineHookProtected::hook() == 0) outlineHooked = true;
        if (!outlineHooked) log.error("Block Outline hook failed (LevelRendererPlayer::_renderOutlineSelection): none of the 3 name variants was found.");
    }
    if (!tessColorHooked) {
        const bool a = ll::memory::SymbolView{TessColorFloatSymbol}.resolve() && TessColorFloatHook::hook() == 0;
        const bool b = ll::memory::SymbolView{TessColorObjectSymbol}.resolve() && TessColorObjectHook::hook() == 0;
        tessColorHooked = a || b;
        if (!tessColorHooked) log.warn("Tessellator::color hooks were not found; Block Outline relies on the shader colour only.");
    }
    if (!textureReload) {
        textureReload = reinterpret_cast<TextureReloadFn>(
            ll::memory::SymbolView{"?_loadLastGlobalResourcePacksFromFile@MinecraftGame@@AEAAX_N@Z"}.resolve());
        if (!textureReload) log.warn("Texture pack reload function (MinecraftGame::_loadLastGlobalResourcePacksFromFile) was not found.");
    }
    log.info("Game hooks: frame={} fov={} fullbright={} nametag={} armor={} outline={} tessColor={} textures={}",
             renderHooked, fovHooked, brightHooked, nametagHooked, damageValue != nullptr, outlineHooked, tessColorHooked,
             textureReload != nullptr);
    if (!(renderHooked || fovHooked || brightHooked || nametagHooked || outlineHooked)) return false;
    std::lock_guard guard(featureMutex);
    if (!channel.Open(GetCurrentProcessId(), true)) return false;
    channel.Data()->telemetry.fullbrightAvailable = AvailabilityBits();
    frames.Reset(); active.store(true); return true;
}
void DisableFeatures()
{
    active.store(false);
    nametagWanted.store(false);
    outlineWanted.store(false);
    std::lock_guard guard(featureMutex);
    if (channel.Data() && channel.Lock()) { ResetChannel(); channel.Unlock(); }
    channel.Close(); frames.Reset();
}
void ClearArmor()
{
    std::lock_guard guard(featureMutex);
    if (!active.load() || !channel.Data() || !channel.Lock()) return;
    if (!MyClientFeatures::Valid(*channel.Data(), GetCurrentProcessId())) ResetChannel();
    auto& data = channel.Data()->telemetry;
    data.armorAt = data.zoomAt = data.fullbrightAt = 0; data.zoomActive = data.fullbrightActive = 0;
    nametagWanted.store(false);
    outlineWanted.store(false);
    for (auto& slot : data.armor) slot = {};
    channel.Data()->command = {};
    channel.Data()->server = {};
    channel.Unlock();
}
uint32_t AutoRqMode() { return autoRqModeValue.load(); }
void SampleArmor()
{
    if (!active.load()) return;
    MyClientFeatures::Armor armor[4]{};
    const auto client = ll::service::getClientInstance();
    const auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) { ClearArmor(); return; }
#ifdef MYCLIENT_NO_ARMOR_API  // older LeviLamina builds do not export Actor::getArmor
    ClearArmor();
    return;
#endif
    for (int i = 0; i < 4; ++i) {
        const auto& stack = player->getArmor(static_cast<SharedTypes::Legacy::ArmorSlot>(i));
        if (!stack.mCount) armor[i] = MyClientFeatures::MakeArmor(false, 0, 0);
        else if (stack.mItem && damageValue) armor[i] = MyClientFeatures::MakeArmor(true, stack.mItem->getMaxDamage(), damageValue(&stack));
        else armor[i] = MyClientFeatures::MakeArmor(false, 0, 0);
    }
    std::lock_guard guard(featureMutex);
    if (!active.load() || !channel.Data() || !channel.Lock()) return;
    if (!MyClientFeatures::Valid(*channel.Data(), GetCurrentProcessId())) ResetChannel();
    for (int i = 0; i < 4; ++i) channel.Data()->telemetry.armor[i] = armor[i];
    channel.Data()->telemetry.armorAt = GetTickCount64();
    channel.Unlock();
}
}
