#pragma once
// ps2x: Anbindung eines echten SPU2-Chips (PCSX2-SPU2 in libps2xgs) an den IOP des Ports.
// Die Runtime traegt die Funktionen ein, sobald die Grafik-/Ton-Bruecke aktiv ist; sonst bleibt alles wie bisher.
#include <cstdint>

namespace ps2x::iop
{
    struct SpuBridge
    {
        void (*write)(unsigned int addr, unsigned short value, unsigned int cycle) = nullptr;
        unsigned short (*read)(unsigned int addr, unsigned int cycle) = nullptr;
        void (*dma)(int core, unsigned short *mem, unsigned int halfwords, int toSpu, unsigned int cycle) = nullptr;
        void (*async)(unsigned int cycle) = nullptr;
        unsigned int (*takeIrqs)() = nullptr;
    };
    extern SpuBridge g_spuBridge;
    extern uint64_t g_spuIopCycle; // aktueller IOP-Takt (vom Emulator nachgefuehrt)
}
