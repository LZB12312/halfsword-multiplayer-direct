// Command-line flags and the local agent-bridge command language. Game-independent, unit-tested.
// The bridge (client/HSMMod/src/agent/bridge.cpp) is only enabled by -hsmbridge=<port> and listens on 127.0.0.1.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace hsm {

// Splits a Windows-style command line into tokens; double quotes group spaces and are removed.
inline std::vector<std::string> SplitCommandLine(const std::string& cl) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : cl) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == ' ' || c == '\t')) { if (any) out.push_back(cur); cur.clear(); any = false; continue; }
        cur.push_back(c); any = true;
    }
    if (any) out.push_back(cur);
    return out;
}

// Value of the first "-name=value" token; nullopt when absent or given without '='.
inline std::optional<std::string> CmdArg(const std::string& cl, const std::string& name) {
    std::string prefix = "-" + name + "=";
    for (const std::string& t : SplitCommandLine(cl))
        if (t.compare(0, prefix.size(), prefix) == 0) {
            std::string v = t.substr(prefix.size());
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            return v;
        }
    return std::nullopt;
}

inline int CmdArgInt(const std::string& cl, const std::string& name, int fallback, int lo, int hi) {
    std::optional<std::string> v = CmdArg(cl, name);
    if (!v || v->empty() || v->size() > 9) return fallback;
    char* end = nullptr;
    long n = std::strtol(v->c_str(), &end, 10);
    return end && *end == 0 && n >= lo && n <= hi ? int(n) : fallback;
}

// The arguments for a new copy of the game when the mod restarts it: "play alone, mod off", or after turning unsupported mods off.
// This game's own arguments go along (its exe is started by path, so not that), except an earlier -hsmrole, -hsmwaitfor or a Steam
// join. The new game waits for this one (pid) to close. -hsmrole=off only lives on that launch, so the next ordinary start has the
// mod again and nothing has to be put back.
inline std::string RelaunchArgs(const std::string& cl, unsigned long pid, bool modOff) {
    const std::vector<std::string> tokens = SplitCommandLine(cl);
    std::string out;
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (t.rfind("-hsmrole=", 0) == 0 || t.rfind("-hsmwaitfor=", 0) == 0) continue;
        if (t == "+connect_lobby") { ++i; continue; }   // and the lobby id after it
        if (!out.empty()) out += ' ';
        out += t.find_first_of(" \t") == std::string::npos ? t : '"' + t + '"';
    }
    if (!out.empty()) out += ' ';
    return out + (modOff ? "-hsmrole=off " : "") + "-hsmwaitfor=" + std::to_string(pid);
}

struct AgentCommand { std::string verb; std::vector<std::string> args; };
constexpr size_t kMaxCommandLine = 1024;

// One text line -> verb + whitespace-separated args. Refuses empty, oversized or control-character lines.
inline bool ParseCommand(const std::string& line, AgentCommand& out) {
    if (line.size() > kMaxCommandLine) return false;
    std::string clean;
    for (char c : line) {
        if (c == '\r' || c == '\n') continue;
        if (static_cast<unsigned char>(c) < 0x20 && c != ' ' && c != '\t') return false;
        clean.push_back(c);
    }
    std::vector<std::string> words;
    std::string cur;
    for (char c : clean) {
        if (c == ' ' || c == '\t') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) words.push_back(cur);
    if (words.empty()) return false;
    out.verb = words[0];
    out.args.assign(words.begin() + 1, words.end());
    return true;
}

// Float argument within [lo, hi]; NaN when unparsable or out of range.
inline float ParseArgFloat(const std::string& s, float lo, float hi) {
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    return !s.empty() && end && *end == 0 && std::isfinite(v) && v >= lo && v <= hi ? v : NAN;
}

inline std::string JsonString(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if (c == '\n') o += "\\n";
        else if (u < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", u); o += b; }
        else o.push_back(c);
    }
    return o + "\"";
}

}  // namespace hsm
