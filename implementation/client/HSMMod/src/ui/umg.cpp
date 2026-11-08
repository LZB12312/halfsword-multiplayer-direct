#include "ui/umg.h"
#include "util/hsm_names.h"
#include "log.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <utility>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

constexpr const char* kTextBlockClass = "TextBlock";
constexpr const char* kBorderClass = "Border";
constexpr const char* kBackgroundBlurClass = "BackgroundBlur";
constexpr const char* kHorizontalBoxClass = "HorizontalBox";
constexpr const char* kVerticalBoxClass = "VerticalBox";
constexpr const char* kOverlayClass = "Overlay";
constexpr const char* kScrollBoxClass = "ScrollBox";
constexpr const char* kSizeBoxClass = "SizeBox";
constexpr const char* kScaleBoxClass = "ScaleBox";
constexpr uint8_t kStretchScaleToFit = 2;   // EStretch::ScaleToFit
constexpr uint8_t kStretchDownOnly = 1;     // EStretchDirection::DownOnly
constexpr const char* kUniformGridPanelClass = "UniformGridPanel";
constexpr const char* kProgressBarClass = "ProgressBar";
constexpr const char* kImageClass = "Image";
constexpr const char* kButtonClass = "Button";
constexpr const char* kEditBoxClass = "EditableTextBox";
constexpr const char* kSliderClass = "Slider";

// A TextBlock's font size when the copied font's own cannot be read (UMG's default).
constexpr float kAssumedFontSize = 24;

// UMG classes are native and never unloaded; each is found once (an object array scan) and checked by slot after.
UClass* WidgetClass(const char* name) {
    static std::unordered_map<std::string, std::pair<UClass*, int32_t>> cache;
    auto& [cls, index] = cache[name];
    if (cls && ObjectAt(index) == reinterpret_cast<UObject*>(cls)) return cls;
    cls = FindClass(name);
    index = cls ? ObjectIndex(reinterpret_cast<UObject*>(cls)) : -1;
    return cls;
}

// Where FSlateFontInfo keeps the size and the outline, from the runtime reflection of its script structs (never
// hard-coded). Resolved once, logged once.
struct FontLayout { bool ok = false; size_t size = 0, outlineSize = 0, outlineColor = 0, end = 0; };

const FontLayout& Fonts() {
    static FontLayout layout;
    static bool tried = false;
    if (tried) return layout;
    tried = true;
    UObject* fontInfo = FindStruct("SlateFontInfo");
    UObject* outline = FindStruct("FontOutlineSettings");
    FProperty* size = StructMember(fontInfo, "Size");
    FProperty* settings = StructMember(fontInfo, "OutlineSettings");
    FProperty* outlineSize = StructMember(outline, "OutlineSize");
    FProperty* outlineColor = StructMember(outline, "OutlineColor");
    if (size && settings && outlineSize && outlineColor && PropertySize(size) == 4 && PropertySize(outlineSize) == 4 &&
        PropertySize(outlineColor) == int32_t(sizeof(Rgba))) {
        layout.ok = true;
        layout.size = size_t(PropertyOffset(size));
        layout.outlineSize = size_t(PropertyOffset(settings) + PropertyOffset(outlineSize));
        layout.outlineColor = size_t(PropertyOffset(settings) + PropertyOffset(outlineColor));
        layout.end = std::max({layout.size + 4, layout.outlineSize + 4, layout.outlineColor + sizeof(Rgba)});
        HSM_LOG("umg: font layout: Size +0x%zx, OutlineSettings.OutlineSize +0x%zx, OutlineSettings.OutlineColor +0x%zx",
                layout.size, layout.outlineSize, layout.outlineColor);
    } else {
        HSM_LOG("umg: font layout not resolved (SlateFontInfo %s, FontOutlineSettings %s); text sizes use render scale, no outlines",
                fontInfo ? "found" : "missing", outline ? "found" : "missing");
    }
    return layout;
}

// Where FSlateColor keeps its colour and its use rule (the TextBlock colour parameter is one).
struct ColorLayout { bool ok = false; size_t color = 0, rule = 0; };

const ColorLayout& SlateColors() {
    static ColorLayout layout;
    static bool tried = false;
    if (tried) return layout;
    tried = true;
    UObject* slateColor = FindStruct("SlateColor");
    FProperty* color = StructMember(slateColor, "SpecifiedColor");
    FProperty* rule = StructMember(slateColor, "ColorUseRule");
    if (color && rule && PropertySize(color) == int32_t(sizeof(Rgba)) && PropertySize(rule) == 1) {
        layout.ok = true;
        layout.color = size_t(PropertyOffset(color));
        layout.rule = size_t(PropertyOffset(rule));
    } else {
        HSM_LOG("umg: SlateColor layout not resolved; text keeps its colour");
    }
    return layout;
}

// A TextBlock's FSlateFontInfo as raw bytes, for SetFont on another block (the engine copies it into the new widget; a
// struct holding a TSharedPtr is never memcpy'd into a widget's own field).
bool ReadFont(UObject* textBlock, std::vector<uint8_t>& out) {
    void* font = textBlock ? FieldPtr(textBlock, "Font") : nullptr;
    FProperty* p = font ? FindProperty(ClassOf(textBlock), "Font") : nullptr;
    if (!p || PropertySize(p) <= 0) return false;
    out.assign(static_cast<uint8_t*>(font), static_cast<uint8_t*>(font) + PropertySize(p));
    return true;
}

float FontSize(const std::vector<uint8_t>& font) {
    const FontLayout& layout = Fonts();
    float size = 0;
    if (layout.ok && layout.end <= font.size()) std::memcpy(&size, font.data() + layout.size, sizeof size);
    return size;
}

// A TextBlock's or an EditableTextBox's text, as UTF-8.
std::string ReadText(UObject* widget) {
    static UObject* lib = nullptr;
    if (!lib) if (UObject* cls = FindObject("KismetTextLibrary", "Class")) lib = DefaultObject(reinterpret_cast<UClass*>(cls));
    Call get(widget, "GetText");
    if (!get.invoke()) return {};
    const FText text = get.get<FText>("ReturnValue");
    Call str(lib, "Conv_TextToString");
    str.set("InText", text);
    if (!str.invoke()) return {};
    return ReadFString(str.paramPtr("ReturnValue"));      // the Call frees the engine's string with its buffer
}

// An FSlateColor living at `base`, when it is a plain specified colour.
bool ReadSlateColor(const uint8_t* base, Rgba& out) {
    const ColorLayout& sc = SlateColors();
    if (!sc.ok || !base || base[sc.rule] != 0) return false;   // 0: ESlateColorStylingMode::UseColor_Specified
    std::memcpy(&out, base + sc.color, sizeof out);
    return true;
}

std::string ColorText(Rgba c) {
    char b[80];
    std::snprintf(b, sizeof b, "(%.2f %.2f %.2f %.2f)", double(c.r), double(c.g), double(c.b), double(c.a));
    return b;
}

// One state brush of a ButtonStyle: what it draws, how big, in what tint, with which resource.
std::string DescribeBrush(const uint8_t* style, const char* member) {
    UObject* buttonStyle = FindStruct("ButtonStyle");
    UObject* brushStruct = FindStruct("SlateBrush");
    FProperty* m = StructMember(buttonStyle, member);
    if (!m) return std::string(member) + " ?";
    const uint8_t* brush = style + PropertyOffset(m);
    std::string out = member;
    FProperty* draw = StructMember(brushStruct, "DrawAs");
    FProperty* size = StructMember(brushStruct, "ImageSize");
    FProperty* tint = StructMember(brushStruct, "TintColor");
    FProperty* res = StructMember(brushStruct, "ResourceObject");
    char b[96];
    if (draw && PropertySize(draw) == 1) { std::snprintf(b, sizeof b, " draw=%d", int(brush[PropertyOffset(draw)])); out += b; }
    if (size && PropertySize(size) == 8) {
        float xy[2]; std::memcpy(xy, brush + PropertyOffset(size), sizeof xy);
        std::snprintf(b, sizeof b, " size=%gx%g", double(xy[0]), double(xy[1])); out += b;
    } else if (size && PropertySize(size) == 16) {
        double xy[2]; std::memcpy(xy, brush + PropertyOffset(size), sizeof xy);
        std::snprintf(b, sizeof b, " size=%gx%g", xy[0], xy[1]); out += b;
    }
    Rgba t;
    if (tint && ReadSlateColor(brush + PropertyOffset(tint), t)) out += " tint=" + ColorText(t);
    if (res && PropertySize(res) == int32_t(sizeof(UObject*))) {
        UObject* r = nullptr; std::memcpy(&r, brush + PropertyOffset(res), sizeof r);
        out += " res=" + (r ? ObjectName(r) : std::string("none"));
    }
    return out;
}

}  // namespace

// ------------------------------------------------------------------ slots
Slot& Slot::Padding(Margin m) { Call(s_, "SetPadding").set("InPadding", m).invoke(); return *this; }
Slot& Slot::Align(HAlign h, VAlign v) {
    Call(s_, "SetHorizontalAlignment").set<uint8_t>("InHorizontalAlignment", uint8_t(h)).invoke();
    Call(s_, "SetVerticalAlignment").set<uint8_t>("InVerticalAlignment", uint8_t(v)).invoke();
    return *this;
}
Slot& Slot::Fill(float share) { Call(s_, "SetSize").set("InSize", ChildSize{share, 1}).invoke(); return *this; }
Slot& Slot::Auto() { Call(s_, "SetSize").set("InSize", ChildSize{1, 0}).invoke(); return *this; }
Slot& Slot::Anchor(Anchors a, Vec2 alignment) {
    Call(s_, "SetAnchors").set("InAnchors", a).invoke();
    Call(s_, "SetAlignment").set("InAlignment", alignment).invoke();
    return *this;
}
Slot& Slot::AutoSize(bool on) { Call(s_, "SetAutoSize").set<bool>("InbAutoSize", on).invoke(); return *this; }
Slot& Slot::Z(int32_t z) { Call(s_, "SetZOrder").set<int32_t>("InZOrder", z).invoke(); return *this; }
Slot& Slot::Offsets(Margin m) { Call(s_, "SetOffsets").set("InOffset", m).invoke(); return *this; }
Slot& Slot::Position(Vec2 p) { Call(s_, "SetPosition").set("InPosition", p).invoke(); return *this; }
Slot& Slot::Size(Vec2 s) { Call(s_, "SetSize").set("InSize", s).invoke(); return *this; }

// ------------------------------------------------------------------ widgets
void Widget::Bind(UObject* w, Vis shown) { w_ = w; shown_ = shown; Visibility(shown); }

void Widget::Show(bool on) { Visibility(on ? shown_ : Vis::Collapsed); }

void Widget::Visibility(Vis v) {
    if (!w_ || !vis_.Changed(uint8_t(v))) return;
    Call(w_, "SetVisibility").set<uint8_t>("InVisibility", uint8_t(v)).invoke();
}

void Widget::Opacity(float a) {
    if (!w_ || !opacity_.Changed(a)) return;
    Call(w_, "SetRenderOpacity").set<float>("InOpacity", a).invoke();
}

void Widget::Scale(float s) { scale_ = s; ApplyScale(); }

void Widget::ApplyScale() {
    const double s = double(scale_) * scaleMul_;
    if (!w_ || !scaleSent_.Changed(Vec2{s, s})) return;
    Call(w_, "SetRenderScale").set("Scale", Vec2{s, s}).invoke();
}

void Widget::Enabled(bool on) {
    if (!w_ || !enabled_.Changed(uint8_t(on))) return;
    Call(w_, "SetIsEnabled").set<bool>("bInIsEnabled", on).invoke();
}

void Text::Set(const std::string& s) {
    if (!w_ || (textKnown_ && s == text_)) return;
    text_ = s; textKnown_ = true;
    FText t = MakeText(s);
    Call(w_, "SetText").set("InText", t).invoke();
}

void Text::Color(Rgba c) {
    if (!w_ || !color_.Changed(c)) return;
    const ColorLayout& layout = SlateColors();
    if (!layout.ok) return;
    Call call(w_, "SetColorAndOpacity");
    FProperty* p = call.function() ? FindProperty(call.function(), "InColorAndOpacity") : nullptr;
    auto* v = static_cast<uint8_t*>(call.paramPtr("InColorAndOpacity"));
    if (!p || !v || size_t(PropertySize(p)) < std::max(layout.color + sizeof(Rgba), layout.rule + 1)) return;
    std::memcpy(v + layout.color, &c, sizeof c);
    v[layout.rule] = 0;   // ESlateColorStylingMode::UseColor_Specified
    call.invoke();
}

void Text::Font(float size, int32_t outline, Rgba outlineColor) {
    if (!w_ || !fontSent_.Changed(FontKey{size, outline, outlineColor})) return;
    bool patched = false;
    if (!font_.empty()) {
        const FontLayout& layout = Fonts();
        std::vector<uint8_t> f = font_;
        patched = layout.ok && layout.end <= f.size();
        if (patched) {
            if (size > 0) std::memcpy(f.data() + layout.size, &size, sizeof size);
            std::memcpy(f.data() + layout.outlineSize, &outline, sizeof outline);
            std::memcpy(f.data() + layout.outlineColor, &outlineColor, sizeof outlineColor);
        }
        Call c(w_, "SetFont");
        FProperty* p = c.function() ? FindProperty(c.function(), "InFontInfo") : nullptr;
        if (p && size_t(PropertySize(p)) == f.size()) {
            std::memcpy(c.paramPtr("InFontInfo"), f.data(), f.size());   // the engine copies it
            c.invoke();
        }
    }
    const float mul = patched || size <= 0 ? 1.f : size / kAssumedFontSize;   // blurry but the right size
    if (mul != scaleMul_) { scaleMul_ = mul; ApplyScale(); }
}

bool Text::StyleFrom(UObject* textBlock) {
    std::vector<uint8_t> f;
    if (!w_ || !ReadFont(textBlock, f)) return false;
    Call c(w_, "SetFont");
    FProperty* p = c.function() ? FindProperty(c.function(), "InFontInfo") : nullptr;
    if (!p || size_t(PropertySize(p)) != f.size()) return false;
    std::memcpy(c.paramPtr("InFontInfo"), f.data(), f.size());   // the engine copies it
    c.invoke();
    font_ = f;
    fontSent_ = Sent<FontKey>{};
    scaleMul_ = 1; ApplyScale();
    if (void* col = FieldPtr(textBlock, "ColorAndOpacity")) {
        Rgba rgba;
        if (ReadSlateColor(static_cast<uint8_t*>(col), rgba)) Color(rgba);
    }
    return true;
}

void Text::Shadow(Vec2 offset, Rgba color) {
    if (!w_) return;
    if (shadow_.Changed(offset)) Call(w_, "SetShadowOffset").set("InShadowOffset", offset).invoke();
    if (shadowColor_.Changed(color)) Call(w_, "SetShadowColorAndOpacity").set("InShadowColorAndOpacity", color).invoke();
}

void Text::Align(Justify j) {
    if (!w_ || !justify_.Changed(uint8_t(j))) return;
    Call(w_, "SetJustification").set<uint8_t>("InJustification", uint8_t(j)).invoke();
}

void Text::Wrap(bool on) {
    if (!w_ || !wrap_.Changed(uint8_t(on))) return;
    Call(w_, "SetAutoWrapText").set<bool>("InAutoTextWrap", on).invoke();
}

Slot Panel::Content(const Widget& child) {
    Call c(w_, "SetContent"); c.set<UObject*>("Content", child.get()).invoke();
    return Slot(c.get<UObject*>("ReturnValue"));
}
void Panel::Color(Rgba c) { if (w_ && color_.Changed(c)) Call(w_, "SetBrushColor").set("InBrushColor", c).invoke(); }
void Panel::Padding(Margin m) { if (w_ && padding_.Changed(m)) Call(w_, "SetPadding").set("InPadding", m).invoke(); }
void Panel::ContentAlign(HAlign h, VAlign v) {
    Call(w_, "SetHorizontalAlignment").set<uint8_t>("InHorizontalAlignment", uint8_t(h)).invoke();
    Call(w_, "SetVerticalAlignment").set<uint8_t>("InVerticalAlignment", uint8_t(v)).invoke();
}

Slot Blur::Content(const Widget& child) {
    Call c(w_, "SetContent"); c.set<UObject*>("Content", child.get()).invoke();
    return Slot(c.get<UObject*>("ReturnValue"));
}
void Blur::Strength(float s) { if (w_ && strength_.Changed(s)) Call(w_, "SetBlurStrength").set<float>("InStrength", s).invoke(); }
void Blur::Padding(Margin m) { if (w_ && padding_.Changed(m)) Call(w_, "SetPadding").set("InPadding", m).invoke(); }

Slot Box::Add(const Widget& child) {
    UObject* slot = nullptr;
    switch (kind_) {
        case Kind::Row: { Call c(w_, "AddChildToHorizontalBox"); c.set<UObject*>("Content", child.get()).invoke(); slot = c.get<UObject*>("ReturnValue"); break; }
        case Kind::Column: { Call c(w_, "AddChildToVerticalBox"); c.set<UObject*>("Content", child.get()).invoke(); slot = c.get<UObject*>("ReturnValue"); break; }
        case Kind::Stack: { Call c(w_, "AddChildToOverlay"); c.set<UObject*>("Content", child.get()).invoke(); slot = c.get<UObject*>("ReturnValue"); break; }
        case Kind::Scroll: { Call c(w_, "AddChild"); c.set<UObject*>("Content", child.get()).invoke(); slot = c.get<UObject*>("ReturnValue"); break; }
    }
    return Slot(slot);
}

void Box::ScrollToEnd() { if (w_ && kind_ == Kind::Scroll) Call(w_, "ScrollToEnd").invoke(); }

Slot Fixed::Content(const Widget& child) {
    Call c(w_, "SetContent"); c.set<UObject*>("Content", child.get()).invoke();
    return Slot(c.get<UObject*>("ReturnValue"));
}
void Fixed::Width(float w) { if (w_ && width_.Changed(w)) Call(w_, "SetWidthOverride").set<float>("InWidthOverride", w).invoke(); }
void Fixed::Height(float h) { if (w_ && height_.Changed(h)) Call(w_, "SetHeightOverride").set<float>("InHeightOverride", h).invoke(); }
void Fixed::MinWidth(float w) { if (w_ && minWidth_.Changed(w)) Call(w_, "SetMinDesiredWidth").set<float>("InMinDesiredWidth", w).invoke(); }

Slot Shrink::Content(const Widget& child) {
    Call c(w_, "SetContent"); c.set<UObject*>("Content", child.get()).invoke();
    return Slot(c.get<UObject*>("ReturnValue"));
}

Slot Grid::Add(const Widget& child, int32_t row, int32_t column) {
    Call c(w_, "AddChildToUniformGrid");
    c.set<UObject*>("Content", child.get()).set<int32_t>("InRow", row).set<int32_t>("InColumn", column).invoke();
    return Slot(c.get<UObject*>("ReturnValue"));
}
void Grid::SlotPadding(Margin m) { Call(w_, "SetSlotPadding").set("InSlotPadding", m).invoke(); }
void Grid::MinSlotWidth(float w) { Call(w_, "SetMinDesiredSlotWidth").set<float>("InMinDesiredSlotWidth", w).invoke(); }

Slot Canvas::Add(const Widget& child) {
    Call c(w_, "AddChildToCanvas"); c.set<UObject*>("Content", child.get()).invoke();
    return Slot(c.get<UObject*>("ReturnValue"));
}

void Bar::Percent(float p) { if (w_ && percent_.Changed(p)) Call(w_, "SetPercent").set<float>("InPercent", p).invoke(); }
void Bar::FillColor(Rgba c) { if (w_ && fill_.Changed(c)) Call(w_, "SetFillColorAndOpacity").set("InColor", c).invoke(); }

void Icon::Color(Rgba c) { if (w_ && color_.Changed(c)) Call(w_, "SetColorAndOpacity").set("InColorAndOpacity", c).invoke(); }
void Icon::Size(Vec2 s) { if (w_ && size_.Changed(s)) Call(w_, "SetDesiredSizeOverride").set("DesiredSize", s).invoke(); }
void Icon::Texture(UObject* texture) {
    if (!w_ || !texture || !texture_.Changed(texture)) return;
    Call(w_, "SetBrushFromTexture").set<UObject*>("Texture", texture).set<bool>("bMatchSize", false).invoke();
}

Slot Button::Content(const Widget& child) {
    Call c(w_, "SetContent"); c.set<UObject*>("Content", child.get()).invoke();
    return Slot(c.get<UObject*>("ReturnValue"));
}

bool Button::Clicked() {
    if (!w_) return false;
    Call p(w_, "IsPressed"); p.invoke();
    const bool pressed = p.get<bool>("ReturnValue");
    Call h(w_, "IsHovered"); h.invoke();
    hovered_ = h.get<bool>("ReturnValue");
    const bool clicked = wasPressed_ && !pressed && hovered_;
    wasPressed_ = pressed;
    return clicked;
}

void Button::Background(Rgba c) { if (w_ && background_.Changed(c)) Call(w_, "SetBackgroundColor").set("InBackgroundColor", c).invoke(); }

void Button::StyleFrom(UObject* button) {
    void* style = w_ && button ? FieldPtr(button, "WidgetStyle") : nullptr;
    if (!style) return;
    Call c(w_, "SetStyle");
    if (!c.function() || !c.paramPtr("InStyle")) return;
    c.setFrom("InStyle", style);   // a deep copy by the engine: the style's brushes hold shared pointers
    c.invoke();
}

// ------------------------------------------------------------------ kit
Kit::Kit(UObject* userWidget, Vis shown) : shown_(shown) {
    UObject** t = userWidget ? Field<UObject*>(userWidget, "WidgetTree") : nullptr;
    tree_ = t ? *t : nullptr;
}

Canvas Kit::Root() const {
    Canvas c;
    UObject** rootPtr = tree_ ? Field<UObject*>(tree_, "RootWidget") : nullptr;
    UObject* root = rootPtr ? *rootPtr : nullptr;
    if (root && SameNameAscii(ObjectName(reinterpret_cast<UObject*>(ClassOf(root))), "CanvasPanel")) c.w_ = root;   // its visibility stays the game's
    return c;
}

Canvas Kit::FirstCanvas() const {
    Canvas c = Root();
    if (c) return c;
    UObject** rootPtr = tree_ ? Field<UObject*>(tree_, "RootWidget") : nullptr;
    std::vector<UObject*> queue;
    if (rootPtr && *rootPtr) queue.push_back(*rootPtr);
    for (size_t i = 0; i < queue.size() && i < 200; ++i) {
        UObject* w = queue[i];
        if (SameNameAscii(ObjectName(reinterpret_cast<UObject*>(ClassOf(w))), "CanvasPanel")) { c.w_ = w; return c; }
        for (UObject* child : ChildrenOf(w)) if (child) queue.push_back(child);
    }
    return c;
}

bool Kit::FontFrom(UObject* textBlock, const char* what) {
    if (!ReadFont(textBlock, font_)) return false;
    HSM_LOG("umg: font copied from %s (%zu bytes, size %.1f)", what, font_.size(), double(FontSize(font_)));
    return true;
}

UObject* Kit::Spawn(UClass* cls) const { return tree_ && cls ? SpawnObject(cls, tree_) : nullptr; }

template <class W> W Kit::Made(UObject* o) const {
    W w;
    if (o) w.Bind(o, shown_);
    return w;
}

Text Kit::MakeText() {
    Text t = Made<Text>(Spawn(WidgetClass(kTextBlockClass)));
    if (t && !font_.empty()) {
        t.font_ = font_;
        Call c(t.w_, "SetFont");   // the copied font as it is, until Font() patches it
        FProperty* p = c.function() ? FindProperty(c.function(), "InFontInfo") : nullptr;
        if (p && size_t(PropertySize(p)) == font_.size()) { std::memcpy(c.paramPtr("InFontInfo"), font_.data(), font_.size()); c.invoke(); }
    }
    return t;
}

Box Kit::MakeBox(UClass* cls, Box::Kind kind) { Box b = Made<Box>(Spawn(cls)); b.kind_ = kind; return b; }
Panel Kit::MakePanel() { return Made<Panel>(Spawn(WidgetClass(kBorderClass))); }
Blur Kit::MakeBlur() { return Made<Blur>(Spawn(WidgetClass(kBackgroundBlurClass))); }
Box Kit::MakeRow() { return MakeBox(WidgetClass(kHorizontalBoxClass), Box::Kind::Row); }
Box Kit::MakeColumn() { return MakeBox(WidgetClass(kVerticalBoxClass), Box::Kind::Column); }
Box Kit::MakeStack() { return MakeBox(WidgetClass(kOverlayClass), Box::Kind::Stack); }
Box Kit::MakeScroll() { return MakeBox(WidgetClass(kScrollBoxClass), Box::Kind::Scroll); }
Fixed Kit::MakeFixed() { return Made<Fixed>(Spawn(WidgetClass(kSizeBoxClass))); }
Shrink Kit::MakeShrink() {
    Shrink s = Made<Shrink>(Spawn(WidgetClass(kScaleBoxClass)));
    if (s) {
        Call(s.get(), "SetStretch").set<uint8_t>("InStretch", kStretchScaleToFit).invoke();
        Call(s.get(), "SetStretchDirection").set<uint8_t>("InStretchDirection", kStretchDownOnly).invoke();
    }
    return s;
}
Grid Kit::MakeGrid() { return Made<Grid>(Spawn(WidgetClass(kUniformGridPanelClass))); }
Bar Kit::MakeBar() { return Made<Bar>(Spawn(WidgetClass(kProgressBarClass))); }
Icon Kit::MakeIcon() { return Made<Icon>(Spawn(WidgetClass(kImageClass))); }
Button Kit::MakeButton() { return Made<Button>(Spawn(WidgetClass(kButtonClass))); }
// The stock EditableTextBox is a light grey field with grey hint text, hard to read on the dark menu: its WidgetStyle
// gets a near-black field and bone text (the hint is drawn from the same colour, faded). It
// is written before the Slate widget exists (the box is in no tree yet), so the first build already uses it.
EditBox Kit::MakeEditBox() {
    EditBox e = Made<EditBox>(Spawn(WidgetClass(kEditBoxClass)));
    if (!e) return e;
    static bool logged = false;
    const ColorLayout& sc = SlateColors();
    uint8_t* ws = sc.ok ? static_cast<uint8_t*>(FieldPtr(e.w_, "WidgetStyle")) : nullptr;
    UObject* boxStyle = FindStruct("EditableTextBoxStyle");
    UObject* textStyle = FindStruct("TextBlockStyle");
    if (!ws || !boxStyle) { if (!logged) { logged = true; HSM_LOG("umg: text box style not found; the stock look stays"); } return e; }
    int painted = 0;
    auto paint = [&](size_t at, const FProperty* p, Rgba c) {
        if (!p || size_t(PropertySize(p)) < sc.color + sizeof(Rgba) || size_t(PropertySize(p)) <= sc.rule) return;
        std::memcpy(ws + at + sc.color, &c, sizeof c);
        ws[at + sc.rule] = 0;                                  // ESlateColorStylingMode::UseColor_Specified
        ++painted;
    };
    const Rgba field{0.016f, 0.014f, 0.012f, 1}, ink{0.93f, 0.89f, 0.80f, 1};   // style::kBone
    for (FProperty* p : {StructMember(boxStyle, "BackgroundColor")}) if (p) paint(size_t(PropertyOffset(p)), p, field);
    for (FProperty* p : {StructMember(boxStyle, "ForegroundColor"), StructMember(boxStyle, "FocusedForegroundColor")}) if (p) paint(size_t(PropertyOffset(p)), p, ink);
    if (FProperty* ts = StructMember(boxStyle, "TextStyle")) {
        const size_t base = size_t(PropertyOffset(ts));
        if (FProperty* c = StructMember(textStyle, "ColorAndOpacity")) paint(base + size_t(PropertyOffset(c)), c, ink);
    }
    if (!logged) { logged = true; HSM_LOG("umg: text box restyled (%d colour(s) set)", painted); }
    return e;
}

std::string EditBox::Value() const { return w_ ? ReadText(w_) : std::string(); }

void EditBox::Clear() { if (w_) Call(w_, "SetText").set("InText", MakeText("")).invoke(); }
// The keyboard sits on the Slate field inside the box, so the box itself is not the answer (HasKeyboardFocus): any
// focused descendant is.
bool EditBox::Focused() const {
    if (!w_) return false;
    Call c(w_, "HasFocusedDescendants");
    if (c.invoke() && c.get<bool>("ReturnValue")) return true;
    Call k(w_, "HasKeyboardFocus");
    return k.invoke() && k.get<bool>("ReturnValue");
}
void EditBox::Focus() { if (w_) Call(w_, "SetKeyboardFocus").invoke(); }
void EditBox::Hint(const std::string& s) { if (w_) Call(w_, "SetHintText").set("InText", MakeText(s)).invoke(); }
void EditBox::Set(const std::string& s) { if (w_) Call(w_, "SetText").set("InText", MakeText(s)).invoke(); }

Slider Kit::MakeSlider() { return Made<Slider>(Spawn(WidgetClass(kSliderClass))); }
void Slider::Range(float min, float max, float step) {
    if (!w_) return;
    if (range_.Changed(Vec2{min, max})) {
        Call(w_, "SetMinValue").set<float>("InValue", min).invoke();
        Call(w_, "SetMaxValue").set<float>("InValue", max).invoke();
    }
    if (step_.Changed(step)) Call(w_, "SetStepSize").set<float>("InValue", step).invoke();
}
void Slider::Set(float v) { if (w_) Call(w_, "SetValue").set<float>("InValue", v).invoke(); }
float Slider::Value() const {
    if (!w_) return 0;
    Call c(w_, "GetValue");
    return c.invoke() ? c.get<float>("ReturnValue") : std::numeric_limits<float>::quiet_NaN();
}
bool Slider::Dragging() const {
    if (!w_) return false;
    Call c(w_, "HasMouseCapture");
    return c.invoke() && c.get<bool>("ReturnValue");
}
void Slider::Colors(Rgba bar, Rgba handle) {
    if (!w_) return;
    if (bar_.Changed(bar)) Call(w_, "SetSliderBarColor").set("InValue", bar).invoke();
    if (handle_.Changed(handle)) Call(w_, "SetSliderHandleColor").set("InValue", handle).invoke();
}

bool InViewport(UObject* userWidget) {
    Call v(userWidget, "IsInViewport"); v.invoke();
    return v.get<bool>("ReturnValue");
}

UObject* OnScreen(const std::vector<UObject*>& instances) {
    for (UObject* o : instances) if (InViewport(o)) return o;
    return nullptr;
}


// ------------------------------------------------------------------ probing the game's own widgets
std::vector<UObject*> ChildrenOf(UObject* widget) {
    std::vector<UObject*> out;
    if (!widget) return out;
    if (UObject** tree = Field<UObject*>(widget, "WidgetTree"); tree && *tree)       // a user widget: its tree's root
        if (UObject** root = Field<UObject*>(*tree, "RootWidget"); root && *root) out.push_back(*root);
    Call n(widget, "GetChildrenCount");                                                  // a panel (a Button included)
    if (!n.invoke()) return out;
    const int32_t count = std::min(n.get<int32_t>("ReturnValue"), 64);
    for (int32_t i = 0; i < count; ++i) {
        Call c(widget, "GetChildAt");
        c.set<int32_t>("Index", i);
        if (c.invoke())
            if (UObject* child = c.get<UObject*>("ReturnValue")) out.push_back(child);
    }
    return out;
}

UObject* FindTextBlock(UObject* root, int depth) {
    UClass* tb = WidgetClass(kTextBlockClass);
    if (!root || !tb) return nullptr;
    if (IsA(root, tb)) return root;
    if (depth <= 0) return nullptr;
    for (UObject* child : ChildrenOf(root))
        if (UObject* found = FindTextBlock(child, depth - 1)) return found;
    return nullptr;
}

std::string Describe(UObject* widget) {
    if (!widget) return "(null)";
    std::string out = ObjectName(widget) + " : " + ObjectName(reinterpret_cast<UObject*>(ClassOf(widget)));
    if (UClass* tb = WidgetClass(kTextBlockClass); tb && IsA(widget, tb)) {
        std::vector<uint8_t> f;
        if (ReadFont(widget, f)) { char b[48]; std::snprintf(b, sizeof b, " font=%.1f", double(FontSize(f))); out += b; }
        Rgba c;
        if (void* col = FieldPtr(widget, "ColorAndOpacity"); col && ReadSlateColor(static_cast<uint8_t*>(col), c)) out += " colour=" + ColorText(c);
        out += " text='" + ReadText(widget) + "'";
    } else if (UClass* bt = WidgetClass(kButtonClass); bt && IsA(widget, bt)) {
        const uint8_t* style = static_cast<const uint8_t*>(FieldPtr(widget, "WidgetStyle"));
        if (style)
            for (const char* state : {"Normal", "Hovered", "Pressed"}) out += " [" + DescribeBrush(style, state) + "]";
        if (const void* bg = FieldPtr(widget, "BackgroundColor")) out += " background=" + ColorText(*static_cast<const Rgba*>(bg));
    }
    return out;
}

}  // namespace hsm::ui
