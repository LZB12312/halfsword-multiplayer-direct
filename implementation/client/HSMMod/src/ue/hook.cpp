#include <atomic>
#include "ue/hook.h"
#include "log.h"
#include "ue/profiler.h"
#include <MinHook.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace hsm::ue {
namespace {
using ProcessEventFn = void (*)(UObject*, UFunction*, void*);
ProcessEventFn g_original = nullptr;
void* g_target = nullptr;

struct NameRule { std::function<bool(const std::string&)> match; PreHook hook; };

// A function's hooks are an immutable list, replaced (never edited) when a rule is added: a hooked call takes a reference
// instead of copying the list (test 5: the hit hook alone ran 8970 times in one frame).
using HookList = std::shared_ptr<const std::vector<PreHook>>;
std::shared_mutex g_mu;
std::unordered_map<UFunction*, HookList> g_exact;                  // includes resolved name rules
std::unordered_map<UFunction*, bool> g_classified;                 // function already checked against name rules
std::vector<NameRule> g_nameRules;

// Functions with no hooks, per thread: almost every ProcessEvent call of a frame, so they skip the lock and the lookups.
// A slot counts only at the g_hookGen it was filled at; every change to the hooks bumps it (with the unique lock held).
std::atomic<uint32_t> g_hookGen{1};
struct NoHookSlot { UFunction* fn; uint32_t gen; };
constexpr size_t kNoHookSlots = 1024;
thread_local NoHookSlot t_noHook[kNoHookSlots];
NoHookSlot& NoHookSlotFor(UFunction* fn) {
    return t_noHook[size_t((uint64_t(reinterpret_cast<uintptr_t>(fn)) * 0x9E3779B97F4A7C15ull) >> 54)];   // top 10 bits
}

// Adds a hook to a function's list. Requires the unique lock.
void AddHook(UFunction* fn, PreHook hook) {
    HookList& list = g_exact[fn];
    auto next = std::make_shared<std::vector<PreHook>>(list ? *list : std::vector<PreHook>{});
    next->push_back(std::move(hook));
    list = std::move(next);
    g_hookGen.fetch_add(1, std::memory_order_release);
}

// Resolve name rules for a function seen for the first time. Requires the unique lock.
void Classify(UFunction* fn) {
    g_classified[fn] = true;
    if (g_nameRules.empty()) return;
    std::string name = ObjectName(reinterpret_cast<UObject*>(fn));
    for (const NameRule& r : g_nameRules) if (r.match(name)) AddHook(fn, r.hook);
}

std::atomic<unsigned long> g_gameThreadId{0};

// Stats (GetProcessEventHookStats): game thread only. t_inHooks is the depth of hook callbacks this thread is in.
uint64_t g_hookCalls = 0, g_hookNs = 0, g_allCalls = 0;
struct FnCost { uint64_t calls = 0, ns = 0; };
std::unordered_map<UFunction*, FnCost> g_fnCost;   // per hooked function, since the last TopHookFunctions
thread_local int t_inHooks = 0;
thread_local uint64_t t_discountNs = 0;   // DiscountHookTime, summed

int64_t QpcNs(int64_t ticks) {
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return ticks * 1000000000ll / freq;
}
int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

struct InHooks {
    InHooks() { ++t_inHooks; }
    ~InHooks() { --t_inHooks; }
    InHooks(const InHooks&) = delete;
    InHooks& operator=(const InHooks&) = delete;
};

void Detour(UObject* obj, UFunction* fn, void* params) {
    static std::atomic<bool> first{true};
    if (first.load(std::memory_order_relaxed) && first.exchange(false)) {
        g_gameThreadId.store(GetCurrentThreadId());
        HSM_LOG("hook: first ProcessEvent call (%s)", fn ? ObjectName(reinterpret_cast<UObject*>(fn)).c_str() : "?");
    }
    bool block = false;
    if (GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed)) ++g_allCalls;
    prof::Scope call(fn, prof::Kind::kEvent);   // the whole call: our hooks and the original
    if (obj && fn) {
        const uint32_t gen = g_hookGen.load(std::memory_order_acquire);   // before the lookup: a later change stales the slot
        NoHookSlot& slot = NoHookSlotFor(fn);
        if (slot.fn == fn && slot.gen == gen) { g_original(obj, fn, params); return; }
        HookList hooks;
        {
            std::shared_lock<std::shared_mutex> lock(g_mu);
            bool known = g_classified.count(fn) != 0;
            if (known) { auto it = g_exact.find(fn); if (it != g_exact.end()) hooks = it->second; }
            if (!known) {
                lock.unlock();
                std::unique_lock<std::shared_mutex> w(g_mu);
                if (!g_classified.count(fn)) Classify(fn);
                auto it = g_exact.find(fn); if (it != g_exact.end()) hooks = it->second;
            }
        }
        if (!hooks || hooks->empty()) slot = {fn, gen};   // filled before any hook runs (a hook may re-enter this detour)
        // Hooks run without the lock so they may call ProcessEvent (re-entering this detour); the list they run
        // stays alive (and unchanged) even if a rule is added meanwhile.
        if (hooks && !hooks->empty()) {
            const bool timed = t_inHooks == 0 && GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed);
            const int64_t t0 = timed ? Qpc() : 0;
            const uint64_t discount0 = t_discountNs;
            {
                InHooks in;
                prof::Scope ours(fn, prof::Kind::kHook);
                for (const PreHook& h : *hooks) if (h(obj, fn, params)) block = true;
            }
            if (timed) {
                const uint64_t spent = uint64_t(QpcNs(Qpc() - t0)), discount = t_discountNs - discount0;
                const uint64_t ns = spent > discount ? spent - discount : 0;
                ++g_hookCalls;
                g_hookNs += ns;
                FnCost& f = g_fnCost[fn];
                ++f.calls;
                f.ns += ns;
            }
        }
    }
    if (!block) g_original(obj, fn, params);
}
}  // namespace

bool EnsureMinHook() {
    static bool ok = MH_Initialize() == MH_OK;
    return ok;
}

bool HookNative(void* target, void* detour, void** original, const char* label) {
    if (!target || !EnsureMinHook()) return false;
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        HSM_LOG("hook: failed to hook %s at %p", label, target); return false;
    }
    HSM_LOG("hook: %s hooked at %p", label, target);
    return true;
}

bool InstallProcessEventHook() {
    g_target = ProcessEventAddress();
    if (!g_target) { HSM_LOG("hook: ProcessEvent address unavailable"); return false; }
    if (!EnsureMinHook()) { HSM_LOG("hook: MH_Initialize failed"); return false; }
    if (MH_CreateHook(g_target, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(g_target) != MH_OK) {
        HSM_LOG("hook: failed to hook ProcessEvent at %p", g_target); return false;
    }
    HSM_LOG("hook: ProcessEvent hooked at %p", g_target);
    return true;
}

void RemoveProcessEventHook() {
    if (g_target) { MH_DisableHook(g_target); MH_RemoveHook(g_target); g_target = nullptr; }
}

void HookFunction(UFunction* fn, PreHook hook) {
    if (!fn) return;
    std::unique_lock<std::shared_mutex> lock(g_mu);
    AddHook(fn, std::move(hook));
}

void HookFunctionsMatching(std::function<bool(const std::string&)> match, PreHook hook) {
    std::unique_lock<std::shared_mutex> lock(g_mu);
    g_nameRules.push_back({match, hook});
    // Re-check functions already classified so the rule also applies to them.
    for (auto& [fn, _] : g_classified)
        if (match(ObjectName(reinterpret_cast<UObject*>(fn)))) AddHook(fn, hook);
}

void ClearHooks() {
    std::unique_lock<std::shared_mutex> lock(g_mu);
    g_exact.clear(); g_classified.clear(); g_nameRules.clear();
    g_hookGen.fetch_add(1, std::memory_order_release);
}

unsigned long GameThreadId() { return g_gameThreadId.load(); }

void NoteGameThread() {
    const unsigned long me = GetCurrentThreadId();
    if (g_gameThreadId.load(std::memory_order_relaxed) == me) return;
    const unsigned long was = g_gameThreadId.exchange(me);
    if (was != me) HSM_LOG("hook: game thread is %lu; the first ProcessEvent call came from thread %lu", me, was);
}

void DiscountHookTime(uint64_t ns) { t_discountNs += ns; }

std::string TopHookFunctions(int n, double frames) {
    std::vector<std::pair<UFunction*, FnCost>> v(g_fnCost.begin(), g_fnCost.end());
    g_fnCost.clear();
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    if (frames < 1.0) frames = 1.0;
    std::string out;
    char b[200];
    for (int i = 0; i < n && i < int(v.size()); ++i) {
        std::snprintf(b, sizeof b, "%s%s %.2f ms/%.0f", out.empty() ? "" : ", ", ObjectName(reinterpret_cast<UObject*>(v[size_t(i)].first)).c_str(),
                      double(v[size_t(i)].second.ns) / 1e6 / frames, double(v[size_t(i)].second.calls) / frames);
        out += b;
    }
    return out;
}

ProcessEventHookStats GetProcessEventHookStats() {
    ProcessEventHookStats s;
    s.calls = g_hookCalls;
    s.hookNs = g_hookNs;
    s.all = g_allCalls;
    return s;
}

}  // namespace hsm::ue
