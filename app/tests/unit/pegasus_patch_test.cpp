// Wings TV app - Experimental Pegasus DL ELF patcher: pattern matching, uniqueness, hash gating.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Uses small synthetic buffers, never the real pegasus_dl.elf (not a test fixture: it is the
// player's own downloaded, verified copy). docs/research/pegasus-dl-patch.md has the real anchor
// and the exact source hash the built-in catalogue targets.

#include "orbit/pegasus_patch.hpp"
#include "orbit/pegasus_manager.hpp" // sha256_hex

#include <gtest/gtest.h>

namespace orbit::pegasus_patch
{
namespace
{

std::vector<std::uint8_t> bytes(std::string_view hex)
{
    std::vector<std::uint8_t> out;
    EXPECT_TRUE(hex_decode(hex, &out));
    return out;
}

std::string hash_of(const std::vector<std::uint8_t> &data)
{
    return sha256_hex(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()));
}

Patch make_patch(const std::string &source_sha256)
{
    return Patch{"t0", "test patch", source_sha256, "deadbeef", "xxxx", "cafefeed"};
}

TEST(HexDecode, RoundTrips)
{
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(hex_decode("deadBEEF", &out));
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], 0xde);
    EXPECT_EQ(out[3], 0xef);
}

TEST(HexDecode, RejectsOddLengthOrBadChar)
{
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(hex_decode("abc", &out));
    EXPECT_FALSE(hex_decode("zz", &out));
}

TEST(PegasusPatchApply, AppliesAUniqueMatchOnlyWhenTheSourceHashMatches)
{
    std::vector<std::uint8_t> data = bytes("00112233deadbeef44556677");
    const std::string hash = hash_of(data);
    Result r;
    const std::vector<std::uint8_t> patched = apply(data, {make_patch(hash)}, &r);
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(r.applied, std::vector<std::string>{"t0"});
    EXPECT_EQ(patched, bytes("00112233cafefeed44556677"));
    // The input vector itself is never mutated.
    EXPECT_EQ(data, bytes("00112233deadbeef44556677"));
}

TEST(PegasusPatchApply, LeavesAWrongHashUntouched)
{
    std::vector<std::uint8_t> data = bytes("00112233deadbeef44556677");
    Result r;
    const std::vector<std::uint8_t> patched = apply(data, {make_patch(std::string(64, '0'))}, &r);
    EXPECT_FALSE(r.changed);
    EXPECT_TRUE(r.applied.empty());
    EXPECT_TRUE(r.error.empty());
    EXPECT_EQ(patched, data);
}

TEST(PegasusPatchApply, SkipsSilentlyWhenTheAnchorIsAbsent)
{
    std::vector<std::uint8_t> data = bytes("0011223344556677");
    const std::string hash = hash_of(data);
    Result r;
    const std::vector<std::uint8_t> patched = apply(data, {make_patch(hash)}, &r);
    EXPECT_FALSE(r.changed);
    EXPECT_TRUE(r.error.empty()); // not an error: this build just does not have that byte shape
    EXPECT_EQ(patched, data);
}

TEST(PegasusPatchApply, RefusesAnAnchorThatIsNotUniqueWhenRequireAllIsSet)
{
    // "deadbeef" occurs twice.
    std::vector<std::uint8_t> data = bytes("deadbeef0011deadbeef2233");
    const std::string hash = hash_of(data);
    Result r;
    const std::vector<std::uint8_t> patched = apply(data, {make_patch(hash)}, &r, /*require_all=*/true);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(patched, data); // refused outright, not a best-effort partial patch
}

TEST(PegasusPatchApply, SkipsANonUniqueAnchorSilentlyWhenRequireAllIsNotSet)
{
    std::vector<std::uint8_t> data = bytes("deadbeef0011deadbeef2233");
    const std::string hash = hash_of(data);
    Result r;
    const std::vector<std::uint8_t> patched = apply(data, {make_patch(hash)}, &r);
    EXPECT_FALSE(r.changed);
    EXPECT_TRUE(r.error.empty());
    EXPECT_EQ(patched, data);
}

TEST(PegasusPatchApply, MaskCharacterSkipsThatByte)
{
    std::vector<std::uint8_t> data = bytes("00dead00beef0011223300ff");
    const std::string hash = hash_of(data);
    // Prove the '?' wildcard itself: "dead??beef" (5 bytes) matches regardless of the 3rd byte.
    Patch p{"t1", "wildcard", hash, "dead00beef", "xx?xx", "feed00face"};
    Result r;
    const std::vector<std::uint8_t> patched = apply(data, {p}, &r);
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(patched, bytes("00feed00face0011223300ff"));
}

TEST(BuiltinCatalogue, EveryEntryIsWellFormed)
{
    for (const Patch &p : builtin_catalogue())
    {
        EXPECT_FALSE(p.id.empty());
        EXPECT_EQ(p.source_sha256.size(), 64u) << p.id;
        std::vector<std::uint8_t> pattern, replacement;
        ASSERT_TRUE(hex_decode(p.pattern_hex, &pattern)) << p.id;
        ASSERT_TRUE(hex_decode(p.replacement_hex, &replacement)) << p.id;
        EXPECT_EQ(pattern.size(), replacement.size()) << p.id;
        EXPECT_EQ(pattern.size(), p.mask.size()) << p.id;
    }
}

// ---- StructuralPatch (P1) ----
//
// Synthetic only - never the real pegasus_dl.elf, same rule as the Patch tests above. Builds a
// tiny buffer with just enough real ELF64 shape (one program header entry, one "FF 15 <disp32>"
// call site) to exercise try_apply_one()'s full path: overlay of the zero cave, program-header
// edit, call-site redirect. The real catalogue entry in pegasus_patch.cpp targets one exact 20 MB file
// by hash; this proves the *engine* is correct independent of that one file.

constexpr std::uint64_t kPOffset = 120; // right after a 64-byte header + one 56-byte phdr entry
constexpr std::uint64_t kPVaddr = 1000;
constexpr std::uint64_t kPFilesz = 10;
constexpr std::uint64_t kPMemsz = 10000;
// Inside e_ident (bytes 0-15), which this engine never reads, so it's safe scratch space clear of
// e_phoff/e_phentsize/e_phnum and the program header table at byte 64. text_p_offset=
// text_p_vaddr=0, so VA == file offset here.
constexpr std::uint64_t kCallSiteVa = 10;
constexpr std::uint64_t kOriginalTarget = 0x9999;
constexpr std::size_t kTotalSize = 200; // > p_offset + p_filesz + cave, so the tail is provable
constexpr std::uint64_t kCaveSize = 16;  // all-zero bytes right after the segment's file content

std::vector<std::uint8_t> build_fake_elf(std::vector<std::uint8_t> *tail_marker_out = nullptr)
{
    std::vector<std::uint8_t> d(kTotalSize, 0xAA); // filler, so "untouched" bytes are checkable
    auto put64 = [&](std::size_t at, std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    auto put32 = [&](std::size_t at, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    auto put16 = [&](std::size_t at, std::uint16_t v)
    {
        d[at] = static_cast<std::uint8_t>(v);
        d[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    // e_phoff (32,8)=64, e_phentsize (54,2)=56, e_phnum (56,2)=1 - every byte of each field is
    // explicit (0xAA filler in a high byte would turn these into garbage 64/16-bit values).
    put64(32, 64);
    put16(54, 56);
    put16(56, 1);
    // One PT_LOAD program header entry at file offset 64.
    const std::size_t ph = 64;
    put32(ph + 0, 1); // p_type = PT_LOAD
    put32(ph + 4, 6); // p_flags = PF_R|PF_W, no PF_X yet
    put64(ph + 8, kPOffset);
    put64(ph + 16, kPVaddr);
    put64(ph + 32, kPFilesz);
    put64(ph + 40, kPMemsz);
    // The call site: FF 15 <disp32>, where disp32 makes it resolve to kOriginalTarget.
    d[kCallSiteVa] = 0xFF;
    d[kCallSiteVa + 1] = 0x15;
    const std::int32_t disp =
        static_cast<std::int32_t>(static_cast<std::int64_t>(kOriginalTarget) - static_cast<std::int64_t>(kCallSiteVa + 6));
    for (int i = 0; i < 4; ++i)
        d[kCallSiteVa + 2 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(disp >> (8 * i));
    // The segment's existing file content (bytes [p_offset, p_offset+p_filesz)) - distinctive, so
    // we can tell it was left alone.
    for (std::uint64_t i = 0; i < kPFilesz; ++i)
        d[kPOffset + i] = 0xEE;
    // The zero-filled file-backed cave right after the segment's content, where the blob lands.
    for (std::uint64_t i = 0; i < kCaveSize; ++i)
        d[kPOffset + kPFilesz + i] = 0x00;
    if (tail_marker_out != nullptr)
        *tail_marker_out = std::vector<std::uint8_t>(d.begin() + static_cast<std::ptrdiff_t>(kPOffset + kPFilesz), d.end());
    return d;
}

StructuralPatch make_structural_patch(const std::string &hash)
{
    StructuralPatch p;
    p.id = "s0";
    p.description = "test";
    p.source_sha256 = hash;
    p.phdr_index = 0;
    p.expect_p_offset = kPOffset;
    p.expect_p_vaddr = kPVaddr;
    p.expect_p_filesz = kPFilesz;
    p.expect_p_memsz = kPMemsz;
    p.call_site_va = kCallSiteVa;
    p.call_site_expected_target_va = kOriginalTarget;
    p.text_p_offset = 0;
    p.text_p_vaddr = 0;
    p.blob_hex = "8877665544332211" "0000000000000000";
    return p;
}

std::string hash_of_vec(const std::vector<std::uint8_t> &data)
{
    return sha256_hex(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()));
}

TEST(ApplyStructural, OverlaysTheCaveAndRedirectsTheCallSite)
{
    const std::vector<std::uint8_t> original = build_fake_elf();
    const StructuralPatch patch = make_structural_patch(hash_of_vec(original));
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(original, {patch}, &r);
    ASSERT_TRUE(r.changed);
    EXPECT_EQ(r.id, "s0");
    EXPECT_TRUE(r.error.empty());

    // The file keeps its exact size: nothing is inserted, truncated or shifted.
    ASSERT_EQ(out.size(), original.size());

    // The segment's original content and everything past the cave are untouched.
    for (std::uint64_t i = 0; i < kPFilesz; ++i)
        EXPECT_EQ(out[kPOffset + i], 0xEE) << i;
    for (std::size_t i = kPOffset + kPFilesz + kCaveSize; i < out.size(); ++i)
        EXPECT_EQ(out[i], original[i]) << i;

    // The blob overlaid the zero cave immediately after the segment's content, verbatim.
    std::vector<std::uint8_t> blob;
    ASSERT_TRUE(hex_decode(patch.blob_hex, &blob));
    for (std::size_t i = 0; i < blob.size(); ++i)
        EXPECT_EQ(out[kPOffset + kPFilesz + i], blob[i]) << i;

    // p_filesz grew by exactly the blob's size; p_memsz and p_flags are unchanged.
    auto read64 = [&](std::size_t at)
    {
        std::uint64_t v = 0;
        for (int i = 7; i >= 0; --i)
            v = (v << 8) | out[at + static_cast<std::size_t>(i)];
        return v;
    };
    const std::size_t ph = 64;
    EXPECT_EQ(read64(ph + 32), kPFilesz + blob.size());
    EXPECT_EQ(read64(ph + 40), kPMemsz);
    EXPECT_EQ(out[ph + 4], original[ph + 4]);

    // The call site became `E8 rel32 90` (CALL + NOP, the same six bytes), targeting the blob's
    // first byte at p_vaddr + p_filesz.
    EXPECT_EQ(out[kCallSiteVa], 0xE8);
    EXPECT_EQ(out[kCallSiteVa + 5], 0x90);
    std::int32_t new_disp = 0;
    for (int i = 3; i >= 0; --i)
        new_disp = (new_disp << 8) | out[kCallSiteVa + 1 + static_cast<std::size_t>(i)];
    const std::int64_t new_target = static_cast<std::int64_t>(kCallSiteVa) + 5 + new_disp;
    EXPECT_EQ(new_target, static_cast<std::int64_t>(kPVaddr + kPFilesz));

    // Section header fields in the ELF header are preserved, not zeroed.
    EXPECT_EQ(read64(0x28), 0xAAAAAAAAAAAAAAAAull);
}

TEST(ApplyStructural, LeavesAWrongHashUntouched)
{
    const std::vector<std::uint8_t> original = build_fake_elf();
    const StructuralPatch patch = make_structural_patch(std::string(64, '0'));
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(original, {patch}, &r);
    EXPECT_FALSE(r.changed);
    EXPECT_TRUE(r.error.empty());
    EXPECT_EQ(out, original);
}

TEST(ApplyStructural, RefusesWhenTheProgramHeaderDoesNotMatchExactly)
{
    const std::vector<std::uint8_t> original = build_fake_elf();
    StructuralPatch patch = make_structural_patch(hash_of_vec(original));
    patch.expect_p_filesz = kPFilesz + 1; // deliberately wrong
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(original, {patch}, &r, /*require_all=*/true);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(out, original);
}

TEST(ApplyStructural, RefusesWhenTheCallSiteNoLongerTargetsTheExpectedAddress)
{
    const std::vector<std::uint8_t> original = build_fake_elf();
    StructuralPatch patch = make_structural_patch(hash_of_vec(original));
    patch.call_site_expected_target_va = kOriginalTarget + 1; // deliberately wrong
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(original, {patch}, &r, /*require_all=*/true);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(out, original);
}

TEST(ApplyStructural, RefusesWhenTheCaveIsNotAllZero)
{
    std::vector<std::uint8_t> original = build_fake_elf();
    original[kPOffset + kPFilesz + 3] = 0x01; // something already lives in the "cave"
    const StructuralPatch patch = make_structural_patch(hash_of_vec(original));
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(original, {patch}, &r, /*require_all=*/true);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(out, original);
}

TEST(ApplyStructural, RefusesWhenTheBlobWouldRunPastTheEndOfTheFile)
{
    std::vector<std::uint8_t> original = build_fake_elf();
    original.resize(kPOffset + kPFilesz + 8); // cave truncated to 8 bytes; blob is 16
    const StructuralPatch patch = make_structural_patch(hash_of_vec(original));
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(original, {patch}, &r, /*require_all=*/true);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(out, original);
}

// P0 companion anchor: P1 either applies the P0 bytes itself (stock input) or accepts a file that
// already carries them (P0-only hash), and refuses anything else.
constexpr std::size_t kP0Offset = 150; // in the untouched tail, clear of the segment and the cave
const std::string kP0Before(16, 'B');
const std::string kP0After(16, 'A');
std::string hex_of(const std::string &bytes)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : bytes)
    {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

std::vector<std::uint8_t> with_p0(std::vector<std::uint8_t> d, const std::string &bytes)
{
    std::copy(bytes.begin(), bytes.end(), d.begin() + static_cast<std::ptrdiff_t>(kP0Offset));
    return d;
}

StructuralPatch make_p0_companion_patch(const std::string &stock_hash, const std::string &p0_hash)
{
    StructuralPatch p = make_structural_patch(stock_hash);
    p.source_sha256_p0 = p0_hash;
    p.p0_file_offset = kP0Offset;
    p.p0_before_hex = hex_of(kP0Before);
    p.p0_after_hex = hex_of(kP0After);
    return p;
}

TEST(ApplyStructural, AppliesTheP0AnchorAtomicallyWithP1)
{
    const std::vector<std::uint8_t> stock = with_p0(build_fake_elf(), kP0Before);
    const std::vector<std::uint8_t> p0_only = with_p0(stock, kP0After);
    const StructuralPatch patch = make_p0_companion_patch(hash_of_vec(stock), hash_of_vec(p0_only));
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(stock, {patch}, &r, /*require_all=*/true);
    ASSERT_TRUE(r.changed) << r.error;
    EXPECT_EQ(std::string(out.begin() + kP0Offset, out.begin() + kP0Offset + 16), kP0After);
    EXPECT_EQ(out[kCallSiteVa], 0xE8);
}

TEST(ApplyStructural, AcceptsAFileThatAlreadyHasP0)
{
    const std::vector<std::uint8_t> stock = with_p0(build_fake_elf(), kP0Before);
    const std::vector<std::uint8_t> p0_only = with_p0(stock, kP0After);
    const StructuralPatch patch = make_p0_companion_patch(hash_of_vec(stock), hash_of_vec(p0_only));
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(p0_only, {patch}, &r, /*require_all=*/true);
    ASSERT_TRUE(r.changed) << r.error;
    EXPECT_EQ(std::string(out.begin() + kP0Offset, out.begin() + kP0Offset + 16), kP0After);
    EXPECT_EQ(out[kCallSiteVa], 0xE8);
}

TEST(ApplyStructural, RefusesWhenTheP0AnchorIsNotAtItsVerifiedOffset)
{
    const std::vector<std::uint8_t> stock = with_p0(build_fake_elf(), kP0Before);
    StructuralPatch patch = make_p0_companion_patch(hash_of_vec(stock), std::string(64, '1'));
    patch.p0_file_offset = kP0Offset + 1; // deliberately wrong
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(stock, {patch}, &r, /*require_all=*/true);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(out, stock);
}

// ---- Format v2: direct-call redirects (P1's completion barrier) ----
//
// Two extra "E8 <rel32>" call sites, both calling the same original function, are repointed at an
// entry point inside the blob. In the synthetic file VA == file offset (text_p_offset == vaddr == 0);
// the sites sit in the ELF header's e_type..e_entry bytes (16..31), which the engine never reads.
constexpr std::uint64_t kSyncSiteA = 20;
constexpr std::uint64_t kSyncSiteB = 26;
constexpr std::uint64_t kSyncOriginal = 0x7777;
constexpr std::uint64_t kSyncEntry = 8; // byte offset of the barrier entry point inside the 16-byte blob

void put_e8_call(std::vector<std::uint8_t> &d, std::uint64_t site, std::uint64_t target)
{
    d[site] = 0xE8;
    const std::int32_t disp =
        static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + 5));
    for (int i = 0; i < 4; ++i)
        d[site + 1 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(disp >> (8 * i));
}

std::vector<std::uint8_t> build_fake_elf_with_sync_sites()
{
    std::vector<std::uint8_t> d = build_fake_elf();
    put_e8_call(d, kSyncSiteA, kSyncOriginal);
    put_e8_call(d, kSyncSiteB, kSyncOriginal);
    return d;
}

StructuralPatch make_barrier_patch(const std::string &hash)
{
    StructuralPatch p = make_structural_patch(hash);
    p.direct_call_redirects = {{kSyncSiteA, kSyncOriginal, kSyncEntry}, {kSyncSiteB, kSyncOriginal, kSyncEntry}};
    return p;
}

std::int64_t call_target(const std::vector<std::uint8_t> &d, std::uint64_t site)
{
    std::int32_t disp = 0;
    for (int i = 3; i >= 0; --i)
        disp = static_cast<std::int32_t>((static_cast<std::uint32_t>(disp) << 8) | d[site + 1 + static_cast<std::size_t>(i)]);
    return static_cast<std::int64_t>(site) + 5 + disp;
}

TEST(ApplyStructuralRedirects, RepointsBothSitesAtTheBlobEntryAndChangesNothingElse)
{
    const std::vector<std::uint8_t> original = build_fake_elf_with_sync_sites();
    const StructuralPatch patch = make_barrier_patch(hash_of_vec(original));
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(original, {patch}, &r, /*require_all=*/true);
    ASSERT_TRUE(r.changed) << r.error;
    ASSERT_EQ(out.size(), original.size());

    const std::int64_t entry = static_cast<std::int64_t>(kPVaddr + kPFilesz + kSyncEntry);
    EXPECT_EQ(out[kSyncSiteA], 0xE8);
    EXPECT_EQ(out[kSyncSiteB], 0xE8);
    EXPECT_EQ(call_target(out, kSyncSiteA), entry);
    EXPECT_EQ(call_target(out, kSyncSiteB), entry);
    // The v1 pwrite redirect still goes to the blob's first byte.
    EXPECT_EQ(out[kCallSiteVa], 0xE8);
    EXPECT_EQ(call_target(out, kCallSiteVa), static_cast<std::int64_t>(kPVaddr + kPFilesz));

    // Exactly these bytes differ: the pwrite site (6), both sync disp32s (4 each), the two
    // program-header size fields (16), the 16-byte cave - nothing else, and not the E8 opcodes.
    std::vector<std::size_t> diff;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i] != original[i])
            diff.push_back(i);
    for (const std::size_t i : diff)
    {
        const bool in_pwrite = i >= kCallSiteVa && i < kCallSiteVa + 6;
        const bool in_sync_a = i >= kSyncSiteA + 1 && i < kSyncSiteA + 5;
        const bool in_sync_b = i >= kSyncSiteB + 1 && i < kSyncSiteB + 5;
        const bool in_phdr = i >= 64 + 32 && i < 64 + 48;
        const bool in_cave = i >= kPOffset + kPFilesz && i < kPOffset + kPFilesz + 16;
        EXPECT_TRUE(in_pwrite || in_sync_a || in_sync_b || in_phdr || in_cave) << "unexpected change at " << i;
    }
}

TEST(ApplyStructuralRedirects, OneBadSiteRefusesTheWholePatchAtomically)
{
    std::vector<std::uint8_t> original = build_fake_elf_with_sync_sites();
    original[kSyncSiteB] = 0x90; // second site is no longer a call
    const StructuralPatch patch = make_barrier_patch(hash_of_vec(original));
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(original, {patch}, &r, /*require_all=*/true);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(out, original); // the good first site and the pwrite site were not touched either
}

TEST(ApplyStructuralRedirects, RefusesASiteThatNoLongerTargetsTheExpectedFunction)
{
    const std::vector<std::uint8_t> original = build_fake_elf_with_sync_sites();
    StructuralPatch patch = make_barrier_patch(hash_of_vec(original));
    patch.direct_call_redirects[1].expected_target_va = kSyncOriginal + 1;
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(original, {patch}, &r, /*require_all=*/true);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(out, original);
}

TEST(ApplyStructuralRedirects, RefusesAnEntryPointOutsideTheBlob)
{
    const std::vector<std::uint8_t> original = build_fake_elf_with_sync_sites();
    StructuralPatch patch = make_barrier_patch(hash_of_vec(original));
    patch.direct_call_redirects[0].blob_entry_offset = 16; // blob is exactly 16 bytes
    StructuralResult r;
    const std::vector<std::uint8_t> out = apply_structural(original, {patch}, &r, /*require_all=*/true);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.error.empty());
    EXPECT_EQ(out, original);
}

TEST(ApplyStructuralRedirects, RefusesOverlappingOrDuplicateSites)
{
    const std::vector<std::uint8_t> original = build_fake_elf_with_sync_sites();
    StructuralPatch duplicate = make_barrier_patch(hash_of_vec(original));
    duplicate.direct_call_redirects[1].va = kSyncSiteA; // same site twice
    StructuralResult r;
    EXPECT_EQ(apply_structural(original, {duplicate}, &r, true), original);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.error.empty());

    // Two sites that are each a valid E8 call with the expected target, but whose 5-byte ranges
    // overlap (the second call starts inside the first one's rel32): refusal can only be the overlap.
    std::vector<std::uint8_t> clash_elf = build_fake_elf();
    put_e8_call(clash_elf, 20, kSyncOriginal);
    put_e8_call(clash_elf, 22, kSyncOriginal);
    StructuralPatch clash = make_structural_patch(hash_of_vec(clash_elf));
    clash.direct_call_redirects = {{20, static_cast<std::uint64_t>(call_target(clash_elf, 20)), kSyncEntry},
                                   {22, kSyncOriginal, kSyncEntry}};
    ASSERT_GT(call_target(clash_elf, 20), 0);
    StructuralResult r2;
    EXPECT_EQ(apply_structural(clash_elf, {clash}, &r2, true), clash_elf);
    EXPECT_FALSE(r2.changed);
    EXPECT_NE(r2.error.find("overlaps"), std::string::npos) << r2.error;
}

TEST(ApplyStructuralRedirects, IsNotReappliedToItsOwnOutput)
{
    const std::vector<std::uint8_t> original = build_fake_elf_with_sync_sites();
    const StructuralPatch patch = make_barrier_patch(hash_of_vec(original));
    StructuralResult first;
    const std::vector<std::uint8_t> once = apply_structural(original, {patch}, &first, true);
    ASSERT_TRUE(first.changed) << first.error;
    StructuralResult second;
    const std::vector<std::uint8_t> twice = apply_structural(once, {patch}, &second, true);
    EXPECT_FALSE(second.changed); // different hash: the patched file is not the gated source
    EXPECT_TRUE(second.error.empty());
    EXPECT_EQ(twice, once);

    // Even handed the patched bytes as if they were the source (hash forged to match), the
    // engine's own structural checks refuse: the pwrite site is no longer FF 15.
    StructuralPatch forged = patch;
    forged.source_sha256 = hash_of_vec(once);
    forged.expect_p_filesz = kPFilesz + 16;
    StructuralResult third;
    EXPECT_EQ(apply_structural(once, {forged}, &third, true), once);
    EXPECT_FALSE(third.changed);
    EXPECT_FALSE(third.error.empty());
}

TEST(ApplyStructuralRedirects, ComposesWithTheP0AnchorInEitherOrder)
{
    const std::vector<std::uint8_t> stock = with_p0(build_fake_elf_with_sync_sites(), kP0Before);
    const std::vector<std::uint8_t> p0_only = with_p0(stock, kP0After);
    StructuralPatch patch = make_barrier_patch(hash_of_vec(stock));
    patch.source_sha256_p0 = hash_of_vec(p0_only);
    patch.p0_file_offset = kP0Offset;
    patch.p0_before_hex = hex_of(kP0Before);
    patch.p0_after_hex = hex_of(kP0After);
    const std::int64_t entry = static_cast<std::int64_t>(kPVaddr + kPFilesz + kSyncEntry);
    for (const std::vector<std::uint8_t> *input : {&stock, &p0_only})
    {
        StructuralResult r;
        const std::vector<std::uint8_t> out = apply_structural(*input, {patch}, &r, true);
        ASSERT_TRUE(r.changed) << r.error;
        EXPECT_EQ(std::string(out.begin() + kP0Offset, out.begin() + kP0Offset + 16), kP0After);
        EXPECT_EQ(call_target(out, kSyncSiteA), entry);
        EXPECT_EQ(call_target(out, kSyncSiteB), entry);
    }
}

TEST(BuiltinStructuralCatalogue, P1HasTheBarrierRedirectsAndARealBlob)
{
    const StructuralPatch *p1 = nullptr;
    for (const StructuralPatch &p : builtin_structural_catalogue())
        if (p.id == "p1-writer-thread")
            p1 = &p;
    ASSERT_NE(p1, nullptr);

    std::vector<std::uint8_t> blob;
    ASSERT_TRUE(hex_decode(p1->blob_hex, &blob));
    EXPECT_FALSE(blob.empty()) << "P1 must ship its verified blob";
    EXPECT_EQ(p1->call_site_va, 0x4e22bu); // pwrite in segment_flush
    EXPECT_EQ(p1->call_site_expected_target_va, 0x7ef838u);

    // Both `call sync_segment_output` sites in download_segmented() go to one in-blob entry point,
    // which is not the blob's first byte (that is w1_write).
    ASSERT_EQ(p1->direct_call_redirects.size(), 2u);
    EXPECT_EQ(p1->direct_call_redirects[0].va, 0x4c1aeu);
    EXPECT_EQ(p1->direct_call_redirects[1].va, 0x4c24au);
    for (const DirectCallRedirect &r : p1->direct_call_redirects)
    {
        EXPECT_EQ(r.expected_target_va, 0x4d870u);
        EXPECT_GT(r.blob_entry_offset, 0u);
        EXPECT_LT(r.blob_entry_offset, blob.size());
        EXPECT_EQ(r.blob_entry_offset, p1->direct_call_redirects[0].blob_entry_offset);
    }
}

TEST(BuiltinStructuralCatalogue, EveryEntryIsWellFormed)
{
    for (const StructuralPatch &p : builtin_structural_catalogue())
    {
        EXPECT_FALSE(p.id.empty());
        EXPECT_EQ(p.source_sha256.size(), 64u) << p.id;
        std::vector<std::uint8_t> blob;
        ASSERT_TRUE(hex_decode(p.blob_hex, &blob)) << p.id;
        // An empty blob is the deliberate "disabled until rebuilt with the PS5 SDK" placeholder:
        // the engine refuses it, so the entry is inert. A non-empty one must be real hex (above).
        if (blob.empty())
        {
            StructuralPatch probe = p;
            const std::vector<std::uint8_t> original = build_fake_elf();
            probe.source_sha256 = hash_of_vec(original);
            StructuralResult r;
            const std::vector<std::uint8_t> out = apply_structural(original, {probe}, &r, /*require_all=*/true);
            EXPECT_FALSE(r.changed) << p.id;
            EXPECT_EQ(out, original) << p.id;
        }
        if (!p.p0_before_hex.empty() || !p.p0_after_hex.empty())
        {
            std::vector<std::uint8_t> before, after;
            ASSERT_TRUE(hex_decode(p.p0_before_hex, &before)) << p.id;
            ASSERT_TRUE(hex_decode(p.p0_after_hex, &after)) << p.id;
            EXPECT_EQ(before.size(), after.size()) << p.id;
            EXPECT_EQ(p.source_sha256_p0.size(), 64u) << p.id;
        }
    }
}

} // namespace
} // namespace orbit::pegasus_patch
