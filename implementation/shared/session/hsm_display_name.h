#pragma once
#include <string>
#include "util/hsm_sanitize.h"
namespace hsm {
inline std::string CleanDisplayName(const std::string& input) {
    auto name=util::StripControlChars(input);
    const auto first=name.find_first_not_of(' ');
    if(first==std::string::npos) return {};
    name=name.substr(first,name.find_last_not_of(' ')-first+1);
    if(name.size()>32) {
        size_t n=32;
        while(n && (static_cast<unsigned char>(name[n])&0xc0)==0x80) --n;
        name.resize(n);
    }
    return name;
}
}
