// Names players choose and other players read: slurs are refused, however they're spelled. Names are plain ASCII by the time they get
// here (hsm_class.h), so lookalike letters can't get round it. The launcher has its own copy of the lists (Companion.ps1), for the
// message only: this one decides. Game-independent.
#pragma once
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hsm {

// Blocked even inside other words or spaced out. Only words that no ordinary word contains.
inline constexpr std::array<std::string_view, 15> kSlursAnywhere = {
    "nigger", "nigga", "faggot", "fagot", "wetback", "towelhead", "raghead", "zipperhead", "tranny", "trannie",
    "shemale", "retard", "mongoloid", "kaffir", "junglebunny"};
// Blocked as whole words, because ordinary words contain them (raccoon, spice).
inline constexpr std::array<std::string_view, 25> kSlursWord = {
    "fag", "dyke", "tard", "kike", "spic", "spick", "chink", "gook", "coon", "negro", "negress", "nig", "niga",
    "jigaboo", "beaner", "paki", "wop", "heeb", "homo", "lesbo", "injun", "redskin", "squaw", "darkie", "darky"};

// Lower case letters, with the numbers people type for letters; anything else splits words.
inline char NameFilterLetter(char c) {
    switch (c) {
        case '0': return 'o'; case '1': return 'i'; case '3': return 'e'; case '4': return 'a';
        case '5': return 's'; case '7': return 't'; case '8': return 'b'; case '9': return 'g';
        default: break;
    }
    return std::isalpha(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '\0';
}

// Does `word` appear in `text`, where a doubled letter may be stretched (cooon) but not shortened (con isn't coon)?
inline std::vector<std::pair<char, size_t>> NameFilterRuns(std::string_view s) {
    std::vector<std::pair<char, size_t>> runs;
    for (char c : s) { if (!runs.empty() && runs.back().first == c) ++runs.back().second; else runs.push_back({c, 1}); }
    return runs;
}
inline bool NameFilterMatch(std::string_view text, std::string_view word, bool whole) {
    const auto t = NameFilterRuns(text), w = NameFilterRuns(word);
    if (t.size() < w.size() || w.empty()) return false;
    for (size_t at = 0; at + w.size() <= t.size(); ++at) {
        if (whole && at != 0) break;
        if (whole && t.size() != w.size()) break;
        bool same = true;
        for (size_t i = 0; i < w.size() && same; ++i) same = t[at + i].first == w[i].first && t[at + i].second >= w[i].second;
        if (same) return true;
    }
    return false;
}

inline bool NameHasSlur(std::string_view name) {
    std::string joined, word;
    std::vector<std::string> words;
    bool lastSingle = false;
    const auto endWord = [&] {
        if (word.empty()) return;
        // Letters typed one at a time (k i k e) are one word.
        if (word.size() == 1 && lastSingle) words.back() += word;
        else words.push_back(word);
        lastSingle = word.size() == 1;
        word.clear();
    };
    for (char c : name) {
        const char l = NameFilterLetter(c);
        if (l) { joined += l; word += l; } else endWord();
    }
    endWord();
    for (std::string_view s : kSlursAnywhere) if (NameFilterMatch(joined, s, false)) return true;
    for (const std::string& w : words)
        for (std::string_view s : kSlursWord) {
            if (NameFilterMatch(w, s, true)) return true;
            if (w.size() > 1 && w.back() == 's' && NameFilterMatch(std::string_view(w).substr(0, w.size() - 1), s, true)) return true;
        }
    return false;
}

}  // namespace hsm
