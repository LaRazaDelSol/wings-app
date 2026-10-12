"""Reference 'Best' ranking for one game page section. Candidates are mirrors:
a single link, or one multipart set on one host (group_parts)."""
TIER_RANK = {"native_pkg": 0, "debrid": 1, "automated": 2, "browser": 3, "unknown": 4, "unsupported": 5}
TIER_TEXT = {"native_pkg": "PKG to PS5", "debrid": "Debrid", "automated": "Automatic",
             "browser": "Browser", "unknown": "Unknown host", "unsupported": "Not supported"}
TIER_WHY = {"native_pkg": "goes straight to the PS5 downloads", "debrid": "debrid: fast, no captcha",
            "automated": "Pegasus fetches it by itself", "browser": "needs the PS5 browser",
            "unknown": "unknown host", "unsupported": "not supported"}

DEFAULT_PREFS = {"firmware": None,       # e.g. "10.xx"; None = not set (fw is shown, not ranked)
                 "region": None,         # "USA"/"EUR"/"JPN"/"ASIA"/...; None = any
                 "format": None,         # "exFAT"/"FPKG"; None = any
                 "prefer_debrid": True,  # False moves debrid to after automated
                 "prefer_single_file": True,
                 "hidden_hosts": set(),
                 "show_incompatible": True}  # shown dimmed, never picked as Best


def fw_num(fw):
    a, b = fw.split(".")
    return int(a) + (0.99 if b == "xx" else int(b) / 100.0)


def fw_fit(cand_fw, user_fw):
    """0 runs, 1 unknown, 2 too new for this console."""
    if not user_fw or not cand_fw:
        return 1 if user_fw else 0
    # '10.xx' needs at least 10.00; the console '10.xx' counts as 10.99
    need = int(cand_fw.split(".")[0]) + (0 if cand_fw.endswith("xx") else int(cand_fw.split(".")[1]) / 100.0)
    return 0 if need <= fw_num(user_fw) else 2


def ver_tuple(v):
    return tuple(int(x) for x in v.split(".")) if v else ()


def rank_key(c, prefs):
    """c: dict(tier, fw (effective min fw), region, format, parts (0 = single), set_status,
    size_known, version, source_order, index)."""
    tier = TIER_RANK[c["tier"]]
    if not prefs["prefer_debrid"] and c["tier"] == "debrid":
        tier = 2.5
    fit = fw_fit(c["fw"], prefs["firmware"])
    region = 0 if not prefs["region"] or c["region"] == prefs["region"] else (1 if c["region"] else 0.5)
    fmt = 0 if not prefs["format"] or c["format"] == prefs["format"] else 1
    parts_class = {"single": 0, "complete": 1, "restarts": 2, "unnumbered": 2, "gaps": 3}[c["set_status"]]
    parts = (parts_class, c["parts"]) if prefs["prefer_single_file"] else (parts_class >= 2, 0)
    return (fit == 2, tier, fit, region, fmt, parts, not c["size_known"],
            tuple(-x for x in ver_tuple(c["version"])), c["source_order"], c["index"])


def best(cands, prefs=DEFAULT_PREFS):
    pool = [c for c in cands if c["host"] not in prefs["hidden_hosts"]]
    if not pool:
        return None, ""
    b = min(pool, key=lambda c: rank_key(c, prefs))
    chips = [TIER_TEXT[b["tier"]]]
    if b["region"]:
        chips.append(b["region"])
    if b["fw"]:
        chips.append(("BP " if b.get("backport") else "") + b["fw"])
    if b["format"]:
        chips.append(b["format"])
    chips.append("1 file" if b["parts"] == 0 else f'{b["parts"]} parts')
    why = [TIER_WHY[b["tier"]]]
    fit = fw_fit(b["fw"], prefs["firmware"])
    if prefs["firmware"]:
        why.append({0: f'runs on your {prefs["firmware"]}', 1: "firmware not stated",
                    2: f'needs {b["fw"]}, newer than your {prefs["firmware"]} (no compatible link)'}[fit])
    if prefs["region"] and b["region"] == prefs["region"]:
        why.append("your region")
    if b["parts"] == 0:
        why.append("single file")
    if b["version"]:
        why.append("newest version" if all(ver_tuple(b["version"]) >= ver_tuple(c["version"]) for c in pool)
                   else f'v{b["version"]}')
    return b, "Best: " + " · ".join(chips) + " — " + ", ".join(why)
