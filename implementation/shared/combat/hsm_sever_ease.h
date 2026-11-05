#pragma once
#include <algorithm>
#include <cmath>
namespace hsm::combat {
inline int SeverEasePct(int pct) { return std::clamp(pct,25,400); }
inline double ScaleSeverWear(double damage,int pct) {
    return std::isfinite(damage) && damage>0 ? damage*(SeverEasePct(pct)/100.0) : damage;
}
}
