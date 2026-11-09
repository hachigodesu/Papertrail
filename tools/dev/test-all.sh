#!/bin/bash
# Runs every test on Windows, then on the Arch and Ubuntu WSL distros, one at a time, as each change should be
# before it's called finished. Run it with Bash on Windows from anywhere: it finds the repository itself.
#
# Usage: tools/dev/test-all.sh [WINDOWS_BUILD_DIR]
#
# WINDOWS_BUILD_DIR defaults to build/windows-debug, and must already be built. The Linux builds are made by
# tools/dev/test-linux.sh inside each distro. Logs go in build/test-logs/. See docs/wsl-dev-setup.md for the distros.
set -uo pipefail
src="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$src/build/windows-debug}"
logs="$src/build/test-logs"
mkdir -p "$logs"
export PATH="/c/msys64/ucrt64/bin:$PATH" # Before any other CMake on PATH, and for the tests' DLLs.

slowest() { tr -d '\000\r' < "$1" | grep -E 'Passed|Failed|Timeout' | awk '{print $(NF-1), $4}' | sort -rn | head -3 | tr '\n' ';'; echo; }

echo "=== WINDOWS"
QT_QPA_PLATFORM=offscreen timeout 1800 ctest --test-dir "$build" -LE perf --output-on-failure </dev/null > "$logs/windows-test.log" 2>&1
grep -E 'tests passed|tests failed|FAIL!|Actual|Expected|Loc:|QFATAL|\(Failed\)|Timeout|Total Test time' "$logs/windows-test.log" | cut -c1-200
echo "slowest: $(slowest "$logs/windows-test.log")"

# The repository as Windows names it, which wslpath turns into the distro's own path for it.
windows_src="$(cygpath -m "$src")"
run_linux() {
    local distro=$1 user=$2 name=$3 qt=$4 tries=0
    while true; do
        MSYS_NO_PATHCONV=1 wsl.exe -d "$distro" -u "$user" bash -lc \
            "cd \"\$(wslpath '$windows_src')\" && bash tools/dev/test-linux.sh $name $qt" \
            > "$logs/$name-run.log" 2>&1
        # Windows is sometimes too short of resources to start WSL's VM. Trying again later usually works.
        if tr -d '\000' < "$logs/$name-run.log" | grep -q 'ERROR_NO_SYSTEM_RESOURCES\|WSAETIMEDOUT' && [ $tries -lt 5 ]; then
            tries=$((tries + 1)); echo "WSL couldn't start for $name, trying again in a minute ($tries)"; sleep 60; continue
        fi
        break
    done
    tr -d '\000\r' < "$logs/$name-run.log" | grep -v networkingMode | cut -c1-200
    [ -f "$logs/$name-test.log" ] && echo "slowest: $(slowest "$logs/$name-test.log")"
}
echo "=== ARCH"; run_linux Papertrail-Arch dev arch ""
echo "=== UBUNTU"; run_linux Papertrail-Ubuntu ubuntu ubuntu /opt/Qt/6.12.0/gcc_64
