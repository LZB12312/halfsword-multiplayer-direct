#include "combat/hsm_sever_ease.h"
#include <cstdio>
#include <limits>
int main() {
    using namespace hsm::combat;
    if(ScaleSeverWear(80,100)!=80 || ScaleSeverWear(80,25)!=20 || ScaleSeverWear(80,400)!=320) return 1;
    if(ScaleSeverWear(80,-1)!=20 || ScaleSeverWear(80,999)!=320) return 2;
    if(ScaleSeverWear(0,400)!=0 || ScaleSeverWear(-20,400)!=-20) return 3;
    if(!std::isnan(ScaleSeverWear(std::numeric_limits<double>::quiet_NaN(),200))) return 4;
    puts("PASS neutral, harder/easier cut wear, bounds and invalid native inputs");
}
