#ifndef PS2_VU1_H
#define PS2_VU1_H

#include <array>
#include <cstdint>
#include <vector>

class GS;
class PS2Memory;

struct VU1State
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t r;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    uint64_t cycles;
    bool ebit;
    bool haltAfterDelaySlot;
    bool dBitEnabled;
    bool tBitEnabled;
    bool stoppedByD;
    bool stoppedByT;
    uint32_t top;  // VIF TOP visible to XTOP
    uint32_t itop; // VIF ITOP visible to XITOP

    bool branchPending;
    uint32_t branchTarget;
    uint32_t branchDelay;
};

class VU1Interpreter
{
public:
    enum class Unit : uint8_t
    {
        VU0,
        VU1
    };

    explicit VU1Interpreter(Unit unit = Unit::VU1);

    void reset();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }

    // Vorab uebersetzte Mikroprogramme (src/lib/vu/ps2_vu1_compiled.cpp, erzeugt von tools/vu1rec.py).
    // Rueckgabe 0: Lauf erledigt (Ende/Budget/Stopp), 1: an m_state.pc mit dem Interpreter weitermachen.
    struct Compiled;
    friend struct Compiled;
    using CompiledFn = int (*)(VU1Interpreter &, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                               GS &gs, PS2Memory *memory, uint64_t budgetEnd, bool &programEnded);
    struct CompiledEntry
    {
        uint64_t hash;
        CompiledFn fn;
    };

private:
    enum Pipeline : uint8_t
    {
        PipelineNone = 0,
        PipelineFmac,
        PipelineLsu,
        PipelineFdiv,
        PipelineEfu,
        PipelineIalu,
        PipelineBranch,
        PipelineXgkick
    };

    struct VfAccess
    {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct InstructionUsage
    {
        std::array<VfAccess, 2> vfRead{};
        VfAccess vfWrite{};
        uint8_t vfReadCount = 0;
        uint16_t viRead = 0;
        uint16_t viWrite = 0;
        uint8_t accRead = 0;
        uint8_t accWrite = 0;
        uint8_t latency = 0;
        uint8_t vfLatency = 0;
        uint8_t viLatency = 0;
        Pipeline pipeline = PipelineNone;
        bool waitQ = false;
        bool waitP = false;
        bool readsClip = false;
        bool writesClip = false;
        bool delaysNextBranchRead = false;
        bool reserved = false;
    };

    struct DecodedInstructionPair
    {
        uint32_t lower = 0;
        uint32_t upper = 0;
        InstructionUsage lowerUsage{};
        InstructionUsage upperUsage{};
        bool iBit = false;
        bool eBit = false;
        bool mBit = false;
        bool dBit = false;
        bool tBit = false;
        uint8_t upperVfShadowReg = 0;
        uint8_t suppressedLowerVf = 0;
    };

    struct FlagPipelineEntry
    {
        uint64_t readyCycle = 0;
        uint64_t issueCycle = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t extraSticky = 0;
        uint32_t clip = 0;
        bool valid = false;
        bool writesMac = false;
        bool writesStatus = false;
        bool writesSticky = false;
        bool writesClip = false;
    };

    struct ScalarPipelineEntry
    {
        uint64_t readyCycle = 0;
        float value = 0.0f;
        uint32_t statusDi = 0;
        bool valid = false;
    };

    struct PendingStore
    {
        uint64_t readyCycle = 0;
        uint32_t address = 0;
        std::array<uint32_t, 4> words{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingVfWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t reg = 0;
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingViWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        int32_t value = 0;
        uint8_t reg = 0;
        bool valid = false;
    };

    struct PendingAccWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct XgkickPipeline
    {
        static constexpr uint32_t kBufferSize = 0x10000u;
        std::array<uint8_t, kBufferSize> packet{};
        uint32_t sourceAddress = 0;
        uint32_t totalBytes = 0;
        uint32_t copiedBytes = 0;
        uint32_t currentTagEnd = 0;
        uint32_t cycleCredit = 0;
        uint64_t issueCycle = 0;
        bool active = false;
        bool currentTagEop = false;
    };

    static constexpr uint32_t kFmacLatency = 4u;
    static constexpr uint32_t kAccForwardLatency = 1u;
    static constexpr uint32_t kMaxFlagEntries = 8u;
    static constexpr uint32_t kMaxPendingStores = 8u;
    static constexpr uint32_t kMaxPendingVfWrites = 16u;
    static constexpr uint32_t kMaxPendingViWrites = 8u;
    static constexpr uint32_t kMaxPendingAccWrites = 8u;
    static constexpr uint32_t kMaxDecodedPairs = 0x4000u / 8u;

    Unit m_unit;
    VU1State m_state;
    std::array<DecodedInstructionPair, kMaxDecodedPairs> m_decodedCodeCache{};
    const uint8_t *m_cachedVuCode = nullptr;
    const PS2Memory *m_cachedMemory = nullptr;
    uint32_t m_cachedCodeSize = 0;
    uint64_t m_cachedCodeGeneration = 0;
    bool m_decodedCodeCacheValid = false;
    std::vector<uint8_t> *m_gifCapture = nullptr; // Differenz-Test: XGKICK-Pakete mitschreiben
    bool m_gifSubmit = true;                       // false: nur mitschreiben (Referenzlauf)
    bool m_compiledDisabled = false;
    uint64_t m_recGen = ~0ull;
    uint64_t m_recPend = 0;
    // Je Objekt (vorher global, wurde bei jedem VU0/VU1-Wechsel verworfen): fruehester Fertig-Zyklus und Belegungsmasken
    uint64_t m_nextCommit = 0;
    struct Occupancy { uint32_t flag = ~0u, store = ~0u, vf = ~0u, vi = ~0u, acc = ~0u; } m_occ;
    void noteReady(uint64_t readyCycle) { if (readyCycle < m_nextCommit) m_nextCommit = readyCycle; }
    // Schlanke Fassung von updateFmacFlags fuer uebersetzte Programme (FAST: ohne Produkt-Sticky). Gleiches Ergebnis.
    // Stufe 3: XGKICK sofort komplett uebertragen (wie microVU) statt Takt fuer Takt
    void xgkickNow(uint32_t qwordAddress)
    {
        if (m_xgkick.active) progressXgkickBulk();
        startXgkick(qwordAddress);
        if (m_xgkick.active) progressXgkickBulk();
    }
    // Uebertraegt eine laufende XGKICK sofort komplett: GIFtags einzeln ueber progressXgkick (Pruefungen wie bisher),
    // die Nutzdaten eines Tags am Stueck. Gleiches Paket, gleiche Laenge wie Quadword fuer Quadword.
    void progressXgkickBulk();
    // Statische Taktplanung (tools/vu1rec.py): uebersetzter Code prueft keine Faelligkeiten je Paar mehr. Deshalb darf
    // beim Einstieg nichts in den Interpreter-Warteschlangen stehen, das er sonst dort eingetragen haette.
    bool hasPendingOther() const
    {
        for (const auto &e : m_flagPipeline) if (e.valid) return true;
        for (const auto &s : m_storePipeline) if (s.valid) return true;
        for (const auto &a : m_accWritePipeline) if (a.valid) return true;
        for (const auto &e : m_efu) if (e.valid) return true;
        return m_xgkick.active;
    }
    // Wiedereinstieg aus dem Interpreter: der uebersetzte Code prueft Bereitschaft nur fuer die Register, die an diesem
    // Einstieg aus uebersetztem Code noch unterwegs sein koennen - aus dem Interpreter darf deshalb nichts unterwegs sein
    bool allWritesRetired() const
    {
        for (const auto &r : m_vfReady) for (uint64_t c : r) if (c > m_cycle) return false;
        for (uint64_t c : m_viReady) if (c > m_cycle) return false;
        for (uint64_t c : m_accReady) if (c > m_cycle) return false;
        return true;
    }
    // Q-Ergebnis sichtbar machen (wie commitReadyPipelines fuer m_fdiv); der uebersetzte Code ruft es vor Q-Lesern/DIV
    void publishQ()
    {
        m_state.q = m_fdiv.value;
        const uint32_t di = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | di | (di << 6);
        m_fdiv = {};
    }
    bool hasPendingRegWrites() const
    {
        for (const auto &w : m_vfWritePipeline) if (w.valid) return true;
        for (const auto &w : m_viWritePipeline) if (w.valid) return true;
        for (const auto &w : m_accWritePipeline) if (w.valid) return true;
        return false;
    }
    void pushFmacFlags(uint32_t mac, uint32_t status)
    {
        for (uint32_t slot = 0; slot < kMaxFlagEntries; ++slot)
        {
            FlagPipelineEntry &e = m_flagPipeline[slot];
            if (e.valid)
                continue;
            e = {};
            e.valid = true;
            m_occ.flag |= 1u << slot;
            e.issueCycle = m_cycle;
            e.readyCycle = m_cycle + kFmacLatency;
            noteReady(e.readyCycle);
            e.mac = mac;
            e.status = status;
            e.writesMac = true;
            e.writesStatus = true;
            return;
        }
        reportReservedInstruction(true, 0xFFFFFFFFu);
    } // uebersetzte Programme schreiben sofort: spaetester Zyklus, bis zu dem der Interpreter noch Schreibvorgaenge in der Warteschlange haette
    CompiledFn m_recFn = nullptr;
    const uint8_t *m_recEntry = nullptr; // Einstiegskarte des uebersetzten Programms (je Paar 1 Byte)
    uint32_t m_recSkipPc = ~0u;          // dort ist der Wiedereinstieg gerade gescheitert
    void dumpDecoded(const uint8_t *vuCode, uint32_t codeSize, const char *path) const;
    bool m_flagsLive = true; // Mikroprogramm liest MAC/Status (FS*/FM*)? Sonst Flag-Pipeline sparen

    std::array<FlagPipelineEntry, kMaxFlagEntries> m_flagPipeline{};
    ScalarPipelineEntry m_fdiv{};
    std::array<ScalarPipelineEntry, 2> m_efu{};
    std::array<PendingStore, kMaxPendingStores> m_storePipeline{};
    std::array<PendingVfWrite, kMaxPendingVfWrites> m_vfWritePipeline{};
    std::array<PendingViWrite, kMaxPendingViWrites> m_viWritePipeline{};
    std::array<PendingAccWrite, kMaxPendingAccWrites> m_accWritePipeline{};
    XgkickPipeline m_xgkick{};
    std::array<std::array<uint64_t, 4>, 32> m_vfReady{};
    std::array<uint64_t, 16> m_viReady{};
    std::array<uint64_t, 4> m_accReady{};
    std::array<std::array<uint64_t, 4>, 32> m_vfLatestWrite{};
    std::array<uint64_t, 16> m_viLatestWrite{};
    std::array<uint64_t, 4> m_accLatestWrite{};

    uint64_t m_cycle = 0;
    uint64_t m_nextWriteSequence = 0;
    uint64_t m_efuResourceReady = 0;
    uint32_t m_workingClip = 0;
    uint32_t m_currentUpperInstruction = 0;
    int32_t m_viBranchBackupValue = 0;
    uint8_t m_viBranchBackupReg = 0;
    bool m_viBranchBackupValid = false;
    uint8_t *m_activeVuData = nullptr;
    uint32_t m_activeVuDataSize = 0;
    GS *m_activeGs = nullptr;
    PS2Memory *m_activeMemory = nullptr;
    bool m_stopRequested = false;
    bool m_pendingHaltD = false;
    bool m_pendingHaltT = false;

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);

    InstructionUsage decodeUpperUsage(uint32_t upper) const;
    InstructionUsage decodeLowerUsage(uint32_t lower) const;
    static void addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static void addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static uint8_t vfReadLanes(const InstructionUsage &usage, uint8_t reg);
    DecodedInstructionPair decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const;
    DecodedInstructionPair getDecodedInstructionPairForPc(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc);
    const DecodedInstructionPair *decodedPairPtr(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc);
    DecodedInstructionPair m_decodeScratch{};
    void rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory, uint64_t generation);

    void execUpper(uint32_t instr);
    void execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);

    void applyDest(float *dst, const float *result, uint8_t dest);
    void applyDestAcc(const float *result, uint8_t dest);
    void applyFmacDest(float *dst, float *result, uint8_t dest);
    void applyFmacDestAcc(float *result, uint8_t dest);
    void normalizeFmacResult(float *result, uint8_t dest, uint8_t laneFlags[4]);
    bool calculateFmacExactResult(uint32_t component, long double &result) const;
    uint8_t normalizeFmacExactResult(float &value, long double exactResult) const;
    uint32_t calculateFmacProductSticky(uint8_t dest) const;
    void updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky);
    void queueFsset(uint16_t immediate);
    void queueClip(uint32_t clip);
    void queueFcset(uint32_t clip);
    void queueQ(float value, uint32_t latency, uint32_t statusDi);
    void queueP(float value, uint32_t latency);
    void queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    void queueVfWrite(uint8_t reg, uint8_t laneMask, const float value[4], uint32_t latency);
    void queueViWrite(uint8_t reg, int32_t value, uint32_t latency);
    void queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency);
    void startXgkick(uint32_t qwordAddress);

    void resetScheduler();
    void commitReadyPipelines();
    void advanceOneCycle();
    void advanceTo(uint64_t targetCycle);
    void flushPipelines();
    void progressXgkick();
    void finishXgkick();
    uint64_t calculatePairReadyCycle(const DecodedInstructionPair &decoded) const;
    void markPairWrites(const DecodedInstructionPair &decoded);
    bool pipelinesPending() const;

    float normalizeOperand(float value) const;
    float normalizeResult(float value, uint32_t &laneFlags) const;
    uint32_t microAddressMask() const;
    int32_t readBranchVi(uint8_t reg) const;
    void recordViWriteForBranch(uint8_t reg, int32_t oldValue);
    void reportReservedInstruction(bool upper, uint32_t instruction);
    float broadcast(const float *vf, uint8_t bc);
};

#endif
