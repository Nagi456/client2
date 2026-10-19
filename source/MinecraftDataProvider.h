#pragma once

#include "BridgeApi.h"
#include "FeatureData.h"
#include "ResourceChannel.h"
#include "PartyData.h"
#include <string>
#include <vector>

namespace MinecraftDataProvider {
struct BlockOutlineData {
    bool valid = false;
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 1, maxY = 1, maxZ = 1;
};

struct BlockOutlineScreenData {
    bool valid = false;
    POINT points[8]{};
};

struct TargetBlockData {
    bool valid = false;
    int32_t x = 0, y = 0, z = 0;
};

enum class Status { Disconnected, WaitingForData, Live, Stale, InvalidData };

// These functions are called only by the overlay's UI thread.
bool Initialize(HWND minecraftWindow);
void Tick(HWND minecraftWindow);
void Shutdown();
bool GetArrowCount(uint32_t& count);
bool IsInWorld();
Status GetStatus();
const wchar_t* GetStatusText();
const TargetBlockData& GetTargetBlockData();
const BlockOutlineData& GetBlockOutlineData();
const BlockOutlineScreenData& GetBlockOutlineScreenData();
void UpdateBlockOutlineData(const BlockOutlineData& data);
void UpdateBlockOutlineScreenData(const BlockOutlineScreenData& data);
void ClearBlockOutlineData();

bool GetFps(float& fps);
bool GetResources(MyClientResources::Counts& counts);
MyClientFeatures::Armor GetArmor(unsigned slot);
bool IsZoomApplied();
void SetZoomRequested(bool requested, uint32_t key = 'C');
bool IsZoomRequested();
bool IsFullbrightApplied();
bool IsFullbrightAvailable();
void SetFullbrightRequested(bool requested);
bool IsNametagApplied();
bool GetPingTarget(std::string& host, int& port);
bool GetHookRates(MyClientFeatures::Stats& rates);
bool GetOutlineEcho(bool& leaseFresh, unsigned int& rgb);
void SetTextures(bool on, unsigned int sequence);
void SetAutoRq(unsigned int mode);  // 0 = off, 1..10 = AutoRq.h mode
bool IsTexturesAvailable();
bool GetTextureReloads(unsigned int& count);
bool IsNametagAvailable();
void SetNametagRequested(bool requested);
bool IsOutlineAvailable();
bool IsOutlineRunning();
bool IsOutlineColored();
void SetOutlineRequested(bool requested, unsigned int rgb);
void SetPartyRequested(bool enabled, const std::vector<std::wstring>& tags);
const wchar_t* GetPartyStatusText();
}
