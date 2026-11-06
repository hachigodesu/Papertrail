#!/usr/bin/env python3
"""Generates tiny test documents for format-probe.

Usage: make_samples.py OUTPUT_DIR

These only show a format's basic path works. They don't replace real-world
files. AZW3/KF8 isn't generated, because a valid one can't be written by hand.

DjVu is made separately from sample.ppm with DjVuLibre's c44 tool.
"""

import struct
import sys
import zipfile
import zlib
from pathlib import Path

MARKER = "PAPERTRAILPROBE"
BODY = f"{MARKER} sample text for the Papertrail format test."


def make_pdf(path):
    stream = f"BT /F1 18 Tf 72 720 Td ({BODY}) Tj ET".encode()
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
        b"/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        b"<< /Length %d >>\nstream\n%s\nendstream" % (len(stream), stream),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n%s\nendobj\n" % (number, body)
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    for offset in offsets:
        out += b"%010d 00000 n \n" % offset
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(objects) + 1, xref)
    path.write_bytes(out)


def make_epub(path):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml",
                   '<?xml version="1.0"?><container version="1.0" '
                   'xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
                   '<rootfiles><rootfile full-path="OEBPS/content.opf" '
                   'media-type="application/oebps-package+xml"/></rootfiles></container>')
        z.writestr("OEBPS/content.opf",
                   '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" '
                   'version="3.0" unique-identifier="id"><metadata '
                   'xmlns:dc="http://purl.org/dc/elements/1.1/">'
                   '<dc:identifier id="id">urn:papertrail:probe</dc:identifier>'
                   '<dc:title>Probe</dc:title><dc:language>en</dc:language></metadata>'
                   '<manifest><item id="c1" href="c1.xhtml" '
                   'media-type="application/xhtml+xml"/></manifest>'
                   '<spine><itemref idref="c1"/></spine></package>')
        z.writestr("OEBPS/c1.xhtml",
                   '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml">'
                   f"<head><title>Probe</title></head><body><p>{BODY}</p></body></html>")


def make_fb2(path):
    path.write_text(
        '<?xml version="1.0" encoding="utf-8"?>'
        '<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">'
        "<description><title-info><book-title>Probe</book-title></title-info></description>"
        f"<body><section><p>{BODY}</p></section></body></FictionBook>",
        encoding="utf-8")


def make_mobi(path):
    """An uncompressed PalmDOC MOBI: the old format, not KF8."""
    text = f"<html><body><p>{BODY}</p></body></html>".encode()
    title = b"Probe"
    header_len = 232

    record0 = bytearray(16 + header_len)
    struct.pack_into(">HHIHHI", record0, 0, 1, 0, len(text), 1, 4096, 0)
    record0[16:20] = b"MOBI"
    struct.pack_into(">IIII", record0, 20, header_len, 2, 65001, 1)  # length, type, UTF-8, uid
    struct.pack_into(">I", record0, 36, 6)  # file version
    struct.pack_into(">II", record0, 84, len(record0), len(title))  # full name offset, length
    struct.pack_into(">I", record0, 108, 0xFFFFFFFF)  # first image record: none
    record0 += title
    record0 += b"\0" * (-len(record0) % 4)

    records = [bytes(record0), text]
    table_start = 78
    data_start = table_start + 8 * len(records) + 2

    out = bytearray(data_start)
    out[0:5] = title
    out[60:68] = b"BOOKMOBI"
    struct.pack_into(">H", out, 76, len(records))
    offset = data_start
    for index, record in enumerate(records):
        struct.pack_into(">II", out, table_start + 8 * index, offset, index * 2)
        offset += len(record)
    for record in records:
        out += record
    path.write_bytes(out)


def png_bytes(width, height):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data)))

    rows = b"".join(b"\0" + bytes(page_pixel(x, y) for x in range(width) for _ in range(3))
                    for y in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows))
            + chunk(b"IEND", b""))


def page_pixel(x, y):
    """A white page with one dark band, so a successful render shows ink."""
    return 20 if 100 <= y < 140 and 20 <= x < 180 else 255


def make_cbz(path):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("page001.png", png_bytes(200, 300))


def make_ppm(path, width=200, height=300):
    pixels = bytes(page_pixel(x, y) for y in range(height) for x in range(width) for _ in range(3))
    path.write_bytes(b"P6\n%d %d\n255\n" % (width, height) + pixels)


def make_xps(path):
    """Vector content only: XPS text needs an embedded font."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml",
                   '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/'
                   'package/2006/content-types"><Default Extension="rels" ContentType='
                   '"application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="fdseq" ContentType='
                   '"application/vnd.ms-package.xps-fixeddocumentsequence+xml"/>'
                   '<Default Extension="fdoc" ContentType='
                   '"application/vnd.ms-package.xps-fixeddocument+xml"/>'
                   '<Default Extension="fpage" ContentType='
                   '"application/vnd.ms-package.xps-fixedpage+xml"/></Types>')
        z.writestr("_rels/.rels",
                   '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/'
                   'package/2006/relationships"><Relationship Id="R0" Type="http://schemas.'
                   'microsoft.com/xps/2005/06/fixedrepresentation" Target="/doc.fdseq"/>'
                   "</Relationships>")
        z.writestr("doc.fdseq",
                   '<FixedDocumentSequence xmlns="http://schemas.microsoft.com/xps/2005/06">'
                   '<DocumentReference Source="/d/doc.fdoc"/></FixedDocumentSequence>')
        z.writestr("d/doc.fdoc",
                   '<FixedDocument xmlns="http://schemas.microsoft.com/xps/2005/06">'
                   '<PageContent Source="/d/1.fpage"/></FixedDocument>')
        z.writestr("d/1.fpage",
                   '<FixedPage Width="595" Height="842" '
                   'xmlns="http://schemas.microsoft.com/xps/2005/06" xml:lang="en">'
                   '<Path Fill="#000000" Data="M 72,72 L 500,72 500,200 72,200 Z"/></FixedPage>')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    make_pdf(out / "sample.pdf")
    make_epub(out / "sample.epub")
    make_fb2(out / "sample.fb2")
    make_mobi(out / "sample.mobi")
    make_cbz(out / "sample.cbz")
    make_xps(out / "sample.xps")
    make_ppm(out / "sample.ppm")


if __name__ == "__main__":
    main()
