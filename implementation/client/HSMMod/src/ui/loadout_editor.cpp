#include "ui/loadout_editor.h"
#include "ui/umg.h"
#include "ui/mp_widgets.h"
#include "log.h"
#include <vector>
namespace hsm::ui::loadout {
using namespace ue;
namespace {
struct Ref {
    UObject* p=nullptr; int32_t index=-1;
    void Set(UObject* o) { p=o; index=o?ObjectIndex(o):-1; }
    bool Live() const { return p && ObjectAt(index)==p && !IsGarbage(p); }
};
struct State {
    Ref editor,preview,camera,light,controller,pawn,view;
    Kit kit; mp::Flat back;
    bool active=false,freeMode=true;
} s;
UObject* Statics(const char* cls) { const auto* c=FindClass(cls); return c?DefaultObject(c):nullptr; }
bool TagInventory(UObject* actor) {
    auto* prop=FindProperty(ClassOf(actor),"Tags"); void* dest=FieldPtr(actor,"Tags");
    if(!prop || !dest) return false;
    FName tag=MakeName("Inventory");
    struct Names { FName* data; int32_t num,max; } value{&tag,1,1};
    CopyPropertyValue(prop,dest,&value); return true;
}
}
bool IsOpen() { return s.active; }
void Close(game::Adapter& a,bool restoreView) {
    if(!s.active) return;
    if(s.camera.Live()) Call(s.camera.p,"Destroy Spawned Item").invoke();
    if(s.editor.Live()) Call(s.editor.p,"RemoveFromParent").invoke();
    if(restoreView && s.controller.Live()) {
        if(s.pawn.Live()) Call(s.controller.p,"Possess").set<UObject*>("InPawn",s.pawn.p).invoke();
        else Call(s.controller.p,"UnPossess").invoke();
        if(s.view.Live()) Call(s.controller.p,"SetViewTargetWithBlend").set<UObject*>("NewViewTarget",s.view.p).set<float>("BlendTime",0).invoke();
    }
    for(auto* r:{&s.preview,&s.camera,&s.light}) if(r->Live()) a.DestroyActor(r->p);
    if(restoreView) SetBool(a.GameInstance(),"Free Mode Activated",s.freeMode);
    // The stock Back button creates these; our lobby supplies their replacement.
    for(const char* cls:{"UI_Hub_Tavern_C","UI_Tevern_LowerPanel_C"})
        for(auto* w:FindInstances(cls)) if(InViewport(w)) Call(w,"RemoveFromParent").invoke();
    s=State{};
    if(restoreView) a.MenuInputBack();
    HSM_LOG("loadout-editor: closed; lobby connection retained");
}
std::string Open(game::Adapter& a,UObject* context) {
    if(s.active) return "Equipment editor is already open";
    // Verified against the cooked UI_Hub_Tavern inventory handler and the
    // UI_Tavern_Inventory PreConstruct graph. No menu navigation or map travel.
    UClass* editor=a.LoadClassBlocking("/Game/UI/Tavern/UI_Tavern_Inventory.UI_Tavern_Inventory_C");
    UClass* dress=a.LoadClassBlocking("/Game/Character/Blueprints/Willie_BP_DressUp.Willie_BP_DressUp_C");
    UClass* camera=a.LoadClassBlocking("/Game/Blueprints/Utility/BP_FreeCamera.BP_FreeCamera_C");
    UObject* gameplay=Statics("GameplayStatics"),*widgets=Statics("WidgetBlueprintLibrary");
    if(!editor || !dress || !camera || !gameplay || !widgets) return "Equipment editor assets could not be loaded";
    s.active=true; GetBool(a.GameInstance(),"Free Mode Activated",s.freeMode);
    SetBool(a.GameInstance(),"Free Mode Activated",true);
    Call pc(gameplay,"GetPlayerController"); pc.set<UObject*>("WorldContextObject",context).set<int32_t>("PlayerIndex",0);
    s.controller.Set(pc.invoke()?pc.get<UObject*>("ReturnValue"):nullptr);
    if(s.controller.Live()) {
        Call pawn(s.controller.p,"K2_GetPawn"); s.pawn.Set(pawn.invoke()?pawn.get<UObject*>("ReturnValue"):nullptr);
        Call view(s.controller.p,"GetViewTarget"); s.view.Set(view.invoke()?view.get<UObject*>("ReturnValue"):nullptr);
    }
    s.preview.Set(a.SpawnActorAt(dress,{5000,5000,1000},{}));
    s.camera.Set(a.SpawnActorAt(camera,{5250,5000,1080},{0,180,0}));
    if(!s.controller.Live() || !s.preview.Live() || !s.camera.Live() || !TagInventory(s.preview.p) || !TagInventory(s.camera.p)) {
        Close(a); return "Could not prepare the equipment preview";
    }
    // The stock map supplies this instance reference. The camera uses it for
    // equipment drag/drop and pointing, so a spawned camera needs it explicitly.
    auto** target=Field<UObject*>(s.camera.p,"MyDressUpDarling");
    if(!target) { Close(a); return "Equipment preview camera binding is unavailable"; }
    *target=s.preview.p;
    if(auto** movement=Field<UObject*>(s.preview.p,"CharacterMovement");movement && *movement) Call(*movement,"DisableMovement").invoke();
    if(auto* cls=FindClass("PointLight")) {
        s.light.Set(a.SpawnActorAt(cls,{5150,5050,1200},{}));
        if(s.light.Live()) if(auto** component=Field<UObject*>(s.light.p,"PointLightComponent");component && *component)
            Call(*component,"SetIntensity").set<float>("NewIntensity",100000.f).invoke();
    }
    Call create(widgets,"Create"); create.set<UObject*>("WorldContextObject",context).set<UClass*>("WidgetType",editor).set<UObject*>("OwningPlayer",s.controller.p);
    s.editor.Set(create.invoke()?create.get<UObject*>("ReturnValue"):nullptr);
    if(!s.editor.Live() || !Call(s.editor.p,"AddToViewport").set<int32_t>("ZOrder",200).invoke()) {
        Close(a); return "Could not open the equipment editor";
    }
    s.kit=Kit(s.editor.p,Vis::Visible);
    if(auto* text=FindTextBlock(s.editor.p,12)) s.kit.FontFrom(text,"equipment editor");
    mp::Maker maker(&s.kit); s.back=maker.MakeFlat("BACK TO LOBBY",18);
    if(auto root=s.kit.FirstCanvas();root && s.back.card)
        root.Add(s.back.card.button).Anchor({{1,0},{1,0}},{1,0}).Offsets({-30,24,0,0}).AutoSize().Z(500);
    a.ShowPauseCursor();
    HSM_LOG("loadout-editor: UI_Tavern_Inventory opened directly with Inventory-tagged preview and camera");
    return {};
}
bool Tick(game::Adapter& a) {
    if(!s.active) return false;
    if(!s.editor.Live() || !InViewport(s.editor.p) || mp::Clicked(s.back.card)) { Close(a); return true; }
    mp::Paint(s.back.card); return false;
}
}
