#pragma once
#include <cstdint>
// aurora::device is not exported to mods; the mod implements this with the GC pad motor API.
namespace aurora::device {
void rumble(uint16_t lowFreq, uint16_t highFreq, uint16_t durationMs) noexcept;
}
