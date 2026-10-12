"""Dump the reference parser's output over library.json as TSV, for diffing the C++ port.

Usage: python -I dump_golden.py <dir of linkparse.py/titlenorm.py> <library.json> <out dir>
Writes links.tsv, sets.tsv, titles.tsv, games.tsv (same format as tests/tools/links_dump.cpp).
"""
import json
import sys

sys.path.insert(0, sys.argv[1])
from linkparse import parse_link, group_parts  # noqa: E402
from titlenorm import norm_title, loose_title, group_games  # noqa: E402

lib = json.load(open(sys.argv[2], encoding="utf-8"))["packages"]
out = sys.argv[3]


def s(v):
    if v is None:
        return ""
    if v is True:
        return "1"
    if v is False:
        return "0"
    return str(v).replace("\t", " ").replace("\n", " ")


with open(out + "/links.tsv", "w", encoding="utf-8", newline="\n") as fl, \
        open(out + "/sets.tsv", "w", encoding="utf-8", newline="\n") as fs:
    for p in lib:
        for l in p["downloadLinks"]:
            f = parse_link(l["name"], l["url"])
            row = [p["id"], l["index"], f["region"], f["lang_from"], f["lang_to"], f["fw"], f["fw_plus"],
                   f["backport"], f["backport_target"], f["kind"], f["kind_inferred"], f["all_dlc"], f["format"],
                   f["build"], f["version"], f["release"], f["part"], f["part_unknown"], f["truncated"],
                   ",".join(f["tags"]), f["packager"], f["host"], f["host_from"], f["size_text"], f["title_id"],
                   f["label"], f["residue"]]
            fl.write("\t".join(s(x) for x in row) + "\n")
        g = group_parts([(l["index"], l["name"], l["url"]) for l in p["downloadLinks"]])
        for st in g["sets"]:
            fs.write("\t".join([s(p["id"]), s(st["host"]), ",".join(map(str, st["links"])),
                                ",".join(map(str, st["numbers"])), st["status"]]) + "\n")

games_in = [p for p in lib if p["downloadLinks"]]
with open(out + "/titles.tsv", "w", encoding="utf-8", newline="\n") as ft:
    for p in lib:
        ft.write("\t".join([s(p["id"]), norm_title(p["title"]), loose_title(p["title"])]) + "\n")
groups, _ = group_games(games_in)
lines = sorted(",".join(str(i) for i in sorted(p["id"] for p in g)) for g in groups)
with open(out + "/games.tsv", "w", encoding="utf-8", newline="\n") as fg:
    fg.write("\n".join(lines) + "\n")
print("links", sum(len(p["downloadLinks"]) for p in lib), "games", len(groups))
