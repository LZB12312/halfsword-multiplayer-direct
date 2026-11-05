#include "gameplay/sever_ease.h"
#include "combat/hsm_sever_ease.h"
#include "ue/script_hook.h"
#include "log.h"
namespace hsm::game {
namespace { int ease=100; bool registered=false; }
void TickSeverEase(int pct) {
    ease=combat::SeverEasePct(pct);
    if(registered || !ue::ScriptHookActive()) return;
    // Cooked Constraint_Weapon_Stuck_BP.Dismemberment Check uses Damage only
    // to wear down cut-marker HP. General Get Damage and health are untouched.
    ue::HookScript("sever-ease","Constraint_Weapon_Stuck_BP_C","Dismemberment Check",[](ue::ScriptCall& call) {
        if(ease==100) return false;
        double damage=0;
        if(call.get("Damage",damage)) call.set("Damage",combat::ScaleSeverWear(damage,ease));
        return false;
    });
    registered=true;
    HSM_LOG("sever-ease: native cut-marker wear hook registered; 100%% leaves native damage unchanged");
}
}
