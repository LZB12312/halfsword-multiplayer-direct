#!/bin/bash
# Builds and runs every implementation/shared/**/test_*.cpp with ASan+UBSan and -Werror.
# Exit 0 only if every test binary compiles and exits 0. Uses $CXX, a host g++/clang++, or the
# org.freedesktop.Sdk flatpak's g++ when the host has no C++ compiler. TEST_JOBS tests compile and run at a time
# (default: half the cores, like the mutation gate); each test's output is printed whole, in path order.
# COMPILER_LAUNCHER (e.g. ccache) wraps each compile of a host compiler: a test is then compiled to an object first,
# so the launcher can cache it, and linked separately. Without it a test compiles and links in one step.
#   run_tests.sh            run all tests
#   run_tests.sh FILTER     run tests whose path contains FILTER
#   run_tests.sh --root DIR run test_*.cpp under DIR instead (used for the known-bad calibration fixture)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
search="$here"
if [ "${1:-}" = "--root" ]; then search="$(cd "$2" && pwd)"; shift 2; fi
filter="${1:-}"
out="${TMPDIR:-/tmp}/hsm-tests-$$"; mkdir -p "$out"; trap 'rm -rf "$out"' EXIT

compile_cmd=()
for c in "${CXX:-}" g++ clang++ c++; do
  if [ -n "$c" ] && command -v "$c" >/dev/null 2>&1; then compile_cmd=("$c"); break; fi
done
run_prefix=()
if [ ${#compile_cmd[@]} -eq 0 ]; then
  if command -v flatpak >/dev/null 2>&1 && flatpak info org.freedesktop.Sdk//25.08 >/dev/null 2>&1; then
    run_prefix=(flatpak run --filesystem=home --filesystem="$out" --command=env org.freedesktop.Sdk//25.08)
    compile_cmd=("${run_prefix[@]}" g++)
  else
    echo "run_tests: no C++ compiler (set CXX or install the flatpak org.freedesktop.Sdk//25.08)" >&2
    exit 2
  fi
fi

flags=(-std=c++20 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined
       -I"$here/protocol" -I"$here")
launch=()
if [ -n "${COMPILER_LAUNCHER:-}" ] && [ ${#run_prefix[@]} -eq 0 ]; then read -r -a launch <<<"$COMPILER_LAUNCHER"; fi
jobs=${TEST_JOBS:-$(( ($(nproc 2>/dev/null || echo 2) + 1) / 2 ))}
case "$jobs" in ''|*[!0-9]*|0) echo "run_tests: TEST_JOBS must be a positive number" >&2; exit 2 ;; esac

# One test in the background: its output goes to $out/NAME.{out,err} and its result line to $out/NAME.result. A test
# with no result file (killed, or the shell died) counts as failed.
one() {
  local src=$1 name
  name="$(basename "${src%.cpp}")"
  if [ ${#launch[@]} -gt 0 ]; then
    "${launch[@]}" "${compile_cmd[@]}" "${flags[@]}" -c -o "$out/$name.o" "$src" >"$out/$name.out" 2>"$out/$name.err" &&
      "${compile_cmd[@]}" "${flags[@]}" -o "$out/$name" "$out/$name.o" >>"$out/$name.out" 2>>"$out/$name.err"
  else
    "${compile_cmd[@]}" "${flags[@]}" -o "$out/$name" "$src" >"$out/$name.out" 2>"$out/$name.err"
  fi || { echo "FAIL (compile) $name" >"$out/$name.result"; return; }
  if "${run_prefix[@]}" env ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 "$out/$name" >>"$out/$name.out" 2>>"$out/$name.err"; then
    echo "PASS $name" >"$out/$name.result"
  else
    echo "FAIL $name" >"$out/$name.result"
  fi
}

srcs=()
while IFS= read -r src; do
  case "$src" in *"$filter"*) srcs+=("$src") ;; esac
done < <(find "$search" -name 'test_*.cpp' | sort)
for src in "${srcs[@]}"; do
  while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do wait -n; done
  one "$src" &
done
wait

pass=0; fail=0; ran=${#srcs[@]}
for src in "${srcs[@]}"; do
  name="$(basename "${src%.cpp}")"
  cat "$out/$name.out" 2>/dev/null
  cat "$out/$name.err" >&2 2>/dev/null
  result="$(cat "$out/$name.result" 2>/dev/null)"
  case "$result" in
    "PASS $name") echo "$result"; pass=$((pass + 1)) ;;
    "") echo "FAIL (no result) $name"; fail=$((fail + 1)) ;;
    *) echo "$result"; fail=$((fail + 1)) ;;
  esac
done

echo "run_tests: $ran test programs, $pass passed, $fail failed"
[ "$ran" -gt 0 ] && [ "$fail" -eq 0 ]
