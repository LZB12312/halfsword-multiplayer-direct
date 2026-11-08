// ProcessEvent interception. Native code reaches Blueprints only through ProcessEvent (input events,
// component hit/overlap delegates, ticks), so this is the single choke point for observing or
// suppressing those entry points. Blueprint-to-Blueprint calls inside the VM do not pass through here.
#pragma once
#include "ue/engine.h"
#include <functional>
#include <string>

namespace hsm::ue {

// Return true to block the call (the original ProcessEvent is skipped).
using PreHook = std::function<bool(UObject* obj, UFunction* fn, void* params)>;

bool InstallProcessEventHook();
void RemoveProcessEventHook();

// Exact-function rule.
void HookFunction(UFunction* fn, PreHook hook);
// Name rule, evaluated once per distinct UFunction the first time it is seen (covers classes loaded later).
void HookFunctionsMatching(std::function<bool(const std::string& functionName)> match, PreHook hook);
// Hook an arbitrary native function (MinHook). Safe to call before or after InstallProcessEventHook.
bool HookNative(void* target, void* detour, void** original, const char* label);
// The game thread: first a guess, the thread of the first ProcessEvent call (0 before it); NoteGameThread corrects it.
unsigned long GameThreadId();
// Called from an actor's ReceiveTick, which only the game thread runs. The bundled UE4SS makes the first ProcessEvent call
// from its own thread, and with that guess every game-thread check (the script VM hooks: claims, fx, gore) refused the game.
void NoteGameThread();
// The pre-hooks' share of the game thread: calls that ran at least one hook, and the time spent in those hooks
// (outermost calls only, so a hook's own ProcessEvent calls and the hooks they run are counted once, inside it).
struct ProcessEventHookStats {
    uint64_t calls = 0;
    uint64_t hookNs = 0;
    uint64_t all = 0;      // every ProcessEvent call on the game thread, hooked or not
};
ProcessEventHookStats GetProcessEventHookStats();   // game thread
// Takes ns out of the hook time of the outermost hooked call this thread is in: work done inside a hook but reported
// apart (the session tick inside the GameMode's ReceiveTick hook). Game thread.
void DiscountHookTime(uint64_t ns);
// The n hooked functions that took longest since the last call, per frame ("Name 1.23 ms/45, ..."); resets. Game thread.
std::string TopHookFunctions(int n, double frames);
// Drop all rules (e.g. when a session ends).
void ClearHooks();

}  // namespace hsm::ue
