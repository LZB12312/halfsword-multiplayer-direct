#!/bin/bash
# Cross-compiles HSMMod as dwmapi.dll (a load-time import of the game exe) with llvm-mingw (no MSVC needed).
# Usage: ./build.sh [Release|Debug]   Output: build/dwmapi.dll
set -euo pipefail
cd "$(dirname "$0")"
TC=${LLVM_MINGW:-$HOME/.local/opt/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64}
CXX="$TC/bin/x86_64-w64-mingw32-clang++"
CC="$TC/bin/x86_64-w64-mingw32-clang"
CFG=${1:-Release}
OPT=$([ "$CFG" = Debug ] && echo "-O0 -g" || echo "-O2")
mkdir -p build/obj

COMMON="$OPT -Wall -Wextra -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -fstack-protector-strong"
# The build string players compare (session/hsm_build.h): kModVersion plus this checkout's commit (and -dirty), if any.
DESCRIBE=$(git describe --always --dirty --exclude='*' 2>/dev/null | tr -cd 'A-Za-z0-9._+-' || true)
INC="-Isrc -I../../shared -I../../shared/protocol -Ithird_party/minhook/include -Ithird_party/enet/include -Ithird_party/miniupnpc/include -iquotethird_party/miniupnpc -DMINIUPNP_STATICLIB"

objs=()
for f in third_party/miniupnpc/src/{igd_desc_parse,miniupnpc,minixml,minisoap,minissdpc,miniwget,upnpcommands,upnpdev,upnpreplyparse,upnperrors,connecthostport,portlistingparse,receivedata,addr_is_reserved}.c; do
  o=build/obj/miniupnpc_$(basename "${f%.c}").o
  "$CC" $COMMON -w $INC -c "$f" -o "$o"
  objs+=("$o")
done
for f in third_party/minhook/src/{buffer,hook,trampoline}.c third_party/minhook/src/hde/hde64.c; do
  o=build/obj/$(basename "${f%.c}").o
  "$CC" $COMMON -w -Ithird_party/minhook/include -c "$f" -o "$o"
  objs+=("$o")
done
for f in third_party/enet/{callbacks,compress,host,list,packet,peer,protocol,win32}.c; do
  o=build/obj/enet_$(basename "${f%.c}").o
  "$CC" $COMMON -w -Ithird_party/enet/include -c "$f" -o "$o"
  objs+=("$o")
done
for f in src/*.cpp src/*/*.cpp; do
  [ -e "$f" ] || continue
  o=build/obj/$(echo "${f%.cpp}" | tr '/' '_').o
  "$CXX" -std=c++20 $COMMON -Werror $INC -DHSM_GIT_DESCRIBE="\"$DESCRIBE\"" -c "$f" -o "$o"
  objs+=("$o")
done
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
