#include "GameData.h"
#include "FeatureAdapter.h"
#include "PartyAdapter.h"
#include "ResourceChannel.h"

#include "ll/api/Versions.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/PlayerInventory.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/phys/HitResult.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace {
std::atomic<bool> enabled{false};
std::mutex callbackMutex;
ll::event::ListenerPtr tickListener;
ll::event::ListenerPtr exitListener;
ll::mod::NativeMod* mod = nullptr;
bool warned = false;
MyClientResources::Channel resourceChannel;

void PublishResources(const MyClientResources::Counts& counts, bool known)
{
    // at == 0 tells the HUD there is no trustworthy data (it shows ---).
    resourceChannel.Publish(GetCurrentProcessId(), counts, known ? GetTickCount64() : 0);
}

void PublishEmpty()
{
    PublishResources({}, false);
    MyClientGame::ClearArmor();
    MyClientGame::ClearParty();
    const auto frame = MyClientGame::EmptyFrame();
    MyClientBridgePublish(&frame);
}

void AddStack(MyClientGame::ArrowCounter& counter, MyClientResources::Counter& resources, ItemStack const& stack)
{
    const uint32_t count = stack.mCount;
    if (!count) return;
    const auto name = stack.getTypeName();
    counter.Add(name, count);
    resources.Add(name, count);
}

void Sample(ll::event::ClientLevelTickEvent& event)
{
    std::lock_guard lock(callbackMutex);
    if (!enabled.load()) return;
    auto frame = MyClientGame::EmptyFrame();
    try
    {
        auto client = ll::service::getClientInstance();
        auto level = ll::service::getMultiPlayerLevel();
        // Never read the integrated server's player, another world or a cached
        // player from before a world transition. Resolve everything per tick.
        if (!client || !level || &event.level() != &*level)
        {
            PublishEmpty();
            return;
        }
        auto* player = client->getLocalPlayer();
        if (!player || !player->mInventory || !player->mInventory->mInventory)
        {
            PublishEmpty();
            return;
        }

        MyClientGame::TickParty();
        MyClientGame::TickAutoRq();
        MyClientGame::SampleArmor();
        auto& inventory = player->getInventory();
        const int slots = inventory.getContainerSize();
        // Bound traversal even if the layout or container is unexpected.
        if (slots > 0 && slots <= 256)
        {
            MyClientGame::ArrowCounter counter;
            MyClientResources::Counter resources;
            for (int slot = 0; slot < slots; ++slot)
                AddStack(counter, resources, inventory.getItem(slot));
#ifndef MYCLIENT_NO_OFFHAND_API  // older LeviLamina builds do not export Actor::getOffhandSlot
            AddStack(counter, resources, player->getOffhandSlot());
#endif
            counter.Write(frame);
            PublishResources(resources.Result(), resources.Valid());
        }
        else
        {
            PublishResources({}, false);
        }

        const auto& hit = level->getHitResult();
        if (hit.mType == HitResultType::Tile)
            MyClientGame::SetTarget(frame, hit.mBlock.x, hit.mBlock.y, hit.mBlock.z);

        if (!MyClientBridgePublish(&frame) && !warned)
        {
            warned = true;
            mod->getLogger().warn("HUD publication failed (Windows error {}).", GetLastError());
        }
    }
    catch (...)
    {
        PublishEmpty();
        if (!warned)
        {
            warned = true;
            mod->getLogger().error("Game data read failed; the HUD snapshot was cleared.");
        }
    }
}

void OnExit(ll::event::ClientExitLevelEvent&)
{
    std::lock_guard lock(callbackMutex);
    if (enabled.load()) PublishEmpty();
}

void Disable()
{
    enabled.store(false);
    MyClientGame::DisableFeatures();
    MyClientGame::DisableParty();
    auto& bus = ll::event::EventBus::getInstance();
    if (tickListener) bus.removeListener(tickListener);
    if (exitListener) bus.removeListener(exitListener);
    tickListener.reset();
    exitListener.reset();
    // Removal does not cancel callbacks already copied by EventBus. They see
    // enabled=false. Wait for a running sample before closing shared memory.
    std::lock_guard lock(callbackMutex);
    MyClientBridgeShutdown();
    resourceChannel.Close();
}
}

#ifndef MYCLIENT_GAME_VERSION
#define MYCLIENT_GAME_VERSION "1.26.51"
#endif
#ifndef MYCLIENT_LOADER_VERSION
#define MYCLIENT_LOADER_VERSION "26.51.6"
#endif

extern "C" __declspec(dllexport) bool ll_mod_load(ll::mod::NativeMod& self)
{
    mod = &self;
    const auto game = ll::getGameVersion();
    const auto loader = ll::getLoaderVersion();
    // Expected versions come from tools/sdks/<game>.lock.json at build time (default: 26.51).
    unsigned gm = 0, gn = 0, gp = 0, lm = 0, ln = 0, lp = 0;
    const bool parsed = std::sscanf(MYCLIENT_GAME_VERSION, "%u.%u.%u", &gm, &gn, &gp) == 3 &&
                        std::sscanf(MYCLIENT_LOADER_VERSION, "%u.%u.%u", &lm, &ln, &lp) == 3;
    if (!ll::isClient() || !parsed ||
        game.major != gm || game.minor != gn || game.patch != gp || game.preRelease ||
        loader.major != lm || loader.minor != ln || loader.patch != lp || loader.preRelease)
    {
        self.getLogger().error(
            "Requires Minecraft {} and LeviLamina Client {}; found game {}, loader {}.",
            MYCLIENT_GAME_VERSION, MYCLIENT_LOADER_VERSION, game.to_string(), loader.to_string());
        return false;
    }
    self.getLogger().info("MyClientData loaded: arrows, target coordinates, render FPS, armor, hold-to-zoom, full bright and Hive party acceptance (live test pending).");
    return true;
}

extern "C" __declspec(dllexport) bool ll_mod_enable(ll::mod::NativeMod& self)
{
    if (enabled.load()) return true;
    if (!MyClientBridgeInitialize())
    {
        self.getLogger().error("Cannot create HUD channel (Windows error {}).", GetLastError());
        return false;
    }
    warned = false;
    if (!resourceChannel.Open(GetCurrentProcessId(), true))
        self.getLogger().warn("Resource counter unavailable: shared memory could not be created (Windows error {}).", GetLastError());
    try
    {
        if (!MyClientGame::EnableFeatures())
            self.getLogger().error("Zoom/FPS/armor unavailable: game symbol, hook or feature channel initialization failed. Arrow/target sampling remains enabled.");
        if (!MyClientGame::EnableParty())
            self.getLogger().error("Hive party unavailable: receive hook or party channel initialization failed.");
        auto& bus = ll::event::EventBus::getInstance();
        tickListener = bus.emplaceListener<ll::event::ClientLevelTickEvent>(Sample);
        exitListener = bus.emplaceListener<ll::event::ClientExitLevelEvent>(OnExit);
    }
    catch (...)
    {
        Disable();
        self.getLogger().error("Client event registration failed; rolled back subscriptions.");
        return false;
    }
    if (!tickListener || !exitListener)
    {
        Disable();
        self.getLogger().error("Cannot subscribe to client world events.");
        return false;
    }
    enabled.store(true);
    self.getLogger().info("MyClientData enabled; waiting for a world.");
    return true;
}

extern "C" __declspec(dllexport) bool ll_mod_disable(ll::mod::NativeMod&)
{
    Disable();
    return true;
}

// No ll_mod_unload: EventBus snapshots may still hold callbacks after removal.
// Disable is supported; unload/replacement requires restarting Minecraft.
