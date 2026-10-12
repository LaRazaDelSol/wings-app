"""Generate the accent/compatibility folding table used by game_group.cpp (norm_title).

Each row maps one code point to lower(NFKD(c) minus combining marks), with any
non-ASCII character of the result written as a space. Code points whose result
is blank are left out (the C++ side turns every unknown non-ASCII character into
a space, which is what the reference does at its step 7). U+2122 and U+00AE are
handled before the table (the reference replaces them with a space before NFKD).
Usage: python -I gen_fold_table.py > fold_table.inc
"""
import unicodedata

RANGES = [(0xA0, 0x250), (0x1E00, 0x1F00), (0x2000, 0x2190), (0x2460, 0x24EA),
          (0xFB00, 0xFB07), (0xFF01, 0xFF5F)]
for lo, hi in RANGES:
    for cp in range(lo, hi):
        c = chr(cp)
        if c in "™®":
            continue
        t = unicodedata.normalize("NFKD", c)
        t = "".join(x for x in t if not unicodedata.combining(x)).lower()
        if cp == 0x2019:
            t = "'"  # the reference maps the right single quote to an apostrophe
        t = "".join(x if ord(x) < 128 else " " for x in t)
        if t.strip() == "":
            continue
        esc = t.replace("\\", "\\\\").replace('"', '\\"')
        print('    {0x%04X, "%s"},' % (cp, esc))
