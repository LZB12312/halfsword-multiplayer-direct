// AC-1: command-line flag lookup and agent-bridge command parsing (game-independent).
#include "agent/hsm_agent.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestCmdArg() {
    std::string cl = "\"C:\\Game\\HalfSwordUE5-Win64-Shipping.exe\" -hsmrole=client -hsmhost=127.0.0.1 -hsmtag=\"my client\" -hsmtest=all -hsmbridge=27800";
    CHECK(CmdArg(cl, "hsmrole").value_or("") == "client");
    CHECK(CmdArg(cl, "hsmhost").value_or("") == "127.0.0.1");
    CHECK(CmdArg(cl, "hsmtag").value_or("") == "my client");                // quoted value
    CHECK(CmdArg(cl, "hsmbridge").value_or("") == "27800");                  // last argument
    CHECK(!CmdArg(cl, "hsm").has_value());                                    // no prefix match
    CHECK(!CmdArg(cl, "role").has_value());                                   // must start a token
    CHECK(!CmdArg(cl, "hsmport").has_value());
    CHECK(!CmdArg("x -hsmrole", "hsmrole").has_value());                      // flag without value
    CHECK(CmdArg("-hsmrole= ", "hsmrole").value_or("?").empty());             // explicit empty value
    CHECK(CmdArg("a -hsmtest=all -hsmtest=x", "hsmtest").value_or("") == "all");   // first wins
    CHECK(CmdArgInt("-hsmport=27001", "hsmport", 1, 100, 65535) == 27001);
    CHECK(CmdArgInt("-hsmport=abc", "hsmport", 1, 100, 65535) == 1);           // garbage -> fallback
    CHECK(CmdArgInt("-hsmport=99", "hsmport", 1, 100, 65535) == 1);            // out of range -> fallback
    CHECK(CmdArgInt("", "hsmport", 7, 100, 65535) == 7);
}

static void TestParseCommand() {
    AgentCommand c;
    CHECK(ParseCommand("state", c) && c.verb == "state" && c.args.empty());
    CHECK(ParseCommand("  axis  MoveForward 1.0 500 \r", c) && c.verb == "axis" && c.args.size() == 3 && c.args[0] == "MoveForward" && c.args[2] == "500");
    CHECK(ParseCommand("open /Game/Maps/Arenas/Map_Arena_Yard", c) && c.args[0] == "/Game/Maps/Arenas/Map_Arena_Yard");
    CHECK(!ParseCommand("", c) && !ParseCommand("   \r\n", c));
    CHECK(!ParseCommand(std::string(5000, 'a'), c));                         // oversized line refused
    CHECK(!ParseCommand("bad\x01verb", c));                                   // control characters refused
    CHECK(ParseArgFloat("0.5", -1, 1) == 0.5f && std::isnan(ParseArgFloat("2", -1, 1)) && std::isnan(ParseArgFloat("x", -1, 1)));
}

static void TestJsonEscape() {
    CHECK(JsonString("a\"b\\c\nd") == "\"a\\\"b\\\\c\\nd\"");
    CHECK(JsonString(std::string("x\x01y")) == "\"x\\u0001y\"");
}

int main() {
    TestCmdArg(); TestParseCommand(); TestJsonEscape();
    // The mod-off relaunch keeps the player's own arguments and drops the exe, an old role and a Steam join.
    CHECK(RelaunchArgs("\"F:\\Steam Library\\HalfSwordUE5-Win64-Shipping.exe\" -dx12 \"-log=my log.txt\"", 42, true) == "-dx12 \"-log=my log.txt\" -hsmrole=off -hsmwaitfor=42");
    CHECK(RelaunchArgs("game.exe -hsmrole=steam +connect_lobby 1098 -hsmwaitfor=7 -windowed", 9, true) == "-windowed -hsmrole=off -hsmwaitfor=9");
    CHECK(RelaunchArgs("game.exe", 1, true) == "-hsmrole=off -hsmwaitfor=1" && RelaunchArgs("", 1, true) == "-hsmrole=off -hsmwaitfor=1");
    CHECK(RelaunchArgs("game.exe -hsmrole=off -dx12", 5, false) == "-dx12 -hsmwaitfor=5");   // a restart with the mod on drops an old off
    CHECK(CmdArg(RelaunchArgs("game.exe -x", 77, true), "hsmrole") == std::optional<std::string>("off") && CmdArgInt(RelaunchArgs("g", 77, true), "hsmwaitfor", 0, 1, 1 << 30) == 77);
    std::printf(g_fail ? "%d FAILED\n" : "all agent tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
