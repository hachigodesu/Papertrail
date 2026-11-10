# Licences MSYS2's packages don't include

`build-package.sh` takes each bundled library's licence text from its MSYS2 package. These packages have none, so their texts are kept here, copied from each project's own release:

- `djvulibre/`: `COPYING` and `COPYRIGHT` from `djvulibre-3.5.30.tar.gz` (SHA-256 `ee5e457d4cfebe566f94b99e5e3d3cc7f5c79ddb741c2ac2ba2e456f00329644`), the version MSYS2 builds.
- `giflib/`: `COPYING` from giflib 6.1.3's source, the version MSYS2 has. Leptonica brings it in, for recognising text.
- `libidn2/`: `COPYING.LESSERv3`, `COPYINGv2` and `COPYING.unicode` from libidn2 2.3.8's source (tag `v2.3.8`). It's used under the LGPL version 3 or later. Tesseract brings it in through curl.
- `lz4/`: `lib/LICENSE` from lz4 1.11.0's source (tag `v1.11.0`). Tesseract brings it in through libarchive.

Update them when the version MSYS2 has changes.
