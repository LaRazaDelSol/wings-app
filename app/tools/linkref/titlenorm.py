"""Reference title normalisation + game grouping for Wings (phase 5)."""
import re, unicodedata, collections

ROMAN = {'ii': 2, 'iii': 3, 'iv': 4, 'vi': 6, 'vii': 7, 'viii': 8, 'ix': 9, 'xi': 11, 'xii': 12,
         'xiii': 13, 'xiv': 14, 'xv': 15, 'xvi': 16}          # i, v, x are left alone (words)
TYPO_NUM = {'ll': '2', 'lll': '3'}                              # 'Little Nightmares lll'
RISK = {'bundle', 'trilogy', 'collection', 'demo', 'trial', 'soundtrack', 'ost', 'dlc', 'dlcs',
        'season', 'vol', 'chapter', 'episode', 'unlock', 'pack', 'bonus'}
EDITION_TAIL = re.compile(
    r"\s+(?:(?:the\s+)?(?:digital\s+deluxe|game of the year|complete|definitive)|[a-z0-9]+)\s+edition$"
    r"|\s+(?:directors|final)\s+cut$|\s+(?:digital\s+)?deluxe$|\s+goty$")


def norm_title(t):
    t = t.replace('™', ' ').replace('®', ' ')          # before NFKD (TM -> 'TM')
    t = unicodedata.normalize('NFKD', t)
    t = ''.join(c for c in t if not unicodedata.combining(c)).lower()
    t = re.sub(r"\[[^\]]*\]|\([^)]*\)", " ", t)                  # [exFAT], (DLC), (PS1)
    t = re.sub(r"\bv?\d{1,2}\.\d{2,3}(?:\.\d{1,3})*\b", " ", t)  # 'v01.005' (Pippo titles)
    t = re.sub(r"(?<=[a-z])\.(?=[a-z])", "", t).replace('.', ' ')  # P.I. -> pi
    t = t.replace("&", " and ").replace("’", "'")
    t = re.sub(r"'s\b", "s", t).replace("'", "")
    t = re.sub(r"[^a-z0-9+]+", " ", t)                            # keep '+' (bundles: 'Kiwami 1 + 2')
    t = re.sub(r"\bps[45]\b", " ", t)
    toks = [str(ROMAN[w]) if w in ROMAN else TYPO_NUM.get(w, w) for w in t.split()]
    return " ".join(toks)


def loose_title(t):
    """norm_title minus ONE trailing edition phrase ('deluxe edition', 'the complete edition')."""
    n = norm_title(t)
    m = EDITION_TAIL.search(n)
    if m and m.start() > 0:
        n = n[:m.start()].strip()
    return n


def numbers(n):
    return {int(w) for w in n.split() if w.isdigit() and not 1900 <= int(w) <= 2100}


def risk_words(n):
    r = set(n.split()) & RISK
    if '+' in n.split():
        r.add('+')
    return r


def same_title_id_compatible(a, b):
    """Two loose titles under ONE titleId: keep together unless there is evidence they differ."""
    if numbers(a) != numbers(b):
        return False                     # Chapter 4 / Chapter 5, MGS 2 / MGS 3, Kiwami / Kiwami 2
    if risk_words(a) != risk_words(b):
        return False                     # 'Turok' vs 'Turok Trilogy Bundle'
    return bool(set(a.split()) & set(b.split()))   # F1 25 vs 'No Sleep For Kaname Date': no overlap


def group_games(packages):
    """packages: list of dict(id, titleId, title). Returns list of lists of packages."""
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        parent[find(a)] = find(b)

    by_tid = collections.defaultdict(list)
    for p in packages:
        find(p['id'])
        by_tid[(p['titleId'] or '').upper() or ('notid', p['id'])].append(p)
    # stage 1: titleId, split on evidence
    for ps in by_tid.values():
        for i in range(len(ps)):
            for j in range(i + 1, len(ps)):
                if same_title_id_compatible(loose_title(ps[i]['title']), loose_title(ps[j]['title'])):
                    union(ps[i]['id'], ps[j]['id'])
    stage1 = collections.defaultdict(list)
    for p in packages:
        stage1[find(p['id'])].append(p)
    # stage 2: join clusters (other titleIds = other regions/SKUs) whose canonical loose title is equal
    by_canon = collections.defaultdict(list)
    for root, ps in stage1.items():
        c = collections.Counter(loose_title(p['title']) for p in ps)
        canon = sorted(c.items(), key=lambda kv: (-kv[1], len(kv[0])))[0][0]
        if canon:
            by_canon[canon.replace(' ', '')].append(root)
    for roots in by_canon.values():
        for r in roots[1:]:
            union(r, roots[0])
    out = collections.defaultdict(list)
    for p in packages:
        out[find(p['id'])].append(p)
    return list(out.values()), stage1
