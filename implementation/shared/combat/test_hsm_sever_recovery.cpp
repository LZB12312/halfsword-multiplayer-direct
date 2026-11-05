#include "combat/hsm_sever.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace hsm::combat;
    assert(NativeSeverDisplayFresh(2, 900));
    assert(NativeSeverDisplayFresh(2, kSeverPendingMs));
    assert(!NativeSeverDisplayFresh(2, kSeverPendingMs + 1));
    assert(NativeSeverDisplayFresh(5, 300));
    assert(!NativeSeverDisplayFresh(5, 401));
    assert(!NativeSeverDisplayFresh(2, -1));
    assert(!NativeSeverDisplayFresh(kSeverParts, 0));
    puts("PASS pelvis recovery window, limb fallback boundary and stale/invalid cuts");
}
