#!/bin/bash
# Builds the AUR package from the current checkout rather than a release, to test the PKGBUILD before tagging.
#
# Usage, on Arch: packaging/arch/test-pkgbuild.sh [OUTPUT_DIR]
#
# Makes a source tarball as GitHub would for the tag, points a copy of the PKGBUILD at it, and runs makepkg,
# which also runs the program's self-test. The package is left in OUTPUT_DIR, not installed.
set -euo pipefail

src="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(realpath -m "${1:-$src/build/arch-package}")"
version="$(sed -n 's/^pkgver=//p' "$src/packaging/arch/PKGBUILD")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# What's committed, plus anything changed but not yet committed, as GitHub's archive of a tag would hold.
git -c safe.directory="$src" -C "$src" ls-files -z --cached --others --exclude-standard \
    | tar -C "$src" --null -T - --transform "s,^,Papertrail-$version/," -czf "$work/papertrail-$version.tar.gz"
sed "s|^source=(\"\$pkgname-\$pkgver.tar.gz::[^\"]*\"|source=(\"\$pkgname-\$pkgver.tar.gz\"|" \
    "$src/packaging/arch/PKGBUILD" > "$work/PKGBUILD"
grep -q '^source=("$pkgname-$pkgver.tar.gz"' "$work/PKGBUILD" || { echo "FAILED: couldn't point the PKGBUILD at the checkout" >&2; exit 1; }
# The checkout's own archive isn't the release's, so its checksum is skipped. The libraries' are still checked.
sed -i "s/^sha256sums=('[0-9a-f]*'/sha256sums=('SKIP'/" "$work/PKGBUILD"
grep -q "^sha256sums=('SKIP'" "$work/PKGBUILD" || { echo "FAILED: couldn't skip the checkout's checksum" >&2; exit 1; }

mkdir -p "$out"
(cd "$work" && PKGDEST="$out" makepkg --force --cleanbuild --noconfirm)
namcap "$out"/papertrail-*.pkg.tar.zst 2>/dev/null || true
ls -l "$out"
