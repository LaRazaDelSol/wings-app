# Link types and what Best means

A card on a game page is one link, or one whole multi-part set on one host.

## What a card shows

Host, region, the firmware it needs (and the lowest firmware a back-port runs on), format (exFAT, FPKG, PKG), version, how many files, and how it will download. Where a link points at a known wrapper page, the host is taken from the link's name so you see the real file host.

## Link types

| Type | Meaning |
|---|---|
| Native `.pkg` | Handed to Pegasus as a package; the first choice. |
| Debrid (yellow) | A ready debrid account (Real-Debrid, TorBox) takes it. |
| Automatic (green) | A provider Pegasus can download from without you. |
| Browser | Needs a check in the PS5 browser; Pegasus captures the result. |
| Unknown host (neutral) | Wings does not recognise the host. |
| Not supported (red) | Pegasus cannot download it. |

The legend is on the page. Colours follow the Pegasus web UI: green automatic or supported, yellow debrid, neutral unknown, red not supported.

## What Best means

Best is a recommendation, not a rule. Wings compares your links with your **Link ranking** settings (preferred firmware, region and format, prefer debrid, prefer single file) and picks by the first difference that matters, in this order:

1. A link that needs a **newer firmware** than yours is never Best while anything else exists.
2. **Tier:** native `.pkg`, then debrid, then automatic, then browser capture, then unknown, then not supported. (With "Prefer debrid links" off, debrid sits between automatic and browser.)
3. **Firmware fit:** it runs on yours, then unknown.
4. **Region:** your preferred one (or no preference), then unknown, then others.
5. **Format:** your preferred one, then others.
6. **Parts:** a single file, then a complete set, then sets with gaps; fewer parts first.
7. A known package size before an unknown one.
8. The newest version.
9. Source order.

The card shows the result and the reason, for example `Debrid · USA · 12.xx · exFAT · 1 file`. If you have not set a preferred firmware, Wings shows firmware but does not rank by it.

Best is chosen over the **Base** section of the page. Updates and DLC are ranked inside their own sections.
