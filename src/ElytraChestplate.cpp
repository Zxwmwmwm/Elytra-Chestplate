/**
 * Elytra Chestplate - probe build.
 *
 * Wear a chestplate and an elytra at the same time (singleplayer).
 *
 * This build does NOT change gameplay yet. It exists to pin down two things
 * the stripped 1.26.60.4 engine does not expose as symbols:
 *   - where the local player's armour value lives,
 *   - where the "is gliding" state lives.
 *
 * How it works: the GameMode break hooks (already proven in production) hand
 * us the GameMode pointer, and GameMode + 0x08 is the local player. A 1 Hz
 * worker thread then dumps raw bytes of that player object to logcat, so the
 * dumps taken in different armour / glide states can be diffed offline.
 *
 * Platform: LeviLaunchroid / LeviLamina (Android ARM64)
 * Target:   Minecraft Bedrock 1.26.60.4
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <link.h>
#include <android/log.h>
#include <unistd.h>

#include <pl/Export.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#define LOG_TAG "ElytraChestplate"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

namespace {

constexpr const char* kModId      = "Elytra Chestplate";   // must equal manifest name
constexpr const char* kModuleName = "Elytra Chestplate";
constexpr const char* kModuleId   = "elytrachestplate";
constexpr const char* kEngine     = "libminecraftpe.so";

// ---- RVAs (MCBE 1.26.60.4, disassembly-verified) --------------------------
constexpr uintptr_t kOffGameModeDestroyBlock = 0x1009D860; // bool(gm, BlockPos&, u8 face, void*, bool& out)
constexpr uintptr_t kOffRakConnect           = 0x0CBC76FC; // RakNetConnector::connect
constexpr uintptr_t kOffRakDisconnect        = 0x0CBC7E34;

// GameMode + 0x08 holds the local player pointer.
constexpr uintptr_t kGmPlayerOffset = 0x08;

// How much of the player object we dump, and how often.
constexpr size_t   kDumpBytes       = 0x600;
constexpr uint64_t kDumpIntervalMs  = 1000;

// ---- state ----------------------------------------------------------------
std::atomic<bool>   g_enabled{true};
std::atomic<bool>   g_remoteServer{false};
std::atomic<void*>  g_player{nullptr};
std::atomic<bool>   g_hooksInstalled{false};
std::atomic<bool>   g_stopThread{false};
uintptr_t           g_engineBase = 0;
uint64_t            g_startMs    = 0;

using DestroyBlockFn        = bool (*)(void* gm, const void* pos, uint8_t face, void* a3, bool& destroyedOut);
using RakConnectFn          = bool (*)(void* self, const void* a1, const void* a2);
using RakDisconnectFn       = void (*)(void* self);

DestroyBlockFn   g_origDestroyBlock = nullptr;
RakConnectFn     g_origRakConnect   = nullptr;
RakDisconnectFn  g_origRakDisconnect= nullptr;

pl::memory::HookHandle g_hDestroyBlock;
pl::memory::HookHandle g_hRakConnect;
pl::memory::HookHandle g_hRakDisconnect;

uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uintptr_t getModuleBase(const char* name) {
    uintptr_t base = 0;
    struct Iter { const char* target; uintptr_t* p; } data{name, &base};
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* ptr) -> int {
        auto* d = static_cast<Iter*>(ptr);
        if (info && info->dlpi_name && std::strstr(info->dlpi_name, d->target)) {
            *d->p = static_cast<uintptr_t>(info->dlpi_addr);
            return 1;
        }
        return 0;
    }, &data);
    return base;
}

// ---------------------------------------------------------------------------
// probe worker: periodic raw dump of the local player object
// ---------------------------------------------------------------------------
void dumpWorker() {
    uint64_t lastDump = 0;
    while (!g_stopThread.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!g_enabled.load(std::memory_order_relaxed)) continue;

        void* p = g_player.load(std::memory_order_relaxed);
        if (!p) continue;
        uint64_t t = nowMs();
        if (t - lastDump < kDumpIntervalMs) continue;
        lastDump = t;

        auto* base = static_cast<const uint8_t*>(p);
        LOGI("DUMP t=%llums player=%p remote=%d",
             (unsigned long long)(t - g_startMs), p,
             g_remoteServer.load() ? 1 : 0);
        for (size_t off = 0; off < kDumpBytes; off += 0x20) {
            const uint64_t* q = reinterpret_cast<const uint64_t*>(base + off);
            LOGI("DUMP %03zx %016llx %016llx %016llx %016llx", off,
                 (unsigned long long)q[0], (unsigned long long)q[1],
                 (unsigned long long)q[2], (unsigned long long)q[3]);
        }
        LOGI("DUMP end");
    }
}

// ---------------------------------------------------------------------------
// detours
// ---------------------------------------------------------------------------
bool detour_destroyBlock(void* gm, const void* pos, uint8_t face, void* a3, bool& destroyedOut) {
    // Capture the local player the first time the player actually breaks a block.
    if (gm) {
        void* player = nullptr;
        std::memcpy(&player, static_cast<const uint8_t*>(gm) + kGmPlayerOffset, sizeof(player));
        void* expected = nullptr;
        if (player && g_player.compare_exchange_strong(expected, player)) {
            LOGI("Local player captured from GameMode: %p (gm=%p)", player, gm);
        }
    }
    if (g_origDestroyBlock) return g_origDestroyBlock(gm, pos, face, a3, destroyedOut);
    return false;
}

bool detour_rakConnect(void* self, const void* a1, const void* a2) {
    g_remoteServer.store(true, std::memory_order_relaxed);
    LOGI("RakNetConnector::connect -> remote server/Realm (singleplayer features off)");
    if (g_origRakConnect) return g_origRakConnect(self, a1, a2);
    return false;
}

void detour_rakDisconnect(void* self) {
    g_remoteServer.store(false, std::memory_order_relaxed);
    LOGI("RakNetConnector::disconnect -> back to local");
    if (g_origRakDisconnect) g_origRakDisconnect(self);
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
void installHooks() {
    if (g_hooksInstalled.exchange(true)) return;
    g_engineBase = getModuleBase(kEngine);
    if (!g_engineBase) { LOGW("libminecraftpe.so not mapped yet"); g_hooksInstalled.store(false); return; }

    LOGI("Probe hooks at base 0x%lx", (unsigned long)g_engineBase);

    void* pDestroy = reinterpret_cast<void*>(g_engineBase + kOffGameModeDestroyBlock);
    g_hDestroyBlock = pl::memory::HookHandle(pDestroy,
        reinterpret_cast<void*>(&detour_destroyBlock),
        reinterpret_cast<void**>(&g_origDestroyBlock));
    LOGI("Hook GameMode::destroyBlock: %s", g_hDestroyBlock.installed() ? "OK" : "FAILED");

    void* pConn = reinterpret_cast<void*>(g_engineBase + kOffRakConnect);
    g_hRakConnect = pl::memory::HookHandle(pConn,
        reinterpret_cast<void*>(&detour_rakConnect),
        reinterpret_cast<void**>(&g_origRakConnect));
    LOGI("Hook RakNetConnector::connect: %s", g_hRakConnect.installed() ? "OK" : "FAILED");

    void* pDisc = reinterpret_cast<void*>(g_engineBase + kOffRakDisconnect);
    g_hRakDisconnect = pl::memory::HookHandle(pDisc,
        reinterpret_cast<void*>(&detour_rakDisconnect),
        reinterpret_cast<void**>(&g_origRakDisconnect));
    LOGI("Hook RakNetConnector::disconnect: %s", g_hRakDisconnect.installed() ? "OK" : "FAILED");
}

bool registerUi() {
    using pl::modmenu::ModuleBuilder;
    return ModuleBuilder(kModuleId, kModuleName)
        .description("Probe build: logs player state so chestplate + elytra dual-wear can be built.")
        .modId(kModId)
        .defaultEnabled(true)
        .hideInHudEditor(true)
        .onToggle([](std::string_view mod, bool enabled) {
            if (mod != kModuleId) return;
            g_enabled.store(enabled);
            LOGI("Probe toggled: %s", enabled ? "ENABLED" : "DISABLED");
        })
        .registerModule();
}

class ElytraChestplateMod {
public:
    bool load() {
        g_startMs = nowMs();
        LOGI("ElytraChestplate probe loading (v1.0.0, MCBE 1.26.60.4)");
        LOGI("Instructions: break ONE block, then follow the test steps in the release notes.");
        registerUi();
        installHooks();
        g_stopThread.store(false);
        std::thread(dumpWorker).detach();
        LOGI("Probe worker started (1 Hz dumps of 0x%zx bytes)", kDumpBytes);
        return true;
    }
    bool enable()  { g_enabled.store(true);  return true; }
    bool disable() { g_enabled.store(false); return true; }
    bool unload() {
        g_stopThread.store(true);
        if (g_hDestroyBlock.installed())   g_hDestroyBlock.reset();
        if (g_hRakConnect.installed())     g_hRakConnect.reset();
        if (g_hRakDisconnect.installed())  g_hRakDisconnect.reset();
        pl::modmenu::unregisterModule(kModuleId);
        return true;
    }
};

static ElytraChestplateMod s_modInstance;

struct ModRegistration {
    void* instance;
    bool (*load)(void*, void*);
    bool (*enable)(void*, void*);
    bool (*disable)(void*, void*);
    bool (*unload)(void*, void*);
};

static ModRegistration s_registration = {
    &s_modInstance,
    [](void* i, void*) -> bool { return static_cast<ElytraChestplateMod*>(i)->load(); },
    [](void* i, void*) -> bool { return static_cast<ElytraChestplateMod*>(i)->enable(); },
    [](void* i, void*) -> bool { return static_cast<ElytraChestplateMod*>(i)->disable(); },
    [](void* i, void*) -> bool { return static_cast<ElytraChestplateMod*>(i)->unload(); },
};

} // namespace

extern "C" {

PL_EXPORT ModRegistration* PLGetModRegistration() { return &s_registration; }
PL_EXPORT void* plInitMod() { return &s_registration; }

PL_EXPORT bool PLMod_Load(void*)    { return s_modInstance.load(); }
PL_EXPORT bool PLMod_Enable(void*)  { return s_modInstance.enable(); }
PL_EXPORT bool PLMod_Disable(void*) { return s_modInstance.disable(); }
PL_EXPORT bool PLMod_Unload(void*)  { return s_modInstance.unload(); }

PL_EXPORT bool LeviMod_Load(void* h) { return PLMod_Load(h); }
PL_EXPORT bool mod_load(void* h)     { return PLMod_Load(h); }
PL_EXPORT bool mod_unload(void* h)   { return PLMod_Unload(h); }

}
