# Software included with Papertrail

Papertrail is free software under the GNU Affero General Public License, version 3 or later (`LICENSE`). Its source code is at https://github.com/hachigodesu/Papertrail.

Packages of Papertrail include other free software. Each package includes the licence texts of what it bundles: on Windows, in the `licenses` folder beside the program, and in Linux packages, under `share/licenses`. The main parts are:

| Software | Used for | Licence | Source |
|---|---|---|---|
| Qt 6 | The window, networking and images | LGPL-3.0-only | https://www.qt.io/download-open-source |
| MuPDF | Reading PDF, EPUB, XPS, CBZ, FB2 and MOBI | AGPL-3.0-or-later | https://mupdf.com |
| DjVuLibre | Reading DjVu | GPL-2.0-or-later | https://djvu.sourceforge.net |
| libmobi | Reading Kindle KF8 books | LGPL-3.0-or-later | https://github.com/bfabiszewski/libmobi |
| SQLite | The library's database | Public domain | https://sqlite.org |
| QtKeychain | Keeping the Claude API key in the system's credential store | BSD-3-Clause | https://github.com/frankosterfeld/qtkeychain |
| zlib | Library exports | Zlib | https://zlib.net |
| Tesseract | Recognising the text of scanned pages | Apache-2.0 | https://github.com/tesseract-ocr/tesseract |
| Leptonica | Tesseract's handling of page images | BSD-2-Clause | http://www.leptonica.org |
| Tesseract's English model (tessdata_fast 4.1.0) | Recognising English. Other languages are fetched when chosen, from the same collection | Apache-2.0 | https://github.com/tesseract-ocr/tessdata_fast |
| OpenSSL | Secure connections for online lookups and summaries, in Linux packages. Windows uses its own | Apache-2.0 | https://www.openssl.org |

MuPDF, Qt and Tesseract bring in others, such as FreeType, HarfBuzz, libjpeg-turbo, OpenJPEG, jbig2dec, Gumbo, libpng, ICU, Brotli, libarchive, libcurl and the GCC OpenMP runtime, whose licences are included the same way.

Qt is used under the LGPL, so you may replace the Qt libraries in a package with your own build of the same version. DjVuLibre is used under GPL version 3, as its "or later" allows, which section 13 of the GNU GPL and AGPL lets be combined with Papertrail.
