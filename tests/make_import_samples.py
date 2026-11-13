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


def issue_without_contents_pdf(path):
    """A journal issue with no table of contents at all. Its articles can only be found from their first pages,
    each with a large title and its own DOI. A running header repeats the issue's numbers, and an article's DOI is
    repeated in its footer, as some publishers do."""
    texts = [
        [(24, "Journal of Imaginary Findings"), (11, "Volume 3, Issue 4")],
        [(18, "Publication Information"), (9, "Journal of Imaginary Findings is published by Example Press.")],
        [(9, "Journal of Imaginary Findings, vol. 3, no. 4"), (18, "A Pretend Survey of Tidal Gardens"),
         (11, "Cara Example"), (8, "Digital Object Identifier 10.5555/imaginary.2026.0401")],
        [(9, "Journal of Imaginary Findings, vol. 3, no. 4"), (11, "The pretend survey continues on this page."),
         (8, "10.5555/imaginary.2026.0401")],
        [(18, "Invented Notes on Paper Boats"), (8, "doi: 10.5555/imaginary.2026.0402"),
         (8, "Compare the earlier work at 10.5555/other.0009")],
        [(11, "More about paper boats, which float.")],
        [(18, "Information for Authors"), (9, "Send made-up manuscripts to the made-up editor.")],
    ]
    path.write_bytes(build_pdf([page_stream(lines) for lines in texts]))


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


def picture_pdf(path):
    """Text above a made-up picture, a grey square with a dark bar, which dark pages should leave alone.
    The picture is 200 by 200 points, at 100 points from the left and 300 from the bottom."""
    width = height = 64
    pixels = bytes((40 if 24 <= y < 40 else 160) for y in range(height) for _ in range(width))
    image = zlib.compress(pixels)
    text = page_stream([(20, "A Page With a Picture"), (10, "The picture below should keep its colours on dark pages.")])
    content = text + b"\nq 200 0 0 200 100 300 cm /Im1 Do Q"
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [4 0 R] /Count 1 >>",
        3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        4: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 5 0 R "
           b"/Resources << /Font << /F1 3 0 R >> /XObject << /Im1 6 0 R >> >> >>",
        5: b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content),
        6: b"<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 8 "
           b"/Filter /FlateDecode /Length %d >>\nstream\n%s\nendstream" % (width, height, len(image), image),
    }
    out = bytearray(b"%PDF-1.4\n")
    offsets = {}
    for number in sorted(objects):
        offsets[number] = len(out)
        out += b"%d 0 obj\n%s\nendobj\n" % (number, objects[number])
    xref = len(out)
    out += b"xref\n0 7\n0000000000 65535 f \n"
    for number in range(1, 7):
        out += b"%010d 00000 n \n" % offsets[number]
    out += b"trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % xref
    path.write_bytes(bytes(out))


def judgment_pdf(path):
    """A made-up judgment of a court in England, laid out as its first page would be, for recognising case law."""
    text_pdf(path, [(10, "Neutral Citation Number: [2021] EWCA Civ 1234"),
                    (10, "Case No: A2/2020/0567"),
                    (12, "IN THE COURT OF APPEAL (CIVIL DIVISION)"),
                    (10, "Before:"),
                    (10, "LORD JUSTICE EXAMPLE"),
                    (10, "Between:"),
                    (14, "Made-Up Holdings Ltd v Another Example Plc"),
                    (16, "JUDGMENT")],
             extra_pages=[[(10, "1. This made-up appeal concerns a made-up contract.")]])


def reading_ruler_pdf(path):
    """Three made-up pages for the reading ruler: a running head and a page number in the margins, a notice running up
    the side of the page, as publishers' downloads have, a heading set in two pieces on one row, then three paragraphs,
    the first two set apart by a gap and the third only by its first line being set in; a second page; and a third
    with a figure's labels scattered between its text, and a footer set above the bottom margin."""

    def placed(items):
        """`items` is a list of (size, x, y, text), in points from the bottom left."""
        return "\n".join(f"BT /F1 {size} Tf {x} {y} Td ({escape_pdf(text)}) Tj ET"
                         for size, x, y, text in items).encode("latin-1")

    sideways = b"\nBT /F1 8 Tf 0 1 -1 0 30 250 Tm (Downloaded from a made-up journal, for testing only) Tj ET"
    pages = [
        placed([(9, 50, 815, "A Made-Up Journal of Reading, Volume 1"),
                (14, 50, 770, "2.3"),
                (14, 110, 770, "Made-up measures"),
                (11, 50, 740, "The first paragraph of a made-up page begins here and"),
                (11, 50, 726, "carries on over a second line of words that mean nothing,"),
                (11, 50, 712, "and ends on a third."),
                (11, 50, 680, "A second paragraph follows after a gap, as most do,"),
                (11, 50, 666, "and has two lines."),
                (11, 70, 652, "A third is only set in from the line before it,"),
                (11, 50, 638, "as some books set them."),
                (9, 290, 30, "1")]) + sideways,
        placed([(9, 50, 815, "A Made-Up Journal of Reading, Volume 1"),
                (11, 50, 740, "The second page has one paragraph of its own,"),
                (11, 50, 726, "over two lines."),
                (9, 290, 30, "2")]),
        placed([(11, 50, 760, "A third page has a made-up figure in the middle of it,"),
                (11, 50, 746, "with labels scattered around a drawing, which are not read"),
                (11, 50, 732, "along with the text around them."),
                (11, 230, 690, "A made-up label above"),
                (11, 60, 640, "Left label"),
                (11, 400, 640, "Right label"),
                (11, 220, 590, "Two lines of a"),
                (11, 235, 576, "made-up label:"),
                (11, 50, 520, "After the figure the text carries on as it did before it,"),
                (11, 50, 506, "and the footer at the bottom of the page is not read either."),
                (9, 50, 70, "Made-up month 2026"),
                (9, 200, 70, "MADE-UP JOURNAL"),
                (9, 520, 70, "3")]),
    ]
    path.write_bytes(build_pdf(pages))


def reading_figures_pdf(path):
    """Two made-up pages with figures for the reading ruler. The first has three panels of small filled boxes side by
    side, as a heat map has, with a colour bar, numbers along their axes, the axes' names, and labels inside each panel
    lining up across them as a table's cells do, then a caption, and a table drawn with its cells' lines. The second
    has a plot drawn with a line in a box, with numbers along its axes."""

    def placed(items):
        """`items` is a list of (size, x, y, text), in points from the bottom left."""
        return "\n".join(f"BT /F1 {size} Tf {x} {y} Td ({escape_pdf(text)}) Tj ET"
                         for size, x, y, text in items).encode("latin-1")

    def upwards(size, x, y, text):
        return f"BT /F1 {size} Tf 0 1 -1 0 {x} {y} Tm ({escape_pdf(text)}) Tj ET"

    # The first page's figure: three panels, 8 boxes of 15 points across at the foot, one fewer each row up.
    shapes = []
    words = []
    for panel in range(3):
        left = 90 + panel * 134
        words.append((9, left + 25, 710, f"max delay: {panel * 10}.0%"))
        for row in range(8):
            for column in range(row + 1):
                shade = 0.2 + 0.08 * ((row + column + panel) % 8)
                shapes.append(f"{shade:.2f} 0.1 0.3 rg {left + 15 * column} {705 - 15 * (row + 1)} 15 15 re f")
        for n, label in enumerate(["min:0.0", f"max:0.{panel + 6}", f"mean:0.{panel + 2}"]):
            words.append((9, left + 62, 680 - 12 * n, label))
        for column, label in enumerate(["365", "730", "1460", "2920", "4380", "8760", "17520", "35040"]):
            shapes.append(upwards(7, left + 15 * column + 11, 556, label))
        words.append((9, left + 25, 543, "Frequency (/year)"))
    for row, label in enumerate(["24.0", "12.0", "6.0", "3.0", "2.0", "1.0", "0.5", "0.25"]):
        words.append((8, 70, 705 - 15 * (row + 1) + 4, label))
    shapes.append(upwards(9, 62, 600, "Duration (hour)"))
    for step in range(8):
        shapes.append(f"{0.1 + 0.1 * step:.2f} 0.2 0.3 rg 495 {585 + 15 * step} 10 15 re f")
    for n, label in enumerate(["0.0", "0.4", "0.8"]):
        words.append((8, 509, 585 + 55 * n, label))
    # A table drawn with its cells' lines, as one shape.
    grid = ["0 0 0 RG 0.6 w"]
    for y in (432, 412, 392, 372, 352):
        grid.append(f"55 {y} m 420 {y} l")
    for x in (55, 190, 330, 420):
        grid.append(f"{x} 352 m {x} 432 l")
    grid.append("S")
    cells = [["Made-up", "First", "Second"], ["Alpha", "12", "30"], ["Beta", "8", "20"], ["Gamma", "5", "11"]]
    table = [(10, [60, 200, 340][c], 418 - 20 * r, cell) for r, row in enumerate(cells) for c, cell in enumerate(row)]
    first = (placed([(11, 50, 790, "A made-up page with a figure drawn as many small boxes,"),
                     (11, 50, 776, "with labels around and inside it, which the ruler takes"),
                     (11, 50, 762, "whole, and which are not read along with the text.")]
                    + words
                    + [(10, 50, 518, "Figure 1: A made-up figure of boxes, for testing the ruler."),
                       (11, 50, 490, "After the figure the text carries on, and then comes a"),
                       (11, 50, 476, "table drawn with its cells' lines, which is one stop too,"),
                       (11, 50, 462, "while each of its rows is still read a row at a time.")]
                    + table
                    + [(9, 290, 30, "1")])
             + ("\n" + "\n".join(shapes + grid)).encode("latin-1"))

    # The second page's plot: a line in a box, with numbers along its axes.
    plot = ["0 0 0 RG 0.8 w 100 550 300 180 re S", "0.2 0.4 0.2 RG 1 w 100 600 m"]
    for n in range(1, 31):
        plot.append(f"{100 + 10 * n} {600 + (37 * n) % 120} l")
    plot.append("S")
    second = (placed([(11, 50, 790, "The second made-up page has a plot drawn with a line, its"),
                      (11, 50, 776, "numbers set around it, and nothing else in it at all, so"),
                      (11, 50, 762, "that it is one stop for the ruler, and none of it is read."),
                      (11, 200, 738, "Made-up utilisation"),
                      (8, 80, 548, "0.0"), (8, 80, 638, "0.5"), (8, 80, 726, "1.0"),
                      (8, 98, 538, "0"), (8, 196, 538, "10"), (8, 296, 538, "20"), (8, 394, 538, "30"),
                      (9, 240, 524, "Day"),
                      (10, 50, 496, "Figure 2: A made-up plot."),
                      (11, 50, 466, "After it, the text ends with one more line to read here,"),
                      (11, 50, 452, "and then the page ends with its number at the foot of it."),
                      (9, 290, 30, "2")])
              + ("\n" + "\n".join(plot)).encode("latin-1"))
    path.write_bytes(build_pdf([first, second]))


def reading_page_ends_pdf(path):
    """Two made-up pages set with narrow margins, so their text reaches where running heads and footers usually are,
    with a footer and a running head set apart from it that aren't read. The first page's last sentence carries on
    over the page, broken in the middle of a word."""

    def placed(items):
        """`items` is a list of (size, x, y, text), in points from the bottom left."""
        return "\n".join(f"BT /F1 {size} Tf {x} {y} Td ({escape_pdf(text)}) Tj ET"
                         for size, x, y, text in items).encode("latin-1")

    first = ["A made-up page whose text runs on almost to the foot",
             "of the page, as some papers set it, with no more than",
             "a narrow margin below its last line. Its last lines",
             "are still read with the rest, and a footer set apart",
             "below them is not. This sentence, though, does not",
             "end on this page, but carries on over it, broken in",
             "the middle of a word as it goes onto the next, exam-"]
    second = ["ple of one, and ends here on the second page. Then the",
              "second page carries on with a sentence of its own, over",
              "two lines, and that is all it has to say about it.",
              "Nothing here means anything at all, and the words were",
              "made up for testing, as were the pages they are on."]
    pages = [
        placed([(11, 50, 110 - 12 * n, line) for n, line in enumerate(first)]
               + [(9, 50, 14, "Made-up footer, not read"), (9, 290, 14, "1")]),
        placed([(9, 50, 826, "A Made-Up Journal of Reading, Volume 1")]
               + [(11, 50, 794 - 12 * n, line) for n, line in enumerate(second)]
               + [(9, 290, 14, "2")]),
    ]
    path.write_bytes(build_pdf(pages))


def reading_table_pdf(path):
    """A made-up page in two columns for the reading ruler, with a table in the first beside running text in the
    second: a caption, a row of column headings, rows of a label and numbers, a row that only heads the rows after it,
    a label wrapped onto a second line, and notes below. The table is drawn a column at a time, as some files keep
    them, so its rows have to be put back together."""

    def placed(items):
        """`items` is a list of (x, y, text), in points from the bottom left, all set in 9 points."""
        return "\n".join(f"BT /F1 9 Tf {x} {y} Td ({escape_pdf(text)}) Tj ET" for x, y, text in items).encode("latin-1")

    labels = [(48, 700, "Number"), (40, 686, "Made-up section"), (48, 672, "First"), (48, 658, "Second"),
              (48, 644, "Mean size of"), (52, 633, "the measure"), (48, 618, "Other")]
    columns = [
        [(140, 715, "Group A"), (140, 700, "30 (60%)"), (140, 672, "12 (40%)"), (140, 658, "18 (60%)"),
         (140, 644, "3.1 (0.4)"), (140, 618, "1 (3%)")],
        [(190, 715, "Group B"), (190, 700, "20 (40%)"), (190, 672, "8 (40%)"), (190, 658, "12 (60%)"),
         (190, 644, "3.3 (0.5)"), (190, 618, "2 (10%)")],
        [(240, 715, "Total"), (240, 700, "50"), (240, 672, "20 (40%)"), (240, 658, "30 (60%)"),
         (240, 644, "3.2 (0.4)"), (240, 618, "3 (6%)")],
    ]
    running = ["The second column has running text of its own, set",
               "beside the table in the first, and it is read after",
               "the table, a line at a time, as the rest of a page",
               "is. None of the words here mean anything at all, and",
               "the numbers in the table beside them were made up",
               "for testing the ruler, which should go through the",
               "whole table a row at a time rather than skipping",
               "most of it, and then carry on with this column."]
    items = [(40, 740, "Table 1 Made-up measures of made-up people, n (%)")] + labels
    for column in columns:
        items += column
    items += [(40, 596, "a Made-up test: U = 12, z = 0.5, for testing only."),
              (40, 584, "b A short note.")]
    items += [(310, 740 - 12 * n, line) for n, line in enumerate(running)]
    path.write_bytes(build_pdf([placed(items)]))


def smart_jump_headings_pdf(path):
    """A made-up paper whose mentions of Appendix E and Figure 4 wrap onto the start of lines in the middle of a
    paragraph, looking like a heading and a caption, before the real caption on page 2 and the real heading, in bold
    and larger, on page 3."""

    def placed(items):
        """`items` is a list of (font, size, x, y, text), in points from the bottom left."""
        return "\n".join(f"BT /{font} {size} Tf {x} {y} Td ({escape_pdf(text)}) Tj ET"
                         for font, size, x, y, text in items).encode("latin-1")

    pages = [
        placed([("F1", 11, 50, 760, "The derivation is given in Appendix E, which sets out"),
                ("F1", 11, 50, 746, "the made-up method in full, and the result is set out in"),
                ("F1", 11, 50, 732, "Appendix E. In the following pages the method is made"),
                ("F1", 11, 50, 718, "up as we go, and its result is shown as in Figure 4 of"),
                ("F1", 11, 50, 704, "the results, which comes later. As it says in the text,"),
                ("F1", 11, 50, 690, "Figure 4. As the made-up result shows, the beam bends."),
                ("F1", 11, 50, 676, "That is all there is to say about it on this page.")]),
        placed([("F1", 11, 50, 760, "A page with a made-up result on it."),
                ("F1", 11, 50, 600, "Figure 4: The made-up result, as a caption.")]),
        placed([("F2", 16, 50, 760, "Appendix E Made-Up Derivations"),
                ("F1", 11, 50, 730, "The made-up derivation follows here.")]),
    ]
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        30: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
    }
    kids = []
    for index, stream in enumerate(pages):
        number = 4 + index * 2
        kids.append(number)
        objects[number] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents %d 0 R "
                           b"/Resources << /Font << /F1 3 0 R /F2 30 0 R >> >> >>" % (number + 1))
        objects[number + 1] = b"<< /Length %d >>\nstream\n%s\nendstream" % (len(stream), stream)
    objects[2] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (
        b" ".join(b"%d 0 R" % kid for kid in kids), len(kids))

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


def smart_jump_contents_pdf(path):
    """A made-up book whose first page is its contents, with each entry's page number set at the right and no dots
    leading to it, then a page mentioning Appendix E and Section 2.1, wrapped so "Appendix E." starts a line, then the
    chapters and sections, each headed in bold, 160 pages of filler, and the appendix and index, further from the
    mention than Smart Jump looks around it. Its printed page numbers are its pages in the file."""

    def placed(items):
        """`items` is a list of (font, size, x, y, text), in points from the bottom left."""
        return "\n".join(f"BT /{font} {size} Tf {x} {y} Td ({escape_pdf(text)}) Tj ET"
                         for font, size, x, y, text in items).encode("latin-1")

    pages = [
        placed([("F2", 16, 72, 760, "Contents"),
                ("F2", 11, 72, 720, "1  Made-Up Beginnings"), ("F2", 11, 500, 720, "3"),
                ("F2", 11, 72, 700, "2  Made-Up Methods"), ("F2", 11, 500, 700, "4"),
                ("F1", 11, 90, 682, "2.1  Bending Made-Up Beams"), ("F1", 11, 500, 682, "4"),
                ("F1", 11, 90, 664, "2.2  Twisting Made-Up Shafts"), ("F1", 11, 500, 664, "4"),
                ("F2", 11, 72, 644, "Appendix E:  Made-Up Tables"), ("F2", 11, 500, 644, "165"),
                ("F2", 11, 72, 624, "Index"), ("F2", 11, 500, 624, "166")]),
        placed([("F1", 11, 50, 760, "The made-up tables are all set out in full in"),
                ("F1", 11, 50, 746, "Appendix E. The method behind them is made up as"),
                ("F1", 11, 50, 732, "it goes, and Section 2.1 explains how beams bend.")]),
        placed([("F2", 16, 50, 760, "1  Made-Up Beginnings"),
                ("F1", 11, 50, 730, "Everything here begins in a made-up way.")]),
        placed([("F2", 16, 50, 760, "2  Made-Up Methods"),
                ("F1", 11, 50, 730, "The methods are made up too."),
                ("F2", 12, 50, 690, "2.1  Bending Made-Up Beams"),
                ("F1", 11, 50, 670, "A made-up beam bends under a made-up load."),
                ("F2", 12, 50, 630, "2.2  Twisting Made-Up Shafts"),
                ("F1", 11, 50, 610, "A made-up shaft twists.")]),
    ] + [placed([("F1", 11, 50, 760, f"A made-up page of filler, number {n}.")]) for n in range(5, 165)] + [
        placed([("F2", 16, 50, 760, "Appendix E  Made-Up Tables"),
                ("F1", 11, 50, 730, "The made-up tables follow here.")]),
        placed([("F2", 16, 50, 760, "Index"),
                ("F1", 11, 50, 730, "Beams, 4")]),
    ]
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        30: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
    }
    kids = []
    for index, stream in enumerate(pages):
        number = 4 + index * 2
        kids.append(number)
        objects[number] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents %d 0 R "
                           b"/Resources << /Font << /F1 3 0 R /F2 30 0 R >> >> >>" % (number + 1))
        objects[number + 1] = b"<< /Length %d >>\nstream\n%s\nendstream" % (len(stream), stream)
    objects[2] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (
        b" ".join(b"%d 0 R" % kid for kid in kids), len(kids))

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


def smart_jump_pdf(path):
    """A made-up paper whose text refers to its figure, table, equation, section and references, with no links, for
    finding what a reference points at. Page 1 has the mentions, page 2 the method, an equation numbered (3) at the
    right and Table 1, page 3 a picture captioned Figure 2 with a mention of it below, and page 4 the references,
    numbered and then by author and year."""

    def placed(items):
        """`items` is a list of (font size, x, y, text), in points from the bottom left."""
        return "\n".join(f"BT /F1 {size} Tf {x} {y} Td ({escape_pdf(text)}) Tj ET"
                         for size, x, y, text in items).encode("latin-1")

    pages = [
        placed([(20, 50, 780, "Smart Jump Sample"),
                (11, 50, 740, "As Figure 2 shows, the made-up beams bend."),
                (11, 50, 722, "The values are listed in Table 1 on the next page."),
                (11, 50, 704, "The load follows from Eq. (3) in the method."),
                (11, 50, 686, "This was first measured by Garner and Holt (2019)."),
                (11, 50, 668, "Earlier work [4] found the same."),
                (11, 50, 650, "Section 2 gives the method.")]),
        placed([(16, 50, 780, "2 Method"),
                (11, 50, 750, "The made-up load is found from the stiffness."),
                (11, 250, 720, "F = k x"),
                (11, 500, 720, "(3)"),
                (11, 50, 680, "Table 1: Made-up values for each beam."),
                (11, 50, 662, "Beam A 12"),
                (11, 50, 644, "Beam B 15")]),
        placed([(11, 50, 780, "The picture below is made up."),
                (11, 50, 420, "Figure 2: A made-up beam bending under load."),
                (11, 50, 380, "Figure 2 is drawn above.")])
        + b"\nq 300 0 0 250 100 450 cm /Im1 Do Q",
        placed([(16, 50, 780, "References"),
                (9, 50, 755, "[1] A. Sample, Made-up beams, 2001."),
                (9, 50, 741, "[2] B. Sample, Made-up beams again, 2003."),
                (9, 50, 727, "[3] C. Sample, Yet more made-up beams, 2005."),
                (9, 50, 713, "[4] D. Example, More made-up beams, 2010,"),
                (9, 62, 699, "pages 1 to 10."),
                (9, 50, 675, "Garner, P. and Holt, R. (2019) Bending of example beams."),
                (9, 50, 661, "Lane, T. (2020) Another made-up study.")]),
    ]

    width = height = 64
    pixels = bytes((40 if 24 <= y < 40 else 160) for y in range(height) for _ in range(width))
    image = zlib.compress(pixels)
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        30: b"<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 8 "
            b"/Filter /FlateDecode /Length %d >>\nstream\n%s\nendstream" % (width, height, len(image), image),
    }
    kids = []
    for index, stream in enumerate(pages):
        number = 4 + index * 2
        kids.append(number)
        objects[number] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents %d 0 R "
                           b"/Resources << /Font << /F1 3 0 R >> /XObject << /Im1 30 0 R >> >> >>" % (number + 1))
        objects[number + 1] = b"<< /Length %d >>\nstream\n%s\nendstream" % (len(stream), stream)
    objects[2] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (
        b" ".join(b"%d 0 R" % kid for kid in kids), len(kids))

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
    issue_without_contents_pdf(out / "issue_without_contents.pdf")
    reader_epub(out / "reader_nav.epub", "nav")
    reader_epub(out / "reader_ncx.epub", "ncx")

    drawing_only_pdf(out / "scanned_no_text.pdf")
    picture_pdf(out / "picture_page.pdf")
    smart_jump_pdf(out / "smart_jump.pdf")
    smart_jump_headings_pdf(out / "smart_jump_headings.pdf")
    reading_ruler_pdf(out / "reading_ruler.pdf")
    reading_table_pdf(out / "reading_table.pdf")
    reading_page_ends_pdf(out / "reading_page_ends.pdf")
    reading_figures_pdf(out / "reading_figures.pdf")
    smart_jump_contents_pdf(out / "smart_jump_contents.pdf")
    judgment_pdf(out / "judgment.pdf")
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
