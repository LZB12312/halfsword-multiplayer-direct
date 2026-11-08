// Unit tests for hsm_claw_mirror.h. Run via implementation/shared/run_tests.sh test_hsm_claw_mirror
#include "match/hsm_claw_mirror.h"
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
int main() {
    ClawState s; s.out = true; s.bone = false;
    s.blades.push_back({"hand_r", 1.5, -2.25, 3, 90, -45, 10});
    s.blades.push_back({"hand_l", 0, 0, 0, 0, 0, 0});
    const ClawState d = DecodeClaws(EncodeClaws(s));
    CHECK(d.out && !d.bone && d.blades.size() == 2 && d.blades[0].bone == "hand_r" && d.blades[1].bone == "hand_l");
    CHECK(std::fabs(d.blades[0].x - 1.5) < 0.11 && std::fabs(d.blades[0].yaw + 45) < 0.11);
    CHECK(EncodeClaws(ClawState{}).empty() && !DecodeClaws("").out);                             // sheathed
    for (const char* bad : {"X;hand_r,0,0,0,0,0,0", "B", "B;", "B;hand_r,0,0,0,0,0", "B;hand_r,0,0,0,0,0,0,0", "B;ha/nd,0,0,0,0,0,0",
                            "B;hand_r,nan,0,0,0,0,0", "B;hand_r,inf,0,0,0,0,0", "B;hand_r,999,0,0,0,0,0", "B;hand_r,0,0,0,999,0,0", "B;hand_r,a,0,0,0,0,0"})
        CHECK(!DecodeClaws(bad).out);                                                            // peer-written: nothing drawn
    std::string many = "B";
    for (int i = 0; i < 7; ++i) many += ";hand_r,0,0,0,0,0,0";
    CHECK(!DecodeClaws(many).out);                                                               // too many blades
    CHECK(!DecodeClaws(std::string(kMaxClawValue + 1, 'B')).out);
    if (g_fail == 0) std::printf("ok\n");
    return g_fail != 0;
}
