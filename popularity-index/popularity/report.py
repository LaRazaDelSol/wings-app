"""report.md generator: coverage per source, low-confidence list, unmatched list."""
from .matcher import ACCEPT


def render(entries, results, records, providers, generated_at, skipped=None):
    """results: {titleId: {source: Signal | None | 'error' | 'ignored'}}"""
    n = len(entries)
    lines = ["# Popularity build report", "", "Generated: %s" % generated_at, "",
             "Titles: %d, with a score: %d (%.1f%%)" % (n, len(records), 100.0 * len(records) / n if n else 0), ""]
    for name, why in sorted((skipped or {}).items()):
        lines.append("Provider `%s` skipped: %s" % (name, why))
    if skipped:
        lines.append("")
    lines += ["## Coverage per source", "", "| source | matched | high conf (>=%.2f) | low conf | errors | coverage |"
              % ACCEPT, "|---|---:|---:|---:|---:|---:|"]
    for p in providers:
        matched = [r[p] for r in results.values() if hasattr(r.get(p), "confidence")]
        hi = sum(1 for s in matched if s.confidence >= ACCEPT)
        err = sum(1 for r in results.values() if r.get(p) == "error")
        lines.append("| %s | %d | %d | %d | %d | %.1f%% |" % (
            p, len(matched), hi, len(matched) - hi, err, 100.0 * len(matched) / n if n else 0))
    low, unmatched = [], []
    by_id = {e.title_id: e for e in entries}
    for tid in sorted(results):
        res = results[tid]
        for p in providers:
            r = res.get(p)
            if hasattr(r, "confidence") and r.confidence < ACCEPT:
                low.append((tid, p, r))
        if not any(hasattr(res.get(p), "confidence") for p in providers if p != "pic0"):
            unmatched.append(tid)
    lines += ["", "## Low confidence (%d) -- fix with overrides.json" % len(low), ""]
    if low:
        lines += ["| titleId | title | source | matched to | conf | ambiguous |", "|---|---|---|---|---:|---|"]
        for tid, p, s in low:
            lines.append("| %s | %s | %s | %s | %.2f | %s |" % (
                tid, by_id[tid].title.replace("|", "/"), p, str(s.external_name).replace("|", "/"),
                s.confidence, "yes" if s.ambiguous else ""))
    else:
        lines.append("None.")
    lines += ["", "## Unmatched in every source (%d)" % len(unmatched), ""]
    lines += ["- %s  %s" % (t, by_id[t].title) for t in unmatched] or ["None."]
    return "\n".join(lines) + "\n"
