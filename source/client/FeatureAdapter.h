#pragma once
#include <cstdint>
namespace MyClientGame {
bool EnableFeatures();
void DisableFeatures();
uint32_t AutoRqMode();  // Auto RQ mode requested by the HUD (0 = off or lease expired)
void SampleArmor();
void ClearArmor();
}
