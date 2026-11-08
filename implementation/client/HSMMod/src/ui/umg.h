// Widget kit (MATCH_ROUNDS_UI_CHAT_PLAN 3.1): the UMG widgets the mod builds at runtime for the match HUD, the chat and
// the multiplayer page, made and set through reflection. Every widget belongs to one user widget's tree (UI_HUD, the
// main menu) and lives and dies with it. Setters are diffed: a value a widget already has is not sent again, so a
// per-frame update costs nothing while nothing changes. Game thread only.
#pragma once
#include "ue/engine.h"
#include "match/hsm_match.h"
#include <cstring>
#include <string>
#include <vector>

namespace hsm::ui {

// Engine value types as UMG functions take them (UE 5.4: FVector2D is two doubles; Rgba is an FLinearColor).
struct Vec2 { double x = 0, y = 0; };
struct Anchors { Vec2 min, max; };
struct Margin { float left = 0, top = 0, right = 0, bottom = 0; };
struct ChildSize { float value = 1; uint8_t rule = 0; uint8_t pad[3] = {}; };   // FSlateChildSize; rule 0 Automatic, 1 Fill
static_assert(sizeof(ChildSize) == 8, "FSlateChildSize");
static_assert(sizeof(Rgba) == 16, "Rgba is passed as FLinearColor");

enum class Vis : uint8_t { Visible = 0, Collapsed = 1, Hidden = 2, HitTestInvisible = 3, SelfHitTestInvisible = 4 };   // ESlateVisibility
enum class HAlign : uint8_t { Fill = 0, Left = 1, Center = 2, Right = 3 };   // EHorizontalAlignment
enum class VAlign : uint8_t { Fill = 0, Top = 1, Center = 2, Bottom = 3 };   // EVerticalAlignment
enum class Justify : uint8_t { Left = 0, Center = 1, Right = 2 };            // ETextJustify

// The last value sent: Changed(v) is true (and remembers v) when v differs from it or nothing was sent yet. For plain
// value types without padding.
template <class T> struct Sent {
    T v{};
    bool known = false;
    bool Changed(const T& n) {
        if (known && std::memcmp(&v, &n, sizeof(T)) == 0) return false;
        v = n; known = true;
        return true;
    }
};

// Where a child sits in the panel that holds it. Set while building (not diffed).
class Slot {
public:
    explicit Slot(ue::UObject* s = nullptr) : s_(s) {}
    ue::UObject* get() const { return s_; }
    explicit operator bool() const { return s_ != nullptr; }
    // Box, overlay, grid, border and size box slots.
    Slot& Padding(Margin m);
    Slot& Align(HAlign h, VAlign v);
    Slot& Fill(float share = 1);              // a share of a row's or column's free space
    Slot& Auto();                             // the child's own size
    // Canvas slots.
    Slot& Anchor(Anchors a, Vec2 alignment);  // the canvas point the child sits at, and which point of the child sits there
    Slot& AutoSize(bool on = true);
    Slot& Z(int32_t z);
    Slot& Offsets(Margin m);
    Slot& Position(Vec2 p);
    Slot& Size(Vec2 s);
private:
    ue::UObject* s_;
};

class Widget {
public:
    Widget() = default;
    ue::UObject* get() const { return w_; }
    explicit operator bool() const { return w_ != nullptr; }
    void Show(bool on);                       // on: the visibility it was made with; off: Collapsed
    void Visibility(Vis v);
    void Opacity(float a);                    // render opacity, children included
    void Scale(float s);                      // render scale about the centre
    void Enabled(bool on);
protected:
    friend class Kit;
    void Bind(ue::UObject* w, Vis shown);
    void ApplyScale();
    ue::UObject* w_ = nullptr;
    Vis shown_ = Vis::HitTestInvisible;
    float scale_ = 1, scaleMul_ = 1;          // scaleMul_: a Text's stand-in for a font size it could not set
    Sent<uint8_t> vis_;
    Sent<float> opacity_;
    Sent<Vec2> scaleSent_;
    Sent<uint8_t> enabled_;
};

// TextBlock in the game's own typeface (the kit's copied font).
class Text : public Widget {
public:
    void Set(const std::string& s);
    const std::string& shown() const { return text_; }
    void Color(Rgba c);
    // Point size (0: the copied font's own) and an outline of `outline` px in `outlineColor`, patched into a copy of
    // the game's font. When the font layout cannot be resolved the size is approximated with render scale instead.
    void Font(float size, int32_t outline = 0, Rgba outlineColor = {0, 0, 0, 1});
    // Takes another TextBlock's font (copied through SetFont) and, when it is a plain specified colour, its colour. False
    // when it has no readable font.
    bool StyleFrom(ue::UObject* textBlock);
    void Shadow(Vec2 offset, Rgba color);
    void Align(Justify j);
    void Wrap(bool on);
private:
    friend class Kit;
    struct FontKey { float size; int32_t outline; Rgba color; };
    std::vector<uint8_t> font_;
    std::string text_;
    bool textKnown_ = false;
    Sent<Rgba> color_, shadowColor_;
    Sent<Vec2> shadow_;
    Sent<FontKey> fontSent_;
    Sent<uint8_t> justify_, wrap_;
};

// Border: a coloured panel around one child.
class Panel : public Widget {
public:
    Slot Content(const Widget& child);
    void Color(Rgba c);
    void Padding(Margin m);
    void ContentAlign(HAlign h, VAlign v);
private:
    Sent<Rgba> color_;
    Sent<Margin> padding_;
};

// BackgroundBlur: blurs what is behind it, around one child.
class Blur : public Widget {
public:
    Slot Content(const Widget& child);
    void Strength(float s);
    void Padding(Margin m);
private:
    Sent<float> strength_;
    Sent<Margin> padding_;
};

// Row (HorizontalBox), Column (VerticalBox), Stack (Overlay: children on top of each other), Scroll (ScrollBox).
class Box : public Widget {
public:
    enum class Kind : uint8_t { Row, Column, Stack, Scroll };
    Slot Add(const Widget& child);
    void ScrollToEnd();                       // a Scroll: show its last child
private:
    friend class Kit;
    Kind kind_ = Kind::Column;
};

// SizeBox: one child held to a width and height.
class Fixed : public Widget {
public:
    Slot Content(const Widget& child);
    void Width(float w);
    void Height(float h);
    void MinWidth(float w);
private:
    Sent<float> width_, height_, minWidth_;
};

// ScaleBox set to ScaleToFit, DownOnly: its child at its own size, shrunk (never grown) to fit the space it is given.
class Shrink : public Widget {
public:
    Slot Content(const Widget& child);
};

// UniformGridPanel: children in rows and columns of equal size.
class Grid : public Widget {
public:
    Slot Add(const Widget& child, int32_t row, int32_t column);
    void SlotPadding(Margin m);
    void MinSlotWidth(float w);
};

// The root CanvasPanel of a user widget.
class Canvas : public Widget {
public:
    Slot Add(const Widget& child);
};

// ProgressBar.
class Bar : public Widget {
public:
    void Percent(float p);                    // 0..1
    void FillColor(Rgba c);
private:
    Sent<float> percent_;
    Sent<Rgba> fill_;
};

// Image: a texture, or a solid colour block (a swatch, a frame line).
class Icon : public Widget {
public:
    void Color(Rgba c);
    void Size(Vec2 s);
    void Texture(ue::UObject* texture);
private:
    Sent<Rgba> color_;
    Sent<Vec2> size_;
    Sent<ue::UObject*> texture_;
};

// Button with one child (its label). Clicked() reports a release over the button, once.
class Button : public Widget {
public:
    Slot Content(const Widget& child);
    bool Clicked();
    bool Hovered() const { return hovered_; }   // under the mouse at the last Clicked() (no extra call)
    void Background(Rgba c);
    void StyleFrom(ue::UObject* button);      // copies another button's WidgetStyle (the game's own look)
private:
    bool wasPressed_ = false, hovered_ = false;
    Sent<Rgba> background_;
};

// EditableTextBox: a one-line text field the player types into (the game's own Slate text box).
class EditBox : public Widget {
public:
    std::string Value() const;                // what is typed now (empty when it cannot be read)
    void Clear();
    void Hint(const std::string& s);          // greyed text shown while empty
    void Set(const std::string& s);           // replaces what is typed
    bool Focused() const;                     // the player is typing in it (the keyboard is on the box or the field inside it)
    void Focus();                             // take the keyboard
};

// Slider: a bar with a handle the player drags (the game's own Slate slider). Range and Set are sent as asked (Set
// is not diffed: the player moves the handle, so the last value sent says nothing about where it is now).
class Slider : public Widget {
public:
    void Range(float min, float max, float step);
    void Set(float v);
    float Value() const;                      // where the handle is now (NaN when it cannot be read)
    bool Dragging() const;                    // the mouse holds the handle
    void Colors(Rgba bar, Rgba handle);
private:
    Sent<Vec2> range_;
    Sent<float> step_;
    Sent<Rgba> bar_, handle_;
};

// Makes widgets in one user widget's tree.
class Kit {
public:
    Kit() = default;
    // shown: the visibility widgets get when shown (HUD: HitTestInvisible, never taking clicks; menu: Visible).
    Kit(ue::UObject* userWidget, Vis shown);
    explicit operator bool() const { return tree_ != nullptr; }
    Canvas Root() const;                      // the tree's root when it is a CanvasPanel, else an empty Canvas
    Canvas FirstCanvas() const;               // the root canvas, else the first CanvasPanel under the root (breadth first)
    // Copies a TextBlock's font (the game's typeface) for every Text made after. False when it has none.
    bool FontFrom(ue::UObject* textBlock, const char* what);
    bool HasFont() const { return !font_.empty(); }
    Text MakeText();
    Panel MakePanel();
    Blur MakeBlur();
    Box MakeRow();
    Box MakeColumn();
    Box MakeStack();
    Box MakeScroll();
    Fixed MakeFixed();
    Shrink MakeShrink();
    Grid MakeGrid();
    Bar MakeBar();
    Icon MakeIcon();
    Button MakeButton();
    EditBox MakeEditBox();
    Slider MakeSlider();
private:
    ue::UObject* Spawn(ue::UClass* cls) const;
    template <class W> W Made(ue::UObject* o) const;
    Box MakeBox(ue::UClass* cls, Box::Kind kind);
    ue::UObject* tree_ = nullptr;
    Vis shown_ = Vis::HitTestInvisible;
    std::vector<uint8_t> font_;
};

// Probing the game's own widgets (to copy their look, and to log their structure once).
std::vector<ue::UObject*> ChildrenOf(ue::UObject* widget);    // a panel's children (a Button's content too), or a user widget's root
ue::UObject* FindTextBlock(ue::UObject* root, int depth);     // the first TextBlock at or under root, `depth` levels down
std::string Describe(ue::UObject* widget);                    // "Name : Class", plus a TextBlock's font/colour/text or a Button's brushes

bool InViewport(ue::UObject* userWidget);
// The first of `instances` that is on screen, or nullptr.
ue::UObject* OnScreen(const std::vector<ue::UObject*>& instances);

}  // namespace hsm::ui
