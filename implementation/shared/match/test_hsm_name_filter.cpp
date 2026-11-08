// Slurs are refused however they're spelled, and ordinary words that contain the same letters aren't.
#include "hsm_name_filter.h"
#include <cstdio>
#include <cstdlib>
using namespace hsm;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED %s:%d: %s\\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

int main() {
    for (std::string_view w : kSlursAnywhere) { CHECK(NameHasSlur(w)); CHECK(NameHasSlur("big" + std::string(w) + "man")); }
    for (std::string_view w : kSlursWord) { CHECK(NameHasSlur(w)); CHECK(NameHasSlur("the " + std::string(w))); CHECK(NameHasSlur(std::string(w) + "s")); }
    // Spelled round it: case, numbers for letters, stretched letters, gaps.
    for (const char* s : {"NIGGER", "n1gg3r", "niiiggggeeer", "n i g g e r", "n-i-g-g-e-r", "n_i_g_g_e_r", "f4gg0t", "FAG", "f a g", "k1ke", "K I K E", "cooon", "sp1cs", "retards", "r3t4rd"})
        CHECK(NameHasSlur(s));
    // Fine: words with the same letters in them. These are the ones a filter like this gets wrong.
    for (const char* s : {"Raccoon", "Cocoon", "Spice", "Spicy", "Bastard", "Con", "Conan", "Niger", "Night", "Nightmare", "Knight", "Tiger", "Homestead", "Paladin",
                          "Pakistan", "Chinook", "Wopper", "Fagin", "Flag", "Dykstra", "Bone Reaver 2", "Montenegro", "Hunter", "Mystic", "Berserker", "Half Wolverine", ""})
        CHECK(!NameHasSlur(s));
    return 0;
}
