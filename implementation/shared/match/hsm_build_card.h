// The build card: a small terminal-style notice in the top-left corner of the match HUD that names a player, the build
// they launched with and the UE4SS mods they have on. Each player publishes it as Steam lobby member data, so it needs
// no protocol change. The text is peer-written: everything here treats it as untrusted, and the card reports what a
// client says about itself, so it informs honest players and is not anti-cheat. Game-independent.
#pragma once
#include "session/hsm_mod_rules.h"
#include "hsm_chat.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

constexpr size_t kMaxCardBuild = 24;     // bytes of a build name
constexpr size_t kMaxCardMods = 400;     // bytes of the mod list, the same bound as the bug report's list
constexpr size_t kMaxCardValue = kMaxCardBuild + 1 + kMaxCardMods;   // the lobby member data value
constexpr size_t kMaxCardQueue = 8;      // one card per player, and a lobby holds at most 8
constexpr uint32_t kCardRevealMs = 700;  // typed out over this long
constexpr uint32_t kCardHoldMs = 6000;   // then stays
constexpr uint32_t kCardFadeMs = 500;    // then fades

// One line of valid UTF-8 at most `maxBytes` long, cut on a code point. '_' reads as a space: a launcher passes the
// name on the command line, where a space would need quoting.
inline std::string CleanCardField(std::string_view in, size_t maxBytes) {
    std::string s(in);
    std::replace(s.begin(), s.end(), '_', ' ');
    s = SanitizeChat(s);
    if (s.size() > maxBytes) {
        size_t n = maxBytes;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
        s.resize(n);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// UE4SS's own mods say nothing about how a fighter plays, so the card leaves them out.
// "A, BPModLoaderMod, C" (ue4ss::ReportMods) to "A, C".
inline std::string CardModsFrom(std::string_view reportMods) {
    std::string out;
    size_t at = 0;
    while (at <= reportMods.size()) {
        size_t end = reportMods.find(", ", at);
        if (end == std::string_view::npos) end = reportMods.size();
        std::string_view name = reportMods.substr(at, end - at);
        if (!name.empty() && !IsStockUe4ssMod(name)) { if (!out.empty()) out += ", "; out += name; }
        at = end + 2;
    }
    return out;
}

// A mod list, each name cleaned on its own (the chat's cleaning stops at one chat line). What does not fit is counted.
inline std::string CleanCardMods(std::string_view in) {
    std::string out;
    size_t at = 0, dropped = 0;
    while (at <= in.size()) {
        size_t end = in.find(", ", at);
        if (end == std::string_view::npos) end = in.size();
        const std::string name = CleanCardField(in.substr(at, end - at), 64);
        if (!name.empty()) {
            if (out.size() + 2 + name.size() + 12 > kMaxCardMods) ++dropped;
            else { if (!out.empty()) out += ", "; out += name; }
        }
        at = end + 2;
    }
    if (dropped) out += " +" + std::to_string(dropped) + " more";
    return out;
}

struct BuildCard { std::string player, build, mods; };

// The lobby member data value: the build, a newline, the mods.
inline std::string EncodeCard(std::string_view build, std::string_view mods) {
    return CleanCardField(build, kMaxCardBuild) + "\n" + CleanCardMods(mods);
}

inline BuildCard DecodeCard(std::string_view player, std::string_view value) {
    BuildCard c;
    c.player = CleanCardField(player, 64);
    const size_t nl = value.find('\n');
    c.build = CleanCardField(value.substr(0, nl), kMaxCardBuild);
    if (nl != std::string_view::npos) c.mods = CleanCardMods(value.substr(nl + 1));
    return c;
}

// The first `n` code points of a string.
inline std::string CardPrefix(const std::string& s, size_t n) {
    size_t i = 0;
    while (i < s.size() && n > 0) {
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        --n;
    }
    return s.substr(0, i);
}

inline std::string CardUpper(std::string s) {
    for (char& ch : s) if (static_cast<unsigned char>(ch) < 0x80) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

struct BuildCardView {
    bool visible = false;
    float opacity = 0;
    bool typing = false;                      // still being typed out: the widget adds a caret
    std::string player, build, mods;          // the three lines, as far as they have been typed
};

// Cards wait until the match HUD can show them and then show one at a time. A player's newer card replaces one still
// waiting. Times are wrap-safe milliseconds.
class BuildCardQueue {
public:
    void Push(BuildCard c) {
        bool knownAlready = false;
        for (BuildCard& k : known_) if (k.player == c.player) { k = c; knownAlready = true; }
        if (!knownAlready && known_.size() < kMaxCardQueue) known_.push_back(c);
        for (BuildCard& w : waiting_) if (w.player == c.player) { w = std::move(c); return; }
        if (waiting_.size() >= kMaxCardQueue) waiting_.pop_front();
        waiting_.push_back(std::move(c));
    }
    void Clear() { waiting_.clear(); known_.clear(); showing_ = false; pinned_ = false; }
    bool Idle() const { return !showing_ && waiting_.empty() && !pinned_; }

    // The class checker key. Nothing showing: the first known player's card, held on screen. A card fading by itself: held
    // on screen. Already held: the next player's, and after the last one it goes away.
    void Toggle(uint32_t now) {
        if (pinned_) {
            ++pinIndex_;
            if (pinIndex_ >= known_.size()) { pinned_ = false; showing_ = false; return; }
            current_ = known_[pinIndex_];
            DropWaiting(current_.player);
            return;
        }
        if (showing_) {
            pinned_ = true;
            pinIndex_ = 0;
            for (size_t i = 0; i < known_.size(); ++i) if (known_[i].player == current_.player) pinIndex_ = i;
            startedAt_ = now - kCardRevealMs;
            return;
        }
        if (known_.empty()) return;
        pinned_ = true; showing_ = true; pinIndex_ = 0; current_ = known_[0];
        DropWaiting(current_.player);
        startedAt_ = now - kCardRevealMs;
    }

    // hudUp: the match HUD is on screen. A card's time only runs while it is, so none is spent on a loading screen.
    BuildCardView Tick(uint32_t now, bool hudUp) {
        BuildCardView v;
        if (!hudUp) { showing_ = false; pinned_ = false; return v; }
        if (!showing_) {
            if (waiting_.empty()) return v;
            current_ = std::move(waiting_.front());
            waiting_.pop_front();
            showing_ = true; startedAt_ = now;
        }
        const uint32_t t = pinned_ ? kCardRevealMs : now - startedAt_;
        if (t >= kCardRevealMs + kCardHoldMs + kCardFadeMs) { showing_ = false; return v; }
        v.visible = true;
        v.opacity = t < kCardRevealMs + kCardHoldMs ? 1.0f : 1.0f - float(t - kCardRevealMs - kCardHoldMs) / float(kCardFadeMs);
        const std::string player = "PLAYER: " + current_.player;
        const std::string build = "BUILD:  " + CardUpper(current_.build.empty() ? std::string("none") : current_.build);
        const std::string mods = "MODS:   " + (current_.mods.empty() ? std::string("none") : current_.mods);
        size_t left = t >= kCardRevealMs ? std::string::npos : size_t((player.size() + build.size() + mods.size()) * uint64_t(t) / kCardRevealMs);
        v.typing = left != std::string::npos;
        auto take = [&left](const std::string& s) {
            std::string out = CardPrefix(s, left);
            if (left != std::string::npos) left -= std::min(left, s.size());
            return out;
        };
        v.player = take(player); v.build = take(build); v.mods = take(mods);
        return v;
    }
private:
    void DropWaiting(const std::string& player) {   // this card has been seen: it won't come round again
        for (auto it = waiting_.begin(); it != waiting_.end();) it = it->player == player ? waiting_.erase(it) : it + 1;
    }
    std::deque<BuildCard> waiting_;
    std::vector<BuildCard> known_;     // every player's latest card, for the class checker key
    bool pinned_ = false;
    size_t pinIndex_ = 0;
    BuildCard current_;
    bool showing_ = false;
    uint32_t startedAt_ = 0;
};

}  // namespace hsm
