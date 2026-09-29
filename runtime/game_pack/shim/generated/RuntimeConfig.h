// Published app only: the player's game pack supplies these at startup
// (src/game_pack.cpp). Other builds use the translator's generated header.
#pragma once

#include <cstdint>

namespace RuntimeConfig {
extern uint32_t SDA1_BASE;
extern uint32_t SDA2_BASE;
}
