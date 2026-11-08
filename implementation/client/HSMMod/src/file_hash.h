// SHA-256 of files on disk: mod paks (gameplay/mod_maps.cpp) and the mod's own DLL (the build string, session/hsm_build.h).
#pragma once
#include "util/hsm_sha256.h"
#include <string>

namespace hsm {

// The whole file at path; false if it cannot be opened or read.
bool HashFile(const std::wstring& path, Sha256Digest& out);

// The file this DLL was loaded from (dwmapi.dll next to the game exe); empty if Windows cannot say.
std::wstring OwnModulePath();

// SHA-256 of OwnModulePath()'s file, worked out once and kept; all zero when the file cannot be read.
const Sha256Digest& OwnBinaryDigest();
// BuildHash of OwnBinaryDigest() (12 hex digits); empty when the file cannot be read.
const std::string& OwnBinaryHash();

}  // namespace hsm
