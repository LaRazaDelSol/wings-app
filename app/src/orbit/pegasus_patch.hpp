// Wings TV app - Experimental: byte-level patches to a verified pegasus_dl.elf (see pegasus_patch.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Off by default everywhere. Nothing here runs unless the caller explicitly asks for a named patch
// set, and every patch is matched by an exact source SHA-256 plus a byte pattern that must occur
// EXACTLY once in the file; anything else (wrong version, pattern missing, pattern not unique) is
// refused, never guessed. The caller always has the original verified bytes and can fall back to
// sending those unchanged.
//
// This exists to test one hypothesis (docs/research/pegasus-dl-patch.md): Pegasus DL's segmented
// downloader writes each range's buffer with a direct pwrite() on the thread that also drives every
// socket in the batch, so a slow write can stall transfers that have nothing to do with it. Orbit
// Store decouples this with a ring buffer and a dedicated writer thread. The patches below are a
// diagnostic, not a redistributed binary: Wings never ships a patched pegasus_dl.elf, it only patches
// the player's own already-downloaded, already-verified copy, in memory, when the player turns this
// on in an experimental build.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::pegasus_patch
{

// One edit: `pattern` (with `mask`, 'x' = must match, '?' = any byte) must occur in `data` exactly
// once, at a file offset inside the loaded .text segment; `replacement` is written over the matched
// bytes (same length). `source_sha256` is the stock pegasus_dl.elf this was derived against; a file
// with a different hash never reaches the matcher, so a future Pegasus release always falls back to
// its own stock bytes until someone re-derives and adds a new entry (docs/research/pegasus-dl-patch.md
// says how).
struct Patch
{
    std::string id;             // "p0-segment-buffer-256k", stable, goes in the cache file name
    std::string description;    // one line, shown in diagnostics
    std::string source_sha256;  // lower-case 64 hex; the exact stock file this patch targets
    std::string pattern_hex;    // the bytes to find, hex, no spaces ("48 81 FF.." -> "4881ff..")
    std::string mask;           // same length as pattern_hex/2, one char per byte: 'x' or '?'
    std::string replacement_hex; // same length as pattern_hex; what to write over the match
};

// The built-in catalogue. One entry today (P0, the 256 KiB segment buffer cap); new entries are
// appended as new Pegasus releases are checked, never edited in place once shipped (the source hash
// keys each one, so stale entries are simply never matched again).
const std::vector<Patch> &builtin_catalogue();

struct Result
{
    bool changed = false;              // true only if at least one patch matched and was applied
    std::vector<std::string> applied;  // ids, in catalogue order
    std::string error;                 // set only when `changed` could not become true as asked
};

// Applies every patch in `catalogue` whose `source_sha256` matches sha256_hex(data) to a COPY of
// `data` (the input is never modified). A patch whose pattern is absent or not unique in `data` is
// skipped, not an error, unless `require_all` is true (used by the unit test, not by the manager).
// Returns the possibly-patched bytes; `out_result` (optional) says what happened.
std::vector<std::uint8_t> apply(const std::vector<std::uint8_t> &data, const std::vector<Patch> &catalogue,
                                 Result *out_result = nullptr, bool require_all = false);

// Decodes one hex string ("4881ff") into bytes; false on odd length or a non-hex character.
bool hex_decode(std::string_view hex, std::vector<std::uint8_t> *out);

// ---- P1: adds the writer-thread module (docs/research/pegasus-dl-patch.md) ----
//
// A `Patch` is a same-length byte swap; P1 needs more than that (bytes written into a verified
// cave and a PT_LOAD's sizes grown). `StructuralPatch` is the data for that kind
// of edit; `apply_structural` is the one, generic, hash-gated engine for it - every number in a
// `StructuralPatch` is re-checked against the live file before anything is touched, and any
// mismatch (wrong hash, a program header that doesn't look exactly as expected, a call site that
// isn't the exact instruction this was derived from) refuses that entry, same rule as `Patch`.
// Format v2 (additive; an empty `direct_call_redirects` is exactly the v1 behaviour).
// A direct-call redirect repoints an existing "E8 <rel32>" call (5 bytes, no NOP needed) at an
// entry point INSIDE the blob. P1 uses two of these to put its completion barrier in front of the
// host's fsync: both `call sync_segment_output` sites in download_segmented() (VA 0x4c1ae and
// 0x4c24a) are redirected to w1_sync, which drains the write ring, reports async failures, and
// only then tail-calls the original sync function.
struct DirectCallRedirect
{
    std::uint64_t va = 0;                 // VA of the E8 opcode, inside the text PT_LOAD
    std::uint64_t expected_target_va = 0; // the callee it must currently resolve to, or refuse
    std::uint64_t blob_entry_offset = 0;  // entry point, as a byte offset into the blob
};

struct StructuralPatch
{
    std::string id;
    std::string description;
    std::string source_sha256;

    // The PT_LOAD entry this grows (its index in the program header table) - checked against the
    // live file's e_phoff/e_phnum/e_phentsize, never assumed to be at a fixed byte offset.
    std::size_t phdr_index = 0;
    std::uint64_t expect_p_offset = 0, expect_p_vaddr = 0, expect_p_filesz = 0, expect_p_memsz = 0;

    // The one call site this redirects: an "FF 15 <disp32>" indirect call (6 bytes) at this VA,
    // which must currently resolve to `call_site_expected_target_va` or the patch refuses.
    // text_p_offset`/`text_p_vaddr` locate the PT_LOAD that VA lives in (to turn it into a file
    // offset), checked the same way.
    std::uint64_t call_site_va = 0, call_site_expected_target_va = 0;
    std::uint64_t text_p_offset = 0, text_p_vaddr = 0;

    // Already relocation-resolved code plus the zero-initialized g_w1 state object. The blob
    // overlays the verified all-zero padding immediately after this PT_LOAD's current file content;
    // the engine grows this PT_LOAD's p_filesz/p_memsz without shifting or truncating the ELF.
    std::string blob_hex;

    // P0 can be applied before or after P1. Accept the known P0-only hash as well as stock,
    // and ensure the P0 anchor is in the corresponding before/after state as part of P1.
    std::string source_sha256_p0;
    std::size_t p0_file_offset = 0;
    std::string p0_before_hex;
    std::string p0_after_hex;

    // Format v2: extra E8 call redirects into the blob (see DirectCallRedirect). Every site is
    // validated - exact opcode, exact original target, in-file, inside the blob, no overlap with
    // another edit - before ANY byte of the file is modified; one bad entry refuses the whole patch.
    std::vector<DirectCallRedirect> direct_call_redirects;
};

struct StructuralResult
{
    bool changed = false;
    std::string id;    // empty if nothing applied
    std::string error; // set only when require_all asked for a patch that could not apply
};

const std::vector<StructuralPatch> &builtin_structural_catalogue();

std::vector<std::uint8_t> apply_structural(const std::vector<std::uint8_t> &data,
                                           const std::vector<StructuralPatch> &catalogue,
                                           StructuralResult *out_result = nullptr, bool require_all = false);

} // namespace orbit::pegasus_patch
