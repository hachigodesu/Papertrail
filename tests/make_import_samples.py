#!/usr/bin/env python3
"""Generates the small documents in tests/samples, which are committed to the repository.

Usage: make_import_samples.py OUTPUT_DIR

All content is made up by this script. DjVu samples need DjVuLibre's c44 and djvused on PATH.
"""

import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path


def escape_pdf(text):
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def page_stream(lines):
    """`lines` is a list of (font size, text), placed from the top of the page down."""
    parts = []
    y = 780
    for size, text in lines:
        parts.append(f"BT /F1 {size} Tf 50 {y} Td ({escape_pdf(text)}) Tj ET")
        y -= size + 8
    return "\n".join(parts).encode("latin-1")


def build_pdf(pages, info=None, xmp=None, stream_filter=None, trailer_extra=b""):
    """Builds a PDF. `stream_filter(data, object_number)` can encrypt each stream and string."""
    objects = {}
    kids = []
    next_number = 4
    for content in pages:
        page_number = next_number
        stream_number = next_number + 1
        next_number += 2
        kids.append(page_number)
        objects[page_number] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                                b"/Contents %d 0 R /Resources << /Font << /F1 3 0 R >> >> >>" % stream_number)
        data = stream_filter(content, stream_number) if stream_filter else content
        objects[stream_number] = b"<< /Length %d >>\nstream\n%s\nendstream" % (len(data), data)

    catalog_extra = b""
    if xmp is not None:
        number = next_number
        next_number += 1
        objects[number] = (b"<< /Type /Metadata /Subtype /XML /Length %d >>\nstream\n%s\nendstream"
                           % (len(xmp), xmp))
        catalog_extra = b" /Metadata %d 0 R" % number

    objects[1] = b"<< /Type /Catalog /Pages 2 0 R%s >>" % catalog_extra
    objects[2] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (
        b" ".join(b"%d 0 R" % kid for kid in kids), len(kids))
    objects[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"

    info_reference = b""
    if info:
        number = next_number
        next_number += 1
        entries = b" ".join(b"/%s (%s)" % (key.encode(), escape_pdf(value).encode("latin-1"))
                            for key, value in info.items())
        objects[number] = b"<< %s >>" % entries
        info_reference = b" /Info %d 0 R" % number

    out = bytearray(b"%PDF-1.4\n")
    offsets = {}
    for number in sorted(objects):
        offsets[number] = len(out)
        out += b"%d 0 obj\n%s\nendobj\n" % (number, objects[number])
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (next_number)
    for number in range(1, next_number):
        out += b"%010d 00000 n \n" % offsets[number]
    out += b"trailer\n<< /Size %d /Root 1 0 R%s%s >>\nstartxref\n%d\n%%%%EOF\n" % (
        next_number, info_reference, trailer_extra, xref)
    return bytes(out)


def text_pdf(path, lines, info=None, xmp=None, extra_pages=None):
    pages = [page_stream(lines)] + [page_stream(p) for p in (extra_pages or [])]
    path.write_bytes(build_pdf(pages, info=info, xmp=xmp))


def reader_pdf(path):
    """Six pages with a table of contents and links, for testing the reader. Page 4 is landscape."""
    texts = [
        [(20, "Chapter One"), (11, "The opening page has links on it."), (11, "Go to page five"),
         (11, "An example web address"), (11, "An unsafe link")],
        [(16, "A Section"), (11, "The word findme appears on this page.")],
        [(11, "Nothing to see on this page.")],
        [(20, "Chapter Two"), (11, "This page is wider than it is tall.")],
        [(11, "Here is findme once."), (11, "And here is findme a second time.")],
        [(11, "The last page.")],
    ]
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R /Outlines 20 0 R >>",
        3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    }
    kids = []
    for index, lines in enumerate(texts):
        number = 4 + index * 2
        kids.append(number)
        box = b"[0 0 842 595]" if index == 3 else b"[0 0 595 842]"
        stream = page_stream(lines)
        annots = b" /Annots [30 0 R 31 0 R 32 0 R]" if index == 0 else b""
        objects[number] = (b"<< /Type /Page /Parent 2 0 R /MediaBox %s /Contents %d 0 R "
                           b"/Resources << /Font << /F1 3 0 R >> >>%s >>" % (box, number + 1, annots))
        objects[number + 1] = b"<< /Length %d >>\nstream\n%s\nendstream" % (len(stream), stream)
    objects[2] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (
        b" ".join(b"%d 0 R" % kid for kid in kids), len(kids))

    # The contents: Chapter One, holding A Section, then Chapter Two.
    objects[20] = b"<< /Type /Outlines /First 21 0 R /Last 23 0 R /Count 3 >>"
    objects[21] = (b"<< /Title (Chapter One) /Parent 20 0 R /Next 23 0 R /First 22 0 R /Last 22 0 R /Count 1 "
                   b"/Dest [%d 0 R /XYZ 0 842 0] >>" % kids[0])
    objects[22] = b"<< /Title (A Section) /Parent 21 0 R /Dest [%d 0 R /XYZ 0 842 0] >>" % kids[1]
    objects[23] = b"<< /Title (Chapter Two) /Parent 20 0 R /Prev 21 0 R /Dest [%d 0 R /XYZ 0 595 0] >>" % kids[3]

    # Links on the first page, laid over the lines they describe.
    objects[30] = (b"<< /Type /Annot /Subtype /Link /Rect [50 720 300 735] /Border [0 0 0] "
                   b"/Dest [%d 0 R /XYZ 0 842 0] >>" % kids[4])
    objects[31] = (b"<< /Type /Annot /Subtype /Link /Rect [50 700 300 715] /Border [0 0 0] "
                   b"/A << /S /URI /URI (https://example.org/paper) >> >>")
    objects[32] = (b"<< /Type /Annot /Subtype /Link /Rect [50 680 300 695] /Border [0 0 0] "
                   b"/A << /S /URI /URI (file:///etc/passwd) >> >>")

    out = bytearray(b"%PDF-1.4\n")
    offsets = {}
    for number in sorted(objects):
        offsets[number] = len(out)
        out += b"%d 0 obj\n%s\nendobj\n" % (number, objects[number])
    xref = len(out)
    size = max(objects) + 1
    out += b"xref\n0 %d\n0000000000 65535 f \n" % size
    for number in range(1, size):
        if number in offsets:
            out += b"%010d 00000 n \n" % offsets[number]
        else:
            out += b"0000000000 00000 f \n"
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (size, xref)
    path.write_bytes(bytes(out))


def numbered_issue_pdf(path):
    """A journal issue whose table of contents only numbers its entries, as some publishers' do. Each
    article's title is the largest text on its first page."""
    texts = [
        [(24, "Journal of Imaginary Findings"), (11, "Volume 3, Issue 2")],
        [(9, "Journal of Imaginary Findings, vol. 3, no. 2"), (18, "A Made-Up Study of Lattice"),
         (18, "Vibrations in Pretend Crystals"), (11, "Ada Example and Ben Sample"),
         (8, "Digital Object Identifier 10.5555/imaginary.2026.0002"), (11, "References"),
         (8, "[1] A cited work, doi:10.5555/cited.0001")],
        [(11, "The made-up study continues on this page.")],
        [(18, "Another Invented Article on Fluid Flow"), (8, "doi: 10.5555/imaginary.2026.0004"),
         (8, "Compare the earlier work at 10.5555/other.0009")],
    ]
    pages = [page_stream(lines) for lines in texts]
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R /Outlines 20 0 R >>",
        3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    }
    kids = []
    for index, stream in enumerate(pages):
        number = 4 + index * 2
        kids.append(number)
        objects[number] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents %d 0 R "
                           b"/Resources << /Font << /F1 3 0 R >> >> >>" % (number + 1))
        objects[number + 1] = b"<< /Length %d >>\nstream\n%s\nendstream" % (len(stream), stream)
    objects[2] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (
        b" ".join(b"%d 0 R" % kid for kid in kids), len(kids))

    # The contents: 01 is the cover, 02 starts on the second page, and 03 on the fourth.
    entries = [(b"01", kids[0]), (b"02", kids[1]), (b"03", kids[3])]
    objects[20] = b"<< /Type /Outlines /First 21 0 R /Last %d 0 R /Count %d >>" % (20 + len(entries), len(entries))
    for index, (title, page) in enumerate(entries):
        number = 21 + index
        links = b""
        if index > 0:
            links += b" /Prev %d 0 R" % (number - 1)
        if index < len(entries) - 1:
            links += b" /Next %d 0 R" % (number + 1)
        objects[number] = b"<< /Title (%s) /Parent 20 0 R%s /Dest [%d 0 R /XYZ 0 842 0] >>" % (title, links, page)

    out = bytearray(b"%PDF-1.4\n")
    offsets = {}
    for number in sorted(objects):
        offsets[number] = len(out)
        out += b"%d 0 obj\n%s\nendobj\n" % (number, objects[number])
    xref = len(out)
    size = max(objects) + 1
    out += b"xref\n0 %d\n0000000000 65535 f \n" % size
    for number in range(1, size):
        if number in offsets:
            out += b"%010d 00000 n \n" % offsets[number]
        else:
            out += b"0000000000 00000 f \n"
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (size, xref)
    path.write_bytes(bytes(out))


def reader_epub(path, navigation):
    """A book long enough to fill several pages. `navigation` is "nav" for EPUB 3's table of contents
    only, or "ncx" for the older kind only."""
    paragraph = "This made-up chapter goes on for long enough to fill a number of pages when laid out. " * 12
    chapters = {
        "chapter1.xhtml": ("The First Chapter", "first"),
        "chapter2.xhtml": ("The Second Chapter", "second"),
        "chapter3.xhtml": ("The Third Chapter", "third"),
    }
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(zipfile.ZipInfo("mimetype", (2020, 1, 1, 0, 0, 0)), "application/epub+zip", zipfile.ZIP_STORED)

        def add(name, text):
            z.writestr(zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0)), text)

        add("META-INF/container.xml",
            '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
            '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
            '</rootfiles></container>')
        items = "".join(f'<item id="{n[:-6]}" href="{n}" media-type="application/xhtml+xml"/>' for n in chapters)
        spine = "".join(f'<itemref idref="{n[:-6]}"/>' for n in chapters)
        if navigation == "nav":
            items += '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
            toc_attribute = ""
        else:
            items += '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
            toc_attribute = ' toc="ncx"'
        add("OEBPS/content.opf",
            '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
            '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">made-up</dc:identifier>'
            '<dc:title>A Made-up Book</dc:title><dc:language>en</dc:language></metadata>'
            f'<manifest>{items}</manifest><spine{toc_attribute}>{spine}</spine></package>')
        for name, (title, word) in chapters.items():
            body = f'<h1 id="top">{title}</h1>' + f"<p>{paragraph}</p>" * 8
            body += f'<h2 id="{word}-part">A part of the {word} chapter</h2>' + f"<p>{paragraph}</p>" * 4
            body += f"<p>The word findme{word} is only in this chapter.</p>"
            add(f"OEBPS/{name}",
                '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title>'
                f'{title}</title></head><body>{body}</body></html>')
        if navigation == "nav":
            add("OEBPS/nav.xhtml",
                '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml" '
                'xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body>'
                '<nav epub:type="toc"><h1>Contents</h1><ol>'
                '<li><a href="chapter1.xhtml">The First Chapter</a></li>'
                '<li><a href="chapter2.xhtml">The Second Chapter</a>'
                '<ol><li><a href="chapter2.xhtml#second-part">A Part of It</a></li></ol></li>'
                '<li><a href="chapter3.xhtml">The Third Chapter</a></li>'
                '</ol></nav><nav epub:type="landmarks"><ol><li><a href="chapter3.xhtml">Not contents</a></li></ol></nav>'
                '</body></html>')
        else:
            points = "".join(
                f'<navPoint id="p{i}" playOrder="{i}"><navLabel><text>{title}</text></navLabel>'
                f'<content src="{name}"/></navPoint>'
                for i, (name, (title, _)) in enumerate(chapters.items(), 1))
            add("OEBPS/toc.ncx",
                '<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">'
                f'<head/><docTitle><text>A Made-up Book</text></docTitle><navMap>{points}</navMap></ncx>')


def reader_djvu(path, work):
    """Three pages, with hidden text, a table of contents and links. Needs DjVuLibre's tools."""
    if not shutil.which("c44"):
        print("c44 not found: skipping the reader's DjVu sample")
        return
    pages = []
    for number in range(3):
        ppm = work / f"page{number}.ppm"
        # A plain page with a dark bar, so that it doesn't look blank.
        rows = bytearray(b"\xff" * (600 * 800 * 3))
        for y in range(100 + number * 40, 130 + number * 40):
            rows[y * 600 * 3 + 300:y * 600 * 3 + 1500] = b"\x20" * 1200
        ppm.write_bytes(b"P6\n600 800\n255\n" + bytes(rows))
        page = work / f"page{number}.djvu"
        subprocess.run(["c44", "-dpi", "100", str(ppm), str(page)], check=True)
        pages.append(page)
    subprocess.run(["djvm", "-c", str(path)] + [str(p) for p in pages], check=True)

    texts = [
        '(page 0 0 600 800 (line 50 700 400 730 (word 50 700 150 730 "Plain") (word 160 700 300 730 "findme") '
        '(word 310 700 400 730 "words")) (line 50 660 400 690 (word 50 660 200 690 "second") (word 210 660 400 690 "line")))',
        '(page 0 0 600 800 (line 50 700 400 730 (word 50 700 200 730 "exact") (word 210 700 400 730 "phrase")))',
        '(page 0 0 600 800 (line 50 700 400 730 (word 50 700 150 730 "more") (word 160 700 300 730 "findme")) '
        '(line 50 660 400 690 (word 50 660 300 690 "FINDME")))',
    ]
    outline = '(bookmarks ("First Part" "#1" ("A Section" "#2")) ("Second Part" "#3"))'
    # Links are in pixels up from the bottom left: within the page, out to the web, and somewhere unsafe.
    annotations = ('(maparea "#3" "" (rect 50 500 300 40)) (maparea "https://example.org/djvu" "" (rect 50 400 300 40)) '
                   '(maparea "file:///etc/passwd" "" (rect 50 300 300 40))')
    script = [f'set-outline {work / "outline.txt"}']
    (work / "outline.txt").write_text(outline)
    for number, text in enumerate(texts, 1):
        name = work / f"text{number}.txt"
        name.write_text(text)
        script += [f"select {number}", f"set-txt {name}"]
    (work / "ant.txt").write_text(annotations)
    script += ["select 1", f'set-ant {work / "ant.txt"}', "save"]
    (work / "script.txt").write_text("\n".join(s.replace("\\", "/") for s in script) + "\n")
    subprocess.run(["djvused", str(path), "-f", str(work / "script.txt")], check=True)


def rc4(key, data):
    state = list(range(256))
    j = 0
    for i in range(256):
        j = (j + state[i] + key[i % len(key)]) % 256
        state[i], state[j] = state[j], state[i]
    out = bytearray()
    i = j = 0
    for byte in data:
        i = (i + 1) % 256
        j = (j + state[i]) % 256
        state[i], state[j] = state[j], state[i]
        out.append(byte ^ state[(state[i] + state[j]) % 256])
    return bytes(out)


PDF_PADDING = bytes.fromhex("28BF4E5E4E758A4164004E56FFFA01082E2E00B6D0683E802F0CA9FE6453697A")


def encrypted_pdf(path, lines, password=b"secret"):
    """A PDF locked with the standard 40-bit handler, which needs a password to open."""
    padded = (password + PDF_PADDING)[:32]
    owner_key = hashlib.md5(padded).digest()[:5]
    owner_entry = rc4(owner_key, padded)
    permissions = -4
    file_id = b"PapertrailSample"
    key = hashlib.md5(padded + owner_entry + struct.pack("<i", permissions) + file_id).digest()[:5]
    user_entry = rc4(key, PDF_PADDING)

    def encrypt(data, object_number):
        object_key = hashlib.md5(key + struct.pack("<I", object_number)[:3] + b"\0\0").digest()[:10]
        return rc4(object_key, data)

    encrypt_dict = (b"/Encrypt << /Filter /Standard /V 1 /R 2 /O <%s> /U <%s> /P %d >> /ID [<%s> <%s>]"
                    % (owner_entry.hex().encode(), user_entry.hex().encode(), permissions,
                       file_id.hex().encode(), file_id.hex().encode()))
    path.write_bytes(build_pdf([page_stream(lines)], stream_filter=encrypt, trailer_extra=b" " + encrypt_dict))


def drawing_only_pdf(path):
    """Marks on the page but no text, like a scan without a text layer."""
    content = b"0 0 0 rg 72 500 400 200 re f 72 400 300 40 re f 72 340 350 40 re f"
    path.write_bytes(build_pdf([content]))


def make_epub(path):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml",
                   '<?xml version="1.0"?><container version="1.0" '
                   'xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
                   '<rootfiles><rootfile full-path="OEBPS/content.opf" '
                   'media-type="application/oebps-package+xml"/></rootfiles></container>')
        z.writestr("OEBPS/content.opf",
                   '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0" '
                   'unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/" '
                   'xmlns:opf="http://www.idpf.org/2007/opf">'
                   '<dc:identifier id="id" opf:scheme="ISBN">urn:isbn:9780306406157</dc:identifier>'
                   '<dc:title>Principles of Sample Engineering</dc:title>'
                   '<dc:creator opf:role="aut">Ada Example</dc:creator>'
                   '<dc:creator opf:role="aut">Grace Sample</dc:creator>'
                   '<dc:publisher>Example Press</dc:publisher><dc:date>2018-05-01</dc:date>'
                   '<dc:language>en</dc:language><dc:description>A made-up textbook.</dc:description>'
                   '<dc:subject>Engineering</dc:subject></metadata>'
                   '<manifest><item id="c1" href="c1.xhtml" media-type="application/xhtml+xml"/></manifest>'
                   '<spine><itemref idref="c1"/></spine></package>')
        z.writestr("OEBPS/c1.xhtml",
                   '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title>c1</title></head>'
                   '<body><h1>Chapter One</h1><p>This chapter introduces the principles of sample engineering '
                   'with several worked exercises for students.</p></body></html>')


def make_fb2(path):
    path.write_text(
        '<?xml version="1.0" encoding="utf-8"?>'
        '<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">'
        "<description><title-info><book-title>Sample FictionBook</book-title></title-info></description>"
        "<body><section><p>A short made-up book, with enough words here to count as having some text.</p>"
        "</section></body></FictionBook>",
        encoding="utf-8")


def mobi_record0(text_length, title, version, exth_entries):
    exth = b""
    if exth_entries:
        body = b"".join(struct.pack(">II", kind, len(value) + 8) + value for kind, value in exth_entries)
        length = 12 + len(body)
        padding = b"\0" * (-length % 4)
        exth = b"EXTH" + struct.pack(">II", length + len(padding), len(exth_entries)) + body + padding

    header_length = 232
    record = bytearray(16 + header_length)
    struct.pack_into(">HHIHHI", record, 0, 1, 0, text_length, 1, 4096, 0)
    record[16:20] = b"MOBI"
    struct.pack_into(">IIII", record, 20, header_length, 2, 65001, 1)
    struct.pack_into(">I", record, 36, version)
    full_name_offset = 16 + header_length + len(exth)
    struct.pack_into(">II", record, 84, full_name_offset, len(title))
    struct.pack_into(">I", record, 108, 0xFFFFFFFF)
    struct.pack_into(">I", record, 128, 0x40 if exth else 0)
    record += exth + title
    record += b"\0" * (-len(record) % 4)
    return bytes(record)


def make_kindle(path, version, title, exth_entries, text):
    records = [mobi_record0(len(text), title, version, exth_entries), text]
    table_start = 78
    data_start = table_start + 8 * len(records) + 2
    out = bytearray(data_start)
    out[0:len(title[:31])] = title[:31]
    out[60:68] = b"BOOKMOBI"
    struct.pack_into(">H", out, 76, len(records))
    offset = data_start
    for index, record in enumerate(records):
        struct.pack_into(">II", out, table_start + 8 * index, offset, index * 2)
        offset += len(record)
    for record in records:
        out += record
    path.write_bytes(out)


def page_pixel(x, y):
    return 20 if 100 <= y < 140 and 20 <= x < 180 else 255


def png_bytes(width, height):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    rows = b"".join(b"\0" + bytes(page_pixel(x, y) for x in range(width) for _ in range(3)) for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def make_cbz(path):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("page001.png", png_bytes(200, 300))


def make_xps(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml",
                   '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="fdseq" ContentType="application/vnd.ms-package.xps-fixeddocumentsequence+xml"/>'
                   '<Default Extension="fdoc" ContentType="application/vnd.ms-package.xps-fixeddocument+xml"/>'
                   '<Default Extension="fpage" ContentType="application/vnd.ms-package.xps-fixedpage+xml"/></Types>')
        z.writestr("_rels/.rels",
                   '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/'
                   'relationships"><Relationship Id="R0" Type="http://schemas.microsoft.com/xps/2005/06/'
                   'fixedrepresentation" Target="/doc.fdseq"/></Relationships>')
        z.writestr("doc.fdseq", '<FixedDocumentSequence xmlns="http://schemas.microsoft.com/xps/2005/06">'
                                '<DocumentReference Source="/d/doc.fdoc"/></FixedDocumentSequence>')
        z.writestr("d/doc.fdoc", '<FixedDocument xmlns="http://schemas.microsoft.com/xps/2005/06">'
                                 '<PageContent Source="/d/1.fpage"/></FixedDocument>')
        z.writestr("d/1.fpage", '<FixedPage Width="595" Height="842" xmlns="http://schemas.microsoft.com/xps/2005/06" '
                                'xml:lang="en"><Path Fill="#000000" Data="M 72,72 L 500,72 500,200 72,200 Z"/></FixedPage>')


def make_djvu(directory):
    if not shutil.which("c44"):
        print("c44 not found: skipping DjVu samples")
        return
    ppm = directory / "page.ppm"
    pixels = bytes(page_pixel(x, y) for y in range(300) for x in range(200) for _ in range(3))
    ppm.write_bytes(b"P6\n200 300\n255\n" + pixels)
    scanned = directory / "scanned.djvu"
    subprocess.run(["c44", str(ppm), str(scanned)], check=True)

    with_text = directory / "with_text.djvu"
    shutil.copy(scanned, with_text)
    text_file = directory / "page.txt"
    words = "This DjVu page has a hidden text layer with enough words to be counted as real text"
    text_file.write_text(f'(page 0 0 200 300 (line 0 0 200 20 "{words}"))\n', encoding="ascii")
    subprocess.run(["djvused", with_text.name, "-e", f"select 1; set-txt {text_file.name}", "-s"],
                   check=True, cwd=directory)
    ppm.unlink()
    text_file.unlink()


XMP = b"""<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
<rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:prism="http://prismstandard.org/namespaces/basic/2.0/"
  prism:publicationDate="2019-06-15">
<dc:title><rdf:Alt><rdf:li xml:lang="x-default">Turbulent Flow in Example Channels</rdf:li></rdf:Alt></dc:title>
<dc:creator><rdf:Seq><rdf:li>Mei Tanaka</rdf:li><rdf:li>Luis Garcia</rdf:li></rdf:Seq></dc:creator>
<prism:doi>10.5555/turbulent.2019</prism:doi>
<prism:issn>1234-5679</prism:issn>
</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>"""


def filler(pages):
    return [[(10, f"Page {i + 2} of this long document.")] for i in range(pages)]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)

    body = [(10, "Abstract: this made-up paper reports on the fatigue life of steel beams under load.")]

    text_pdf(out / "article.pdf",
             [(20, "Fatigue of Steel Beams")] + body + [(9, "https://doi.org/10.1234/example.5678."), (9, "Copyright 2020")],
             info={"Title": "Fatigue of Steel Beams", "Author": "Jane Smith; Omar Haddad", "Keywords": "fatigue, steel"})

    text_pdf(out / "xmp_article.pdf", [(20, "Turbulent Flow in Example Channels")] + body, xmp=XMP)

    text_pdf(out / "junk_metadata.pdf",
             [(24, "Neural Bridges in Practice"), (9, "A made-up paper with a useless title in its metadata.")] + body,
             info={"Title": "Microsoft Word - draft3.docx", "Author": "Administrator"})

    text_pdf(out / "no_metadata.pdf", [(9, "Plain text with no big heading and nothing useful in it at all.")] * 3)

    text_pdf(out / "book.pdf", [(30, "Applied Sample Mechanics"), (12, "Second Edition")],
             extra_pages=[[(10, "Published by Example Press. ISBN 978-0-306-40615-7. Copyright 2015.")]]
             + [[(10, f"Page {i} of the book, with exercises for the reader to attempt.")] for i in range(3, 121)],
             info={"Title": "Applied Sample Mechanics", "Author": "Ada Example"})

    text_pdf(out / "thesis.pdf",
             [(20, "On the Behaviour of Example Materials"),
              (11, "A thesis submitted in partial fulfilment of the requirements for the degree of"),
              (11, "Master of Science at the University of Example")],
             extra_pages=filler(40),
             info={"Title": "On the Behaviour of Example Materials", "Author": "Tara Sample"})

    text_pdf(out / "dissertation.pdf",
             [(20, "Advanced Example Dynamics"), (11, "Doctor of Philosophy, University of Example")],
             extra_pages=filler(40),
             info={"Title": "Advanced Example Dynamics", "Author": "Sven Sample"})

    # A short journal paper whose references mention other kinds of work. It's still an article.
    text_pdf(out / "article_citing_things.pdf",
             [(20, "Short Paper With Many References"),
              (9, "https://doi.org/10.1234/own.doi")] + body,
             extra_pages=[[(10, "References"),
                           (9, "[1] A. Author, PhD thesis, University of Example, 2001. doi:10.9999/cited.one"),
                           (9, "[2] B. Author, Technical Report 12, Example Institute."),
                           (9, "[3] C. Author, in Proceedings of the Conference on Examples."),
                           (9, "[4] D. Author, A Book, ISBN 978-0-306-40615-7."),
                           (9, "[5] E. Author, arXiv:2101.00001.")]],
             info={"Title": "Short Paper With Many References", "Author": "Jane Smith"})

    # The kinds of metadata real publishers produce.
    text_pdf(out / "publisher_code_title.pdf",
             [(28, "Medicine"), (18, "Mechanisms of Example Disease in Adults"), (9, "doi:10.1234/code.title")] + body,
             info={"Title": "ab1c00123 1..10", "Author": "Nadia Sample, Hiro Example, Yara Testing"})

    text_pdf(out / "entity_title.pdf", [(20, "Heading")] + body,
             info={"Title": "Samples&#x2019; Challenges &#x0026; Perspectives&quest;"})

    text_pdf(out / "wrapped_title.pdf",
             [(20, "A Regional Sample Market for Example Goods and"), (20, "Services in Example Districts")] + body)

    text_pdf(out / "report.pdf",
             [(20, "Corrosion Survey of Example Bridges"), (11, "Technical Report TR-2021-14, Example Institute")],
             info={"Title": "Corrosion Survey of Example Bridges"})

    text_pdf(out / "standard.pdf",
             [(20, "Sample Management Systems"), (11, "INTERNATIONAL STANDARD ISO 9999:2015")],
             info={"Title": "Sample Management Systems"})

    text_pdf(out / "conference.pdf",
             [(20, "Graphs for Example Networks"), (11, "In Proceedings of the International Conference on Examples 2022")],
             info={"Title": "Graphs for Example Networks", "Author": "Wen Li and Rosa Silva"})

    text_pdf(out / "preprint.pdf",
             [(20, "Sampling Made Simple"), (10, "arXiv:2101.00001v2 [cs.LG] 3 Jan 2021"),
              (10, "This made-up preprint explains simple sampling.")],
             info={"Title": "Sampling Made Simple"})

    text_pdf(out / "preprint_stamp.pdf",
             [(20, "Measuring Example Flows"), (10, "Preprint submitted to the Journal of Made-up Results"),
              (10, "doi:10.1000/example.2020.1"), (10, "This made-up paper has no arXiv number.")],
             info={"Title": "Measuring Example Flows"})

    with tempfile.TemporaryDirectory() as work:
        reader_djvu(out / "reader_doc.djvu", Path(work))
    reader_pdf(out / "reader_doc.pdf")
    numbered_issue_pdf(out / "numbered_issue.pdf")
    reader_epub(out / "reader_nav.epub", "nav")
    reader_epub(out / "reader_ncx.epub", "ncx")

    drawing_only_pdf(out / "scanned_no_text.pdf")
    (out / "blank.pdf").write_bytes(build_pdf([b""]))
    encrypted_pdf(out / "encrypted.pdf", [(20, "Locked Paper"), (10, "You should not be able to read this text.")])

    good = (out / "article.pdf").read_bytes()
    (out / "truncated.pdf").write_bytes(good[:len(good) // 3])
    (out / "garbage.pdf").write_bytes(bytes((i * 37 + 11) % 256 for i in range(4096)))
    (out / "empty.pdf").write_bytes(b"")
    (out / "not_really.epub").write_bytes(b"This is plain text with an EPUB file name.")

    make_epub(out / "book.epub")
    make_fb2(out / "sample.fb2")
    make_cbz(out / "sample.cbz")
    make_xps(out / "sample.xps")

    html = b"<html><body><p>A short made-up Kindle book with enough words here to count as real text.</p></body></html>"
    make_kindle(out / "old_format.mobi", 6, b"Old Kindle Book",
                [(100, b"Omar Haddad"), (101, b"Example Press"), (503, b"Old Kindle Book")], html)
    make_kindle(out / "kf8_header.azw3", 8, b"New Kindle Book",
                [(100, b"Priya Sample"), (101, b"Example Press"), (104, b"978-0-306-40615-7"),
                 (106, b"2021-03-04"), (503, b"New Kindle Book")], html)

    (out / "ignored_notes.txt").write_text("Not a supported document type.\n", encoding="ascii")
    make_djvu(out)


if __name__ == "__main__":
    main()
