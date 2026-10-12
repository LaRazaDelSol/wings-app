"""Emit the 75 research test vectors (docs/research/links-grouping.md 1.8) as C++ rows
for tests/unit/links_test.cpp, with the reference parser's full output.

Usage: python -I gen_vectors.py <library.json> > vectors.inc
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from linkparse import parse_link, host_of_url  # noqa: E402

WANT = [
    'USA 12.xx Base - Vikingfile part 5', 'EUR 9.xx Base - Akirabox', 'JPN 11.xx Base - Rootz part 3',
    'ASIA 9.xx Base - Akirabox part 2', 'HK 10.xx Base - Rootz', 'EUR 5.00 Base - Mega',
    'EUR 7.61 Base - 1fichier part 1', 'USA 13.60 Game (v01.010.000) (All DLC) - Vikingfile part 1',
    'EUR 6.xx BackPort 4.xx - Vikingfile', 'USA 7.xx BackPort 4.xx - 1fichier part 3',
    'USA 12.40 BackPort 4.xx - Rootz', 'EUR 9.xx BackPort 7.xx - Gofile', 'EUR BackPort 4.xx - Vikingfile',
    'USA 11.xx BackPort - Akirabox', 'EUR 11.xx BackPort - Vikingfile part 1',
    'EUR exFAT 01.000 Base - VIKI (exFAT)', 'USA exFAT 01.006 Base - FileK (exFAT)',
    'JPN exFAT 01.060 Base - Vault (exFAT)', 'EUR exFAT 01.002 Base - AKIA (exFAT)',
    'USA exFAT 01.001 Base - DATA (exFAT)', 'JPN to EUR Languages exFAT 01.000.300 Base - FileK Std (exFAT)',
    'USA to EUR Languages exFAT 01.011 Base - AKIA (exFAT)',
    'EUR exFAT 01.000 BackPort backport - AKIA Backport (exFAT)',
    'USA to EUR Langs exFAT 01.008.001 BackPort backport - DATA Back',
    'exFAT v01.052 Base - Vikingfile part 1', 'exFAT v01.000.003 Base - Vikingfile',
    'EUR v0.2.5.3 Base - Filecrypt part 1', 'Release 1 Base - Akirabox',
    'Release 2 DLCs (PKG – Psykzhack) - Akirabox part 1', 'USA 10.xx DLC (@Pkykzhack) - Vikingfile',
    'USA 10.01 DLC (@Pkykzhack) - Akirabox', 'EUR 10.xx Only DLCs (PKG – DUPLEX) - Akirabox',
    'USA 9.xx Only DLCs (EMU – Pippo) - 1fichier', 'USA 7.xx Game (v01.000)(DLC) - Rootz part 3',
    'USA 12.xx Game (v01.055) (All DLCs) (APR-EMU) - Vikingfile part',
    'USA 12.xx Game (v01.004.003) [APR-EMU] [ ALL DLC ] - Mediafire ',
    'EUR 12.xx Game (v01.005.000) (DLC Deluxe Edition) - Akirabox pa',
    'USA 12.xx Game (v17.661.002) (DLC Premium Edition) - Akirabox p', 'USA 12.xx DLC Multiplayer - Rootz part 1',
    'USA 9.xx Update 01.003 - Akirabox', 'EUR DLC Unlocker - Vikingfile', 'USA 10.xx DLC 1 (@Pkykzhack) - Akirabox',
    '4.xx PPSA26344 – v1.008.000 – DLC – 95.9 gb – 4.xx – ', 'DATA (exFAT)', 'Filecrypt',
    'Gofile', 'AkiraBox', 'Fpkg - VikingFile', 'Fpkg Backport - DataNodes', 'Fpkg Standard - Filed',
    '4.xx BackPort - DataNodes', '7.xx BackPort - Vault', 'Standard 9.xx - BuzzHeavier', 'Dump - AkiraBox',
    'Fix - DataNodes', 'DLC - Filek', 'Viki', 'Archive', 'Akirabox - pt.7', 'Akirabox - 4.xx+ - pt.5',
    'Viki - DLC', 'Viki (+Arabic Lang)', 'Viki - Backport - 4.xx+', '1fichier (Backport 4.xx)',
    'Archive (Game + DLC)', 'Archive - 5xx (Game + DLC)', 'Archive - FW 10.xx - 10.xx+', '1fichier- 5.xx Backport',
    'Data - DLC by Psykzhack - DLC', 'FULL GAME UNLOCK (DUPLEX – PKG) - DLC', 'BASE GAME - akirabox.com',
    'BASE GAME + DLC - akirabox.to', 'FPKG BACKPORT - akirabox.to', 'Game - vik1ngfile.site',
    'DLC - fuckingfast.net',
]


def c(s):
    if s is None:
        s = ""
    out = '"'
    for b in str(s).encode("utf-8"):
        ch = chr(b)
        if b >= 0x80:
            out += "\\%03o" % b
        elif ch in '"\\':
            out += "\\" + ch
        else:
            out += ch
    return out + '"'


def bl(v):
    return "true" if v else "false"


KIND = {"base": "LinkKind::base", "game+dlc": "LinkKind::game_dlc", "dlc": "LinkKind::dlc",
        "update": "LinkKind::update", "fix": "LinkKind::fix"}
FMT = {None: "LinkFormat::none", "exFAT": "LinkFormat::exfat", "FPKG": "LinkFormat::fpkg", "PKG": "LinkFormat::pkg",
       "Dump": "LinkFormat::dump", "FFPFS": "LinkFormat::ffpfs"}
FROM = {None: "HostSource::none", "name": "HostSource::name", "url": "HostSource::url"}

lib = json.load(open(sys.argv[1], encoding="utf-8"))["packages"]
urls = {}
for p in lib:
    for l in p["downloadLinks"]:
        urls.setdefault(l["name"], l["url"])
n = 0
for w in WANT:
    if w not in urls:
        continue
    n += 1
    f = parse_link(w, urls[w])
    print("    {%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %d, %d, %s, %s, %s, %s, %s, %s, %s, %s, %s}," % (
        c(w), c(host_of_url(urls[w])), c(f["region"]), c(f["lang_from"]), c(f["lang_to"]), c(f["fw"]), bl(f["fw_plus"]),
        bl(f["backport"]), c(f["backport_target"]), KIND[f["kind"]], bl(f["kind_inferred"]), bl(f["all_dlc"]),
        FMT[f["format"]], bl(f["build"]), c(f["version"]), -1 if f["release"] is None else f["release"],
        -1 if f["part"] is None else f["part"], bl(f["part_unknown"]), bl(f["truncated"]), c(",".join(f["tags"])),
        c(f["packager"]), c(f["host"]), FROM[f["host_from"]], c(f["size_text"]), c(f["title_id"]), c(f["label"])))
print("// %d vectors" % n)
