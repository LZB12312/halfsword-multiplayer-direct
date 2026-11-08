#include "ue4ss_bundle.h"
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "config.h"
#include "file_hash.h"
#include "log.h"
#include "match/hsm_online_rules.h"
#include "miniz.h"

// The zip as UE4SS published it, read-only data in this DLL. The path is relative to build.sh's folder.
__asm__(
    ".section .rdata,\"dr\"\n"
    ".p2align 4\n"
    ".globl hsm_ue4ss_zip\n"
    "hsm_ue4ss_zip:\n"
    ".incbin \"third_party/ue4ss/UE4SS_v3.0.1-1217-g4fb9b615.zip\"\n"
    ".globl hsm_ue4ss_zip_end\n"
    "hsm_ue4ss_zip_end:\n"
    ".text\n");
extern "C" const unsigned char hsm_ue4ss_zip[];
extern "C" const unsigned char hsm_ue4ss_zip_end[];

namespace hsm::ue4ss {
namespace {
constexpr const char* kBundleDll = "ue4ss\\UE4SS.dll";
constexpr const char* kBundleModsTxt = "ue4ss\\Mods\\mods.txt";

std::wstring WideAscii(const std::string& s) { return std::wstring(s.begin(), s.end()); }
bool FileThere(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
void MakeParents(const std::wstring& path) {
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1))
        CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
}
// Written beside its place and moved in whole: a crash or a full disk never leaves half a file for UE4SS to load.
bool WriteFileWhole(const std::wstring& path, const void* data, size_t size) {
    MakeParents(path);
    const std::wstring tmp = path + L".hsm-tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, data, DWORD(size), &wrote, nullptr) && wrote == size;
    CloseHandle(f);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(tmp.c_str());
    return false;
}
bool Unpack(mz_zip_archive& zip, mz_uint index, const std::string& rel, const std::wstring& dir) {
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, index, &size, 0);
    if (!data) return false;
    bool ok;
    if (rel == kBundleModsTxt) {
        const std::vector<std::string> off(std::begin(kConsoleMods), std::end(kConsoleMods));
        const std::string text = ModsTxtWithOff(std::string(static_cast<const char*>(data), size), off);
        ok = WriteFileWhole(dir + WideAscii(rel), text.data(), text.size());
    } else {
        ok = WriteFileWhole(dir + WideAscii(rel), data, size);
    }
    mz_free(data);
    return ok;
}
// Unpacks what's missing and writes the DLL. True when the bundled DLL is in place.
bool UnpackZip(const std::wstring& dir, BundleStep step) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, hsm_ue4ss_zip, size_t(hsm_ue4ss_zip_end - hsm_ue4ss_zip), 0)) {
        HSM_LOG("ue4ss: the bundled zip cannot be read");
        return false;
    }
    int written = 0, failed = 0;
    mz_uint dllIndex = 0;
    bool dllSeen = false, dllIn = false;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { ++failed; continue; }
        const std::string rel = BundleFilePath(st.m_filename);
        if (rel.empty()) continue;
        if (rel == kBundleDll) { dllIndex = i; dllSeen = true; continue; }
        if (FileThere(dir + WideAscii(rel))) continue;
        if (Unpack(zip, i, rel, dir)) ++written; else ++failed;
    }
    // The DLL goes last and only after the rest: its being there is what says UE4SS is installed.
    if (dllSeen && !failed) { dllIn = Unpack(zip, dllIndex, kBundleDll, dir); if (dllIn) ++written; else ++failed; }
    mz_zip_reader_end(&zip);
    HSM_LOG("ue4ss: bundled build %s: %d file(s) written, %d failed", step == BundleStep::Install ? "installed" : "replaced another build", written, failed);
    return dllIn;
}
// On the worker thread, so only UE4SS waits for the answer. No answer at all (the box couldn't open): it stays, and they're asked next time.
bool AskToReplace(const std::string& sha256) {
    const int answer = MessageBoxW(nullptr,
        L"ue4ss\\UE4SS.dll isn't a UE4SS build the mod knows. It may be one of your own.\n\n"
        L"Replace it with the UE4SS that comes with the mod? Your UE4SS mods and settings stay, but this DLL is overwritten.\n\n"
        L"Yes: replace it.\n"
        L"No: keep it. Online play will refuse it, and the mod won't ask about this file again.",
        L"Half Sword Multiplayer", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
    HSM_LOG("ue4ss: unknown UE4SS.dll %s: %s", sha256.c_str(), answer == IDYES ? "the player chose to replace it" : answer == IDNO ? "the player kept it" : "no answer, kept for now");
    if (answer == IDNO && !SaveIniValues({{"ue4ss_keep", sha256}}, "mod")) HSM_LOG("ue4ss: could not save that answer; the mod will ask again");
    return answer == IDYES;
}
// A rename works on a DLL that is running, and the player can undo it.
void SetAside(const std::wstring& dir, const char* name) {
    const std::wstring from = dir + WideAscii(name);
    const bool ok = MoveFileExW(from.c_str(), (from + L".off").c_str(), MOVEFILE_REPLACE_EXISTING);
    HSM_LOG("ue4ss: %s %s", name, ok ? "renamed to .off: the bundled build takes its place" : "could not be renamed to .off");
}
}  // namespace

void InstallBundled(const std::wstring& dir, const Files& files, const std::string& keep) {
    std::string have;
    Sha256Digest sha{};
    if (files.sub && HashFile(dir + WideAscii(kBundleDll), sha)) have = Sha256Hex(sha);
    BundleStep step = ChooseBundleStep(files.sub, have == kBundledUe4ss, IsKnownUe4ss(have), !have.empty() && have == keep);
    if (step == BundleStep::Keep) return;
    if (step == BundleStep::Ask) {
        if (!AskToReplace(have)) return;
        step = BundleStep::Replace;
    }
    if (step != BundleStep::None && !UnpackZip(dir, step)) return;   // The old one stays rather than leave no UE4SS at all.
    if (files.beside) SetAside(dir, "UE4SS.dll");
    if (files.xinput2x) SetAside(dir, "xinput1_3.dll");
}
}  // namespace hsm::ue4ss
