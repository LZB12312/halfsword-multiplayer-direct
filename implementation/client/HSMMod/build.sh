#!/bin/bash
# Cross-compiles HSMMod as dwmapi.dll (a load-time import of the game exe) with llvm-mingw (no MSVC needed).
# Usage: ./build.sh [Release|Debug]   Output: build/dwmapi.dll
# BUILD_JOBS files compile at a time (default: half the cores); the link always takes the objects in file order.
# COMPILER_LAUNCHER (e.g. ccache) wraps each compile. Only files that use HSM_GIT_DESCRIBE get the commit, so a new
# commit does not change every file's command line and a cache can keep the rest.
set -euo pipefail
cd "$(dirname "$0")"
TC=${LLVM_MINGW:-$HOME/.local/opt/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64}
CXX="$TC/bin/x86_64-w64-mingw32-clang++"
CC="$TC/bin/x86_64-w64-mingw32-clang"
CFG=${1:-Release}
OPT=$([ "$CFG" = Debug ] && echo "-O0 -g" || echo "-O2")
mkdir -p build/obj

COMMON="${HSM_EXTRA:-} $OPT -Wall -Wextra -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -fstack-protector-strong"
# Only reading a zip from memory (src/ue4ss_bundle.cpp). Its structs depend on these, so every file gets them.
COMMON="$COMMON -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_WRITING_APIS -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES"
# The build string players compare (session/hsm_build.h): kModVersion plus this checkout's commit (and -dirty), if any.
DESCRIBE=$(git describe --always --dirty --exclude='*' 2>/dev/null | tr -cd 'A-Za-z0-9._+-' || true)
INC="-Isrc -I../../shared -I../../shared/protocol -Ithird_party/minhook/include -Ithird_party/miniz -Ithird_party/enet/include -Ithird_party/miniupnpc/include -iquotethird_party/miniupnpc -DMINIUPNP_STATICLIB"

JOBS=${BUILD_JOBS:-$(( ($(nproc 2>/dev/null || echo 2) + 1) / 2 ))}
case "$JOBS" in ''|*[!0-9]*|0) echo "build: BUILD_JOBS must be a positive number" >&2; exit 2 ;; esac
# Each compile runs in the background with its diagnostics in OBJ.log and its exit status in OBJ.status; the logs are
# printed in file order once all are done. A missing status (the compile never finished) counts as a failure.
launch=()
[ -z "${COMPILER_LAUNCHER:-}" ] || read -r -a launch <<<"$COMPILER_LAUNCHER"
objs=()
compile() {
  local o=$1; shift
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n || true; done
  rm -f "$o.status"
  ( if "${launch[@]}" "$@" -o "$o" >"$o.log" 2>&1; then echo 0; else echo 1; fi >"$o.status" ) &
  objs+=("$o")
}
for f in third_party/minhook/src/{buffer,hook,trampoline}.c third_party/minhook/src/hde/hde64.c; do
  compile "build/obj/$(basename "${f%.c}").o" "$CC" $COMMON -w -Ithird_party/minhook/include -c "$f"
done
compile build/obj/miniz.o "$CC" $COMMON -w -c third_party/miniz/miniz.c
for f in third_party/miniupnpc/src/{igd_desc_parse,miniupnpc,minixml,minisoap,minissdpc,miniwget,upnpcommands,upnpdev,upnpreplyparse,upnperrors,connecthostport,portlistingparse,receivedata,addr_is_reserved}.c; do
  compile "build/obj/miniupnpc_$(basename "${f%.c}").o" "$CC" $COMMON -w $INC -c "$f"
done
for f in third_party/enet/{callbacks,compress,host,list,packet,peer,protocol,win32}.c; do
  compile "build/obj/enet_$(basename "${f%.c}").o" "$CC" $COMMON -w $INC -c "$f"
done
for f in src/*.cpp src/*/*.cpp; do
  [ -e "$f" ] || continue
  desc=()
  if grep -q HSM_GIT_DESCRIBE "$f"; then desc=(-DHSM_GIT_DESCRIBE="\"$DESCRIBE\""); fi
  compile "build/obj/$(echo "${f%.cpp}" | tr '/' '_').o" "$CXX" -std=c++20 $COMMON -Werror $INC "${desc[@]}" -c "$f"
done
wait
failed=0
for o in "${objs[@]}"; do
  cat "$o.log" >&2
  [ "$(cat "$o.status" 2>/dev/null)" = 0 ] || { echo "build: compile failed for $o" >&2; failed=1; }
done
[ "$failed" -eq 0 ] || exit 1
# The map keeps symbol addresses for crash reports even though the DLL itself is stripped.
LINK_FLAGS="-fstack-protector-strong -s -Wl,-Map,build/dwmapi.map"
# Try control flow guard if supported. tools/check_dll.py requires the GUARD_CF bit exactly when build/guard_cf says 1.
echo 0 > build/guard_cf
if "$CXX" -c -x c++ /dev/null -o /dev/null -mguard=cf 2>/dev/null; then
  LINK_FLAGS="$LINK_FLAGS -mguard=cf"
  echo 1 > build/guard_cf
fi
"$CXX" -shared -o build/dwmapi.dll "${objs[@]}" src/dwmapi.def \
  $LINK_FLAGS -static -static-libgcc -static-libstdc++ -Wl,--kill-at -lws2_32 -lwinmm -liphlpapi
echo "built build/dwmapi.dll ($CFG)"
