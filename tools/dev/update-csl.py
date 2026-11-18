#!/usr/bin/env python3
"""Fetches the CSL styles and locales that come with Papertrail into data/csl/, at the commits pinned below.

Run it with `python -I tools/dev/update-csl.py`. To move to newer styles or locales, change the commits here and
in data/csl/SOURCE.md, run it again, and check the tests in test_csl still pass. A dependent style's parent is
fetched too, so every style here can be rendered without anything else.
"""
import io
import pathlib
import re
import sys
import tarfile
import urllib.request

STYLES_COMMIT = "7a37c5a736c10fbd468bc31f92cbc6eafaa2173d"  # citation-style-language/styles, v1.0.2 branch
LOCALES_COMMIT = "f637c83f6267b761513bcc1cb0e95571172e768c"  # citation-style-language/locales, v1.0.2 branch

# Each style's file name in the styles repository, without ".csl". Dependent ones are in its "dependent" folder.
STYLES = [
    "apa",
    "american-medical-association",
    "nlm-citation-sequence",
    "dependent/vancouver-nlm",
    "modern-language-association",
    "chicago-note-bibliography",
    "chicago-fullnote-bibliography",
    "turabian-fullnote-bibliography",
    "nature",
    "science",
    "cell",
    "plos",
    "bmj",
    "the-lancet",
    "annual-reviews",
    "elsevier-with-titles",
    "elsevier-harvard",
    "springer-basic-brackets",
    "springer-basic-author-date",
    "taylor-and-francis-chicago-author-date",
    "american-chemical-society",
    "royal-society-of-chemistry",
    "american-institute-of-physics",
    "american-physics-society",
    "ieee",
    "association-for-computing-machinery",
    "american-society-of-civil-engineers",
    "american-society-of-mechanical-engineers",
    "american-sociological-association",
    "american-political-science-association",
    "american-anthropological-association",
    "cse-name-year",
    "iso690-author-date-en",
    "din-1505-2",
    "oscola",
]

AGENT = "Papertrail-dev-tools (https://github.com/hachigodesu/Papertrail)"
MAX_BYTES = 8 * 1024 * 1024


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": AGENT})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        sys.exit("too large: " + url)
    return data


def style_url(name):
    return f"https://raw.githubusercontent.com/citation-style-language/styles/{STYLES_COMMIT}/{name}.csl"


PARENT = re.compile(rb'<link[^>]*rel="independent-parent"[^>]*>')
HREF = re.compile(rb'href="https?://www\.zotero\.org/styles/([a-z0-9-]+)"')


def main():
    root = pathlib.Path(__file__).resolve().parents[2] / "data" / "csl"
    styles_dir = root / "styles"
    locales_dir = root / "locales"
    for folder in (styles_dir, locales_dir):
        folder.mkdir(parents=True, exist_ok=True)
        for old in folder.iterdir():
            old.unlink()

    wanted = list(STYLES)
    done = set()
    while wanted:
        name = wanted.pop(0)
        if name in done:
            continue
        done.add(name)
        data = fetch(style_url(name))
        if not data.lstrip().startswith(b"<?xml"):
            sys.exit("not a style: " + name)
        (styles_dir / (name.split("/")[-1] + ".csl")).write_bytes(data)
        link = PARENT.search(data)
        if link:
            parent = HREF.search(link.group(0))
            if not parent:
                sys.exit("a parent that isn't a Zotero style: " + name)
            wanted.append(parent.group(1).decode())
        print("style", name)

    archive = fetch(f"https://github.com/citation-style-language/locales/archive/{LOCALES_COMMIT}.tar.gz")
    count = 0
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        for member in tar.getmembers():
            name = member.name.rsplit("/", 1)[-1]
            if member.isfile() and re.fullmatch(r"locales-[a-zA-Z-]+\.xml", name):
                (locales_dir / name).write_bytes(tar.extractfile(member).read())
                count += 1
            elif member.isfile() and name == "locales.json":
                (locales_dir / name).write_bytes(tar.extractfile(member).read())
    print(count, "locales")


if __name__ == "__main__":
    main()
