class Entry:
    """One catalogue row. platform is 'PS5' (PPSA), 'PS4' (CUSA) or None."""

    def __init__(self, title_id, title, year=None):
        self.title_id, self.title, self.year = title_id, title, year
        self.platform = "PS5" if title_id.startswith("PPSA") else "PS4" if title_id.startswith("CUSA") else None


class Signal:
    """What one provider learned about one title. rating is 0-100, attention is a raw
    magnitude (daily views, review count, ...) that the blend turns into a percentile."""

    def __init__(self, source, external_id, external_name, confidence, ambiguous=False,
                 rating=None, rating_count=None, attention=None, attention_kind=None):
        self.source, self.external_id, self.external_name = source, external_id, external_name
        self.confidence, self.ambiguous = confidence, ambiguous
        self.rating, self.rating_count = rating, rating_count
        self.attention, self.attention_kind = attention, attention_kind


class Provider:
    """Common interface. lookup() returns a Signal or None (no match). Raising
    ProviderError marks a transient failure for that title only."""
    name = "base"

    def available(self):
        """(ok, reason). A provider that is not available is skipped, never an error."""
        return True, ""

    def lookup(self, entry, override=None):
        raise NotImplementedError


class ProviderError(Exception):
    pass
