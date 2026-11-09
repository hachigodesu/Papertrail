#!/bin/bash
# Builds and tests Papertrail on the Linux distro this runs in, normally a WSL distro started by test-all.sh.
#
# Usage: tools/dev/test-linux.sh NAME [CMAKE_PREFIX_PATH]
#
# NAME labels the logs, which go in build/test-logs/ in the repository. The build stays in ~/papertrail-build, in the
# Linux filesystem, since building on /mnt/c is much slower. CMAKE_PREFIX_PATH is where Qt is, if it isn't the
# distro's own, such as /opt/Qt/6.12.0/gcc_64 on Ubuntu.
set -uo pipefail
name=${1:?Give a name for the logs, such as arch}
shift
src="$(cd "$(dirname "$0")/../.." && pwd)"
logs="$src/build/test-logs"
mkdir -p "$logs"
extra=()
[ $# -ge 1 ] && [ -n "$1" ] && extra=("-DCMAKE_PREFIX_PATH=$1")

grep PRETTY_NAME /etc/os-release
cd "$src" || exit 1
cmake --preset linux-debug -B "$HOME/papertrail-build" "${extra[@]}" > "$logs/$name-configure.log" 2>&1 \
    || { echo "configure failed"; tail -20 "$logs/$name-configure.log"; exit 1; }
# One distro at a time, with a job limit: building on two at once, or without one, has hung the shared WSL VM.
timeout 1800 cmake --build "$HOME/papertrail-build" -j 8 > "$logs/$name-build.log" 2>&1
echo "build rc=$?"
grep -E 'error|warning:|FAILED' "$logs/$name-build.log" | cut -c1-200 | head -15

QT_QPA_PLATFORM=offscreen timeout 1800 ctest --test-dir "$HOME/papertrail-build" -LE perf --output-on-failure </dev/null \
    > "$logs/$name-test.log" 2>&1
echo "ctest rc=$?"
grep -E 'tests passed|tests failed|FAIL!|Actual|Expected|Loc:|QFATAL|\(Failed\)|\(Subprocess|Timeout|Total Test time' \
    "$logs/$name-test.log" | cut -c1-200
