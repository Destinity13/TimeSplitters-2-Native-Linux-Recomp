#pragma once
// GS in eigenem Thread (Vorbild PCSX2 MTGS): Zeichen- und Transferbefehle laufen über eine Warteschlange in
// einen Arbeits-Thread, der Spiel-Thread rechnet sofort weiter. Alles, was ein Ergebnis zurückgibt oder den
// VRAM-Inhalt sehen muss (Present, Rücklesen, Snapshots), wartet vorher, bis die Warteschlange leer ist.
// Damit bleibt die Reihenfolge exakt wie im synchronen Betrieb; nur die Ausführung ist zeitversetzt.
// Einschalten: PS2X_MTGS=1 (siehe gs_frontend.cpp).

#include "runtime/gs/gs_backend.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

class GSThreadedBackend final : public GSRasterBackend
{
public:
    explicit GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner) : m_inner(std::move(inner))
    {
        m_thread = std::thread([this] { run(); });
    }
    ~GSThreadedBackend() override
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_quit = true;
        }
        m_cvWork.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    void Initialize(uint8_t *vram, uint32_t vramSize) override { drain(0); m_inner->Initialize(vram, vramSize); }
    void Reset() override { drain(1); m_inner->Reset(); }

    void Submit(const GSPrimitiveBatch &batch) override { push(Cmd{batch}); }
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override { push(Cmd{ClutCmd{tex0, texclut}}); }
    void BeginTransfer(const GSTransferCommand &command) override { push(Cmd{command}); }
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
    {
        UploadCmd u;
        u.data.assign(data, data + sizeBytes);
        push(Cmd{std::move(u)});
    }
    void Flush() override { push(Cmd{FlushCmd{}}); }
    void TextureFlush() override { push(Cmd{TexFlushCmd{}}); }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
    {
        push(Cmd{WriteCmd{psm, base, bw, x, y, value}});
    }

    void Sync(GSSyncReason reason) override
    {
        // FINISH (pro Paket) braucht kein Warten: der Interrupt kommt vom Frontend, und alles, was danach VRAM liest
        // (Present, Rücklesen, Lokal->Wirt), wartet selbst. So läuft der Rasterizer wirklich parallel (wie PCSX2 MTGS).
        if (reason == GSSyncReason::Finish || reason == GSSyncReason::Presentation) { ++m_syncSkipped; return; }
        drain(2);
        m_inner->Sync(reason);
    }
    PresentationFrame Present(const GSPresentationRequest &request) override { drain(3); return m_inner->Present(request); }
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override { drain(4); return m_inner->ClearFramebuffer(context, rgba); }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override { drain(5); return m_inner->ConsumeLocalToHostBytes(dst, maxBytes); }
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
    {
        drain(6);
        return m_inner->ReadVram(psm, base, bw, x, y);
    }
    void SnapshotVram(std::vector<uint8_t> &out) const override { drain(7); m_inner->SnapshotVram(out); }
    GSTransferSnapshot GetTransferSnapshot() const override { drain(8); return m_inner->GetTransferSnapshot(); }

private:
    struct ClutCmd { GSTex0Reg tex0; GSTexClutReg texclut; };
    struct UploadCmd { std::vector<uint8_t> data; };
    struct FlushCmd {};
    struct TexFlushCmd {};
    struct WriteCmd { uint32_t psm, base, bw, x, y, value; };
    using Cmd = std::variant<GSPrimitiveBatch, ClutCmd, GSTransferCommand, UploadCmd, FlushCmd, TexFlushCmd, WriteCmd>;

    void push(Cmd &&c)
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_queue.push_back(std::move(c));
            ++m_pushed;
        }
        // Arbeiter nur wecken, wenn er schläft, und dann gebündelt alle 64 Befehle (er schaut ohnehin jede ms nach)
        if (m_sleeping.load(std::memory_order_acquire) && (m_pushed & 63u) == 0u)
            m_cvWork.notify_one();
    }

    void drain(int why) const
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        ++m_syncCalls[why];
        report();
        const uint64_t target = m_pushed;   // nur bis zum Stand dieses Aufrufs warten (sonst Verhungern bei Dauerzufluss)
        if (m_done >= target)
            return;
        ++m_syncWaits[why];
        const auto t0 = std::chrono::steady_clock::now();
        m_cvWork.notify_one();
        m_cvDone.wait(lk, [this, target] { return m_done >= target; });
        m_syncWaitUs[why] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    }

    void report() const
    {
        static const bool on = std::getenv("PS2X_MTGS_STATS") != nullptr;
        if (!on) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastReport < std::chrono::seconds(10)) return;
        m_lastReport = now;
        static const char *nm[] = {"Init", "Reset", "Sync", "Present", "Clear", "L2H", "ReadVram", "Snapshot", "TransferSnap"};
        std::fprintf(stderr, "[MTGS] befehle=%llu finish/present-sync uebersprungen=%llu sync(aufrufe/gewartet/ms):", (unsigned long long)m_pushed, (unsigned long long)m_syncSkipped.load());
        for (int i = 0; i < 9; ++i)
            if (m_syncCalls[i]) std::fprintf(stderr, " %s=%llu/%llu/%.0f", nm[i], (unsigned long long)m_syncCalls[i], (unsigned long long)m_syncWaits[i], m_syncWaitUs[i] / 1000.0);
        std::fprintf(stderr, "\n");
    }

    void exec(Cmd &c)
    {
        struct V
        {
            GSRasterBackend &b;
            void operator()(GSPrimitiveBatch &p) { b.Submit(p); }
            void operator()(ClutCmd &k) { b.LoadClut(k.tex0, k.texclut); }
            void operator()(GSTransferCommand &t) { b.BeginTransfer(t); }
            void operator()(UploadCmd &u) { b.UploadImage(u.data.data(), static_cast<uint32_t>(u.data.size())); }
            void operator()(FlushCmd &) { b.Flush(); }
            void operator()(TexFlushCmd &) { b.TextureFlush(); }
            void operator()(WriteCmd &w) { b.WriteVram(w.psm, w.base, w.bw, w.x, w.y, w.value); }
        };
        std::visit(V{*m_inner}, c);
    }

    void run()
    {
        std::vector<Cmd> local;
        local.reserve(4096);
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lk(m_mutex);
                if (m_queue.empty())
                {
                    m_sleeping.store(true, std::memory_order_release);
                    m_cvWork.wait_for(lk, std::chrono::milliseconds(1), [this] { return m_quit || !m_queue.empty(); });
                    m_sleeping.store(false, std::memory_order_release);
                }
                if (m_quit && m_queue.empty())
                    return;
                local.swap(m_queue);
            }
            for (Cmd &c : local)
                exec(c);
            const size_t n = local.size();
            local.clear();
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_done += n;
            }
            m_cvDone.notify_all();
        }
    }

    std::unique_ptr<GSRasterBackend> m_inner;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cvWork, m_cvDone;
    std::vector<Cmd> m_queue;
    uint64_t m_pushed = 0, m_done = 0;
    std::atomic<bool> m_sleeping{false};
    std::atomic<uint64_t> m_syncSkipped{0};
    mutable std::array<uint64_t, 9> m_syncCalls{}, m_syncWaits{}, m_syncWaitUs{};
    mutable std::chrono::steady_clock::time_point m_lastReport{};
    bool m_quit = false;
    std::thread m_thread;
};
