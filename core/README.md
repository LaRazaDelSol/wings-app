# Wings core

Host-compilable C++17 library that models and talks to Pegasus DL's HTTP API (tested against
Pegasus **1.11.0**). No PS5 SDK, no third-party dependencies. GPL-3.0-or-later. The Wings UI is meant to
see only `wings/model.hpp` and `wings/pegasus.hpp`; `src/pegasus_v1.cpp` is the only file that knows
Pegasus routes and JSON.

```
UI -> model.hpp / logic.hpp -> IPegasus (pegasus.hpp) -> PegasusV1 (pegasus_v1.cpp) -> http::Transport
                                                                                       |- SocketTransport (http.cpp)
                                                                                       `- ReplayTransport (tests)
```

## Layout

| Path | What |
|---|---|
| `include/wings/model.hpp` | Plain structs and enums: Package, DownloadLink, Source, DefaultSource(s), QueueJob (full field set), QueueSummary, JobStatus/JobPhase, DispatchResult, QueueResponse, ResolveProbe, DebridStatus, Settings, CaptureStatus, Mount/Location, FileEntry, FileOperation, LogEntry, InstalledGame + AprEmu, AprJob, DeleteJob, Capabilities, ApiError |
| `include/wings/pegasus.hpp` | `IPegasus` (one method per route/flow), `Result<T>`, `connect()`, `probeCapabilities()`, `Compat` |
| `include/wings/pegasus_v1.hpp`, `src/pegasus_v1.cpp` | `PegasusV1`: adapter for Pegasus 1.x |
| `include/wings/logic.hpp`, `src/logic.cpp` | Ports of the web-UI JavaScript that is not in the backend (each function cites `app.js:NNN` / `fm.js:NNN`) |
| `src/connect.cpp` | Version selection (`/api/health`) and the GET-only capability probe |
| `include/wings/http.hpp`, `src/http.cpp` | `Transport` interface + `SocketTransport` (adapted from Orbit's http.cpp; POSIX sockets, winsock shim for host builds) |
| `include/wings/replay_transport.hpp` | Offline transport: scripted responses, records every request (method, path, body) |
| `include/wings/json.hpp`, `src/json.cpp`, `json_writer.hpp` | Orbit's small JSON DOM reader (renamed, `\b`/`\f` quoting added) + a JSON.stringify-shaped body builder |
| `tests/` | Plain C++ asserts (`test_harness.hpp`), 55 tests / ~3900 checks (`test_stream.cpp`: array-wise catalogue reading) |
| `tools/smoke.cpp` | Read-only live smoke tool (GET only) |

## Build and test

Needs only a C++17 compiler.

```bash
# Git Bash / MSYS2 (mingw g++ 15 was used) or any Linux/WSL with g++ or clang++:
cd wings/core
./run_tests.sh                 # builds build/wings_core_tests and runs it; exit code 0 = all green
./run_tests.sh write_          # run only tests whose name contains "write_"
CXX=clang++ ./run_tests.sh     # pick another compiler
./run_tests.sh --build-smoke   # builds build/wings_smoke instead of running tests
```

CMake file provided but NOT verified on this machine (no make/ninja installed) (`cmake -S . -B build && cmake --build build && ctest --test-dir build`); `WINGS_CORE_TESTS`
and `WINGS_CORE_SMOKE` options toggle the executables. Tests must run with `wings/core` as the working
directory (the script and CTest do this); they read `../../recon/samples/*` and `tests/fixtures/settings.json`.
Set `WINGS_SAMPLES=/path/to/recon/samples` to relocate the samples.
The recorded samples are third-party catalogue data and are not part of this repository; capture your own from a Pegasus DL you run (see `tools/pegasus_contract.py`), or the tests that need them will skip or fail.

Note: on the dev machine the Bash tool sandbox made `g++` fail silently; run the script from PowerShell
(`bash run_tests.sh`) or a normal terminal. WSL Ubuntu on this machine has no g++ installed
(`apt install g++` first); MSVC was not tried.

Live smoke (read-only, GET requests only):

```bash
./run_tests.sh --build-smoke && build/wings_smoke 10.0.0.113 6970 --library
```

## Using it

```cpp
auto transport = std::make_shared<wings::http::SocketTransport>("127.0.0.1", 6970);
wings::Connection c = wings::connect(transport);          // health -> adapter -> capability probe
if (!c.usable()) { /* c.compat == unreachable | unsupported, c.message explains */ }
if (c.caps.queue) {
    auto q = c.api->getQueue();                            // Result<QueueSnapshot>
    if (!q.ok()) show(q.error.status, q.error.message);    // {status, message} from {"error": ".."}
}
// Activating a download link, exactly like the Pegasus web UI:
auto action = wings::logic::decideLinkAction(pkg, linkIndex, settings);
switch (action.kind) { case wings::logic::LinkActionKind::nativePkg: c.api->enqueuePackage(pkg.id, action.linkIndex); ... }
```

All `IPegasus` calls are synchronous and blocking: call from a worker thread.

### Errors

`Result<T>{ApiError error; T value;}`. `ApiError::kind` is `transport` (status 0), `http` (non-2xx; `message` is the
`error` field of the body, else `"HTTP <status>"`), `parse` (2xx but not JSON), or `client` (rejected locally by
the same validation the web UI does, e.g. empty password, connections outside 1..64; nothing was sent).
`error.route_missing()` is Pegasus' `404 {"error":"Route not found"}`.

### Capability probe and degradation

`connect()` reads `/api/health`, parses `version`, and:

- 1.11.x -> `PegasusV1`, `Compat::tested`; other 1.x -> `PegasusV1`, `Compat::untestedMinor` (features probed anyway);
- any other major or unparsable version -> `api == nullptr`, `Compat::unsupported`, all feature flags false;
- health unreachable -> `Compat::unreachable` with the transport error.

It then issues **GET-only** probes (settings, sources, default-sources, queue, mounts, browse `/`, logs with a huge
`since`, capture/active, `/appdb`, apr-emu/versions, files/operations, and the three `*/status?titleId=WINGS-PROBE`
routes that answer a bogus title with a normal error). A probe that fails clears that flag and appends a line to
`caps.notes`; nothing throws. `/api/library` (7 MB) is not probed: `caps.library` is inferred from `/api/sources`.

**APR Emu manifest 503.** The user's console answers `GET /api/apr-emu/versions` with
`503 {"error":"APR Emu manifest has too many versions"}`. That is surfaced as `caps.aprEmuVersions = false` plus a note,
while `caps.aprEmu` stays true (the update/restore status routes exist), so "update" still works and only the
"reinstall a specific version" picker is disabled. Verified live: `wings_smoke` reports
`aprEmu=1 aprEmuVersions=0` with the note.

## UI logic ported from the Pegasus JS

`logic.hpp` (tests in `tests/test_logic.cpp`): provider classification (`downloadSupportForUrl`: automated /
direct-browser / unsupported / unknown, host tables, `matchSubdomains`, `guarded`, `requiresBrowser`), debrid host
matching (`debridServicesForUrl`, `debridReady`), the whole `queueDownloadLink` decision tree (`decideLinkAction`:
`.pkg` -> native handoff, then debrid, then provider tables), base64 download-URL decoding (`downloadLinkUrl`,
with `atob` semantics), `nativeDispatchResults` / `queuedDispatchResults` / `finishDispatchResponse`, manual-link
probe validation, capture status text, queue status sets and poll delay, APR state and version ordering, firmware text
from the hex `requiredSystemSoftwareVersion`, and file-manager path/name rules.

Deliberate differences from the JS (all about `new URL()` / the DOM):

- `safeHttpUrl` and friends parse only **absolute** `http(s)` URLs. The JS resolves relative input against
  `window.location`; here relative or empty input gives `""` (a relative `foo.pkg` therefore is not a native `.pkg`).
- No IDNA/punycode and no percent-decoding of hosts; such hosts are kept lower-cased as-is. Path/query/fragment get the
  WHATWG percent-encode sets and dot-segment removal.
- `localeCompare` (tie-break in APR version sort) is a byte comparison.
- The `description` HTML sanitiser (`sanitizeDescriptionHtml`) and optimistic queue edits are UI concerns and not ported.

## What the tests prove

- Every sample in `recon/samples/*.json` and `recon/samples/mutating/*.json` parses through `PegasusV1` with field-level
  assertions; the 7 MB `library.json` (2962 packages, 35559 links) parses in ~90 ms on the dev PC (asserted < 3 s).
- A request-shape test per write route compares **exact method, path and body bytes** with what the JS builds
  (`JSON.stringify` key order, compact; `encodeURIComponent` for query strings; `URLSearchParams` for the upload query).
- Tolerant parsing: missing/mistyped/unknown fields, empty bodies, non-JSON error bodies, deep nesting, transport failures.
- `connect()` against a user-shaped console, an untested minor, an unsupported major, an unreachable console, a stripped
  Pegasus where every route but `/api/health` and `/api/queue` is missing, a mid-probe transport drop, and the APR 503.

Nothing in the test-suite or smoke tool performs a write against a live console. The only writes ever sent live were the
sandboxed ones in `tools/pegasus_contract.py` (recorded in `recon/samples/mutating`).

## Large catalogue: `streamLibrary`

`IPegasus::streamLibrary(PackageSink&)` reads `GET /api/library` array-wise (`json::for_each_array_element`): each
package is parsed on its own and handed to the sink, so no DOM of the 7 MB reply (about 36 MB) is built. The Wings app
uses it and keeps only what its UI needs (see `docs/wings-app-skeleton.md`; peak about 16 MiB for the real sample).

## Known gaps

See the report that accompanied this code; in short: no streaming upload/download (bodies are in memory),
no async layer or cancellation of in-flight requests, success-reply shapes of several routes were never recorded
(`sources/url`, `sources/refresh`, `queue/{resume,retry,...}`, `default-sources/apply`, `debrid/refresh`, `browser/open`)
so those parsers are best-effort and tolerant, HTTPS is unsupported by `SocketTransport` (Pegasus is plain HTTP), and the
PS5 build of `SocketTransport` is unverified (it is Orbit's POSIX code plus a host-only winsock shim).
