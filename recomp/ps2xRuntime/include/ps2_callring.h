#pragma once
#include <atomic>
#include <cstdint>
struct R5900Context;
struct PS2CallRingEntry { const R5900Context *ctx; uint32_t from, to, sp; };
extern PS2CallRingEntry g_ps2xCallRing[4096];
extern std::atomic<uint32_t> g_ps2xCallRingPos;
