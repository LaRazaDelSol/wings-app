// Wings TV app - Experimental: byte-level patches to a verified pegasus_dl.elf (see pegasus_patch.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/pegasus_patch.hpp"

#include "orbit/pegasus_manager.hpp" // sha256_hex

#include <algorithm>

namespace orbit::pegasus_patch
{

bool hex_decode(std::string_view hex, std::vector<std::uint8_t> *out)
{
    out->clear();
    if (hex.size() % 2 != 0)
        return false;
    out->reserve(hex.size() / 2);
    const auto nibble = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
        {
            out->clear();
            return false;
        }
        out->push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

namespace
{

// Every offset in `data` where `pattern` matches under `mask` ('?' = any byte). Stops early past
// `limit` matches (the catalogue only ever needs to know "0", "1" or "more than 1").
std::vector<std::size_t> find_matches(const std::vector<std::uint8_t> &data, const std::vector<std::uint8_t> &pattern,
                                      std::string_view mask, std::size_t limit)
{
    std::vector<std::size_t> hits;
    if (pattern.empty() || pattern.size() != mask.size() || data.size() < pattern.size())
        return hits;
    const std::size_t last = data.size() - pattern.size();
    for (std::size_t at = 0; at <= last; ++at)
    {
        bool ok = true;
        for (std::size_t i = 0; i < pattern.size() && ok; ++i)
            ok = mask[i] == '?' || data[at + i] == pattern[i];
        if (ok)
        {
            hits.push_back(at);
            if (hits.size() >= limit)
                break;
        }
    }
    return hits;
}

} // namespace

const std::vector<Patch> &builtin_catalogue()
{
    // P0 - the per-segment write buffer in download_segmented()/segment_write()/segment_flush() is
    // capped at 256 KiB (0x00040000) before each pwrite(). Orbit Store's equivalent path holds up to
    // 8 MiB total, split across the active ranges, written by a separate thread. This patch only
    // raises the cap (to 2 MiB, Orbit's per-range share at 4 connections) so a flush needs 1/8th as
    // many pwrite() calls; it does NOT move the write off the socket-servicing thread (that is P1,
    // still a design in docs/research/pegasus-dl-patch.md, not implemented here).
    //
    // Anchor (16 bytes, unique in the v1.11.0 ELF this was derived from - see the doc for how to
    // re-derive it for a later release):
    //   48 81 FF 00 00 04 00   cmp  rdi, 0x40000
    //   B8 00 00 04 00         mov  eax, 0x40000
    //   48 0F 43 F8            cmovae rdi, rax
    static const std::vector<Patch> kCatalogue = {
        Patch{
            "p0-segment-buffer-2m",
            "Raise the 256 KiB per-segment write buffer to 2 MiB (Orbit Store's per-range share at 4 "
            "connections), so each flush needs 8x fewer pwrite() calls. Diagnostic only; does not move "
            "the write off the network thread.",
            "32b69c2089bb545abfabd04d5827444698f9c4f1bc4915d72247cb781e8c23d9",
            "4881ff00000400b800000400480f43f8",
            "xxxxxxxxxxxxxxxx",
            "4881ff00002000b800002000480f43f8",
        },
    };
    return kCatalogue;
}

std::vector<std::uint8_t> apply(const std::vector<std::uint8_t> &data, const std::vector<Patch> &catalogue,
                                 Result *out_result, bool require_all)
{
    Result local;
    Result &result = out_result != nullptr ? *out_result : local;
    result = Result{};
    std::vector<std::uint8_t> out = data;
    if (data.empty())
    {
        result.error = "empty input";
        return out;
    }
    const std::string source_hash =
        sha256_hex(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()));
    for (const Patch &patch : catalogue)
    {
        if (patch.source_sha256.empty() || patch.pattern_hex.empty())
        {
            if (require_all)
            {
                result.error = "patch '" + patch.id + "' is incomplete";
                return data;
            }
            continue;
        }
        if (source_hash != patch.source_sha256)
            continue; // a different Pegasus build: never guess, always leave it alone
        std::vector<std::uint8_t> pattern, replacement;
        if (!hex_decode(patch.pattern_hex, &pattern) || !hex_decode(patch.replacement_hex, &replacement) ||
            pattern.size() != patch.mask.size() || pattern.size() != replacement.size())
        {
            if (require_all)
            {
                result.error = "patch '" + patch.id + "' has a malformed pattern";
                return data;
            }
            continue;
        }
        const std::vector<std::size_t> hits = find_matches(out, pattern, patch.mask, 2);
        if (hits.size() != 1)
        {
            if (require_all)
            {
                result.error = "patch '" + patch.id + "' anchor is " +
                               (hits.empty() ? "absent" : "not unique") + " in this build";
                return data;
            }
            continue; // 0 or >1 matches: this build is not the one byte-for-byte shape expected
        }
        std::copy(replacement.begin(), replacement.end(), out.begin() + static_cast<std::ptrdiff_t>(hits[0]));
        result.applied.push_back(patch.id);
        result.changed = true;
    }
    return result.changed ? out : data;
}

namespace
{

std::uint64_t read_u64(const std::vector<std::uint8_t> &d, std::size_t at)
{
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | d[at + static_cast<std::size_t>(i)];
    return v;
}
std::uint32_t read_u32(const std::vector<std::uint8_t> &d, std::size_t at)
{
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | d[at + static_cast<std::size_t>(i)];
    return v;
}
std::uint16_t read_u16(const std::vector<std::uint8_t> &d, std::size_t at)
{
    return static_cast<std::uint16_t>(d[at] | (d[at + 1] << 8));
}
void write_u64(std::vector<std::uint8_t> &d, std::size_t at, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
void write_u32(std::vector<std::uint8_t> &d, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}

// One attempt at `patch`; true and *out mutated on success, false (and *out untouched) on any
// mismatch - never a partial edit.
bool try_apply_one(const std::vector<std::uint8_t> &in, const StructuralPatch &patch,
                   std::vector<std::uint8_t> *out, std::string *why)
{
    if (in.size() < 64)
    {
        *why = "file too small to be an ELF";
        return false;
    }
    const std::uint64_t e_phoff = read_u64(in, 32);
    const std::uint16_t e_phentsize = read_u16(in, 54);
    const std::uint16_t e_phnum = read_u16(in, 56);
    if (e_phentsize != 56 || patch.phdr_index >= e_phnum ||
        e_phoff > in.size() || static_cast<std::uint64_t>(e_phnum) * e_phentsize > in.size() - e_phoff)
    {
        *why = "program header table does not look as expected";
        return false;
    }
    const std::size_t ph = static_cast<std::size_t>(e_phoff) + patch.phdr_index * e_phentsize;
    const std::uint32_t p_type = read_u32(in, ph);
    const std::uint32_t p_flags = read_u32(in, ph + 4);
    const std::uint64_t p_offset = read_u64(in, ph + 8);
    const std::uint64_t p_vaddr = read_u64(in, ph + 16);
    const std::uint64_t p_filesz = read_u64(in, ph + 32);
    const std::uint64_t p_memsz = read_u64(in, ph + 40);
    if (p_type != 1 || p_offset != patch.expect_p_offset || p_vaddr != patch.expect_p_vaddr ||
        p_filesz != patch.expect_p_filesz || p_memsz != patch.expect_p_memsz)
    {
        *why = "target PT_LOAD entry does not match exactly";
        return false;
    }
    if (patch.call_site_va < patch.text_p_vaddr)
    {
        *why = "call-site VA precedes its expected text segment";
        return false;
    }
    const std::uint64_t call_site_off = patch.call_site_va - patch.text_p_vaddr + patch.text_p_offset;
    if (call_site_off > in.size() || in.size() - call_site_off < 6 ||
        in[call_site_off] != 0xFF || in[call_site_off + 1] != 0x15)
    {
        *why = "call site is not the expected FF 15 indirect call";
        return false;
    }
    const std::int32_t old_disp = static_cast<std::int32_t>(read_u32(in, call_site_off + 2));
    const std::int64_t old_target = static_cast<std::int64_t>(patch.call_site_va) + 6 + old_disp;
    if (old_target < 0 || static_cast<std::uint64_t>(old_target) != patch.call_site_expected_target_va)
    {
        *why = "call site no longer targets the expected address";
        return false;
    }

    std::vector<std::uint8_t> blob;
    if (!hex_decode(patch.blob_hex, &blob) || blob.empty())
    {
        *why = "patch blob is malformed or empty";
        return false;
    }
    if (p_filesz > UINT64_MAX - blob.size() || p_offset > UINT64_MAX - p_filesz)
    {
        *why = "PT_LOAD size arithmetic overflow";
        return false;
    }

    // The PT_LOAD's file-backed content ends exactly where the zero-filled cave begins.
    // Overlay there instead of inserting bytes: inserting would shift later segments and ELF
    // metadata. Require the whole cave to be in-file, all-zero, and free of any other PT_LOAD.
    const std::uint64_t cave_off = p_offset + p_filesz;
    if (cave_off > UINT64_MAX - blob.size())
    {
        *why = "patch cave offset overflows";
        return false;
    }
    const std::uint64_t cave_end_off = cave_off + blob.size();
    const std::uint64_t code_va = p_vaddr + p_filesz;
    const std::uint64_t code_end_va = code_va + blob.size();
    if (cave_end_off > in.size() || code_end_va < code_va)
    {
        *why = "patch blob does not fit in the file-backed cave";
        return false;
    }
    if (!std::all_of(in.begin() + static_cast<std::ptrdiff_t>(cave_off),
                     in.begin() + static_cast<std::ptrdiff_t>(cave_end_off),
                     [](std::uint8_t b) { return b == 0; }))
    {
        *why = "executable cave is not all-zero padding";
        return false;
    }
    for (std::uint16_t i = 0; i < e_phnum; ++i)
    {
        if (i == patch.phdr_index)
            continue;
        const std::size_t other_ph = static_cast<std::size_t>(e_phoff) + i * e_phentsize;
        if (read_u32(in, other_ph) != 1) // only PT_LOAD ranges matter here
            continue;
        const std::uint64_t other_off = read_u64(in, other_ph + 8);
        const std::uint64_t other_va = read_u64(in, other_ph + 16);
        const std::uint64_t other_filesz = read_u64(in, other_ph + 32);
        const std::uint64_t other_memsz = read_u64(in, other_ph + 40);
        if (other_off <= UINT64_MAX - other_filesz &&
            cave_off < other_off + other_filesz && other_off < cave_end_off)
        {
            *why = "patch cave overlaps another PT_LOAD file range";
            return false;
        }
        if (other_va <= UINT64_MAX - other_memsz &&
            code_va < other_va + other_memsz && other_va < code_end_va)
        {
            *why = "patch code overlaps another PT_LOAD memory range";
            return false;
        }
    }

    const std::uint64_t new_filesz = p_filesz + blob.size();
    const std::uint64_t new_memsz = std::max(p_memsz, new_filesz);
    if (new_memsz < new_filesz || p_vaddr > UINT64_MAX - new_memsz)
    {
        *why = "new PT_LOAD size overflows";
        return false;
    }
    const std::int64_t new_disp =
        static_cast<std::int64_t>(code_va) - static_cast<std::int64_t>(patch.call_site_va + 5);
    if (new_disp < -(1LL << 31) || new_disp >= (1LL << 31))
    {
        *why = "CALL rel32 displacement out of range";
        return false;
    }

    // P0 and P1 must compose regardless of application order. P0's ordinary patch may already
    // have changed this one anchor, or P1 can apply it here atomically with the structural edit.
    std::vector<std::uint8_t> p0_before, p0_after;
    bool p0_already_applied = false;
    if (!patch.p0_before_hex.empty() || !patch.p0_after_hex.empty())
    {
        if (!hex_decode(patch.p0_before_hex, &p0_before) ||
            !hex_decode(patch.p0_after_hex, &p0_after) || p0_before.empty() ||
            p0_before.size() != p0_after.size() ||
            patch.p0_file_offset > in.size() || in.size() - patch.p0_file_offset < p0_before.size())
        {
            *why = "P0 companion anchor is malformed";
            return false;
        }
        const std::string exact_mask(p0_before.size(), 'x');
        const auto before_hits = find_matches(in, p0_before, exact_mask, 2);
        const auto after_hits = find_matches(in, p0_after, exact_mask, 2);
        if (before_hits.size() == 1 && before_hits[0] == patch.p0_file_offset && after_hits.empty())
            p0_already_applied = false;
        else if (after_hits.size() == 1 && after_hits[0] == patch.p0_file_offset && before_hits.empty())
            p0_already_applied = true;
        else
        {
            *why = "P0 companion anchor is absent, duplicated, or not at the verified offset";
            return false;
        }
    }

    // Format v2: validate EVERY extra E8 call redirect against the live file before any byte is
    // changed, so one bad site refuses the whole patch (never a partial P1).
    struct ResolvedRedirect
    {
        std::uint64_t off;
        std::int32_t disp;
    };
    const auto ranges_overlap = [](std::uint64_t a, std::uint64_t alen, std::uint64_t b, std::uint64_t blen)
    { return a < b + blen && b < a + alen; };
    std::vector<ResolvedRedirect> redirects;
    for (const DirectCallRedirect &r : patch.direct_call_redirects)
    {
        if (r.va < patch.text_p_vaddr || r.blob_entry_offset >= blob.size())
        {
            *why = "direct call redirect is outside its text segment or the blob";
            return false;
        }
        const std::uint64_t off = r.va - patch.text_p_vaddr + patch.text_p_offset;
        if (off > in.size() || in.size() - off < 5 || in[off] != 0xE8)
        {
            *why = "direct call redirect site is not the expected E8 call";
            return false;
        }
        const std::int64_t old_target =
            static_cast<std::int64_t>(r.va) + 5 + static_cast<std::int32_t>(read_u32(in, off + 1));
        if (old_target < 0 || static_cast<std::uint64_t>(old_target) != r.expected_target_va)
        {
            *why = "direct call redirect site no longer targets the expected address";
            return false;
        }
        const std::int64_t disp = static_cast<std::int64_t>(code_va + r.blob_entry_offset) -
                                  static_cast<std::int64_t>(r.va + 5);
        if (disp < -(1LL << 31) || disp >= (1LL << 31))
        {
            *why = "direct call redirect displacement out of range";
            return false;
        }
        bool clash = ranges_overlap(off, 5, call_site_off, 6) || ranges_overlap(off, 5, cave_off, blob.size());
        if (!p0_before.empty())
            clash = clash || ranges_overlap(off, 5, patch.p0_file_offset, p0_before.size());
        for (const ResolvedRedirect &prev : redirects)
            clash = clash || ranges_overlap(off, 5, prev.off, 5);
        if (clash)
        {
            *why = "direct call redirect overlaps another edit";
            return false;
        }
        redirects.push_back({off, static_cast<std::int32_t>(disp)});
    }

    std::vector<std::uint8_t> result = in;
    if (!p0_already_applied && !p0_before.empty())
        std::copy(p0_after.begin(), p0_after.end(),
                  result.begin() + static_cast<std::ptrdiff_t>(patch.p0_file_offset));
    std::copy(blob.begin(), blob.end(), result.begin() + static_cast<std::ptrdiff_t>(cave_off));
    result[call_site_off] = 0xE8; // CALL rel32
    write_u32(result, static_cast<std::size_t>(call_site_off + 1), static_cast<std::uint32_t>(new_disp));
    result[call_site_off + 5] = 0x90; // NOP: preserve the original six-byte instruction footprint
    for (const ResolvedRedirect &r : redirects) // format v2: same-length E8 -> E8, new rel32 only
        write_u32(result, static_cast<std::size_t>(r.off + 1), static_cast<std::uint32_t>(r.disp));
    // Segment #0 is already RWE; preserve p_flags and every section/header/tail byte.
    (void)p_flags;
    write_u64(result, ph + 32, new_filesz);
    write_u64(result, ph + 40, new_memsz);

    *out = std::move(result);
    return true;
}
} // namespace

const std::vector<StructuralPatch> &builtin_structural_catalogue()
{
    // P1 is hash-gated to the exact stock ELF. The embedded blob is relocation-resolved code
    // generated from w1.c plus the zero-initialized g_w1 state; the ring itself is host-malloc'd.
    static const std::vector<StructuralPatch> kCatalogue = {
        StructuralPatch{
            "p1-writer-thread",
            "Redirect segment_flush's pwrite() to a bounded ring-buffered background writer thread, "
            "and drain it (reporting async write failures) before fsync/close. "
            "Experimental.",
            "32b69c2089bb545abfabd04d5827444698f9c4f1bc4915d72247cb781e8c23d9",
            /*phdr_index*/ 0,
            /*expect p_offset/p_vaddr/p_filesz/p_memsz*/ 0x4000, 0x0, 0x5f1a65, 0x5f1a65,
            /*call_site_va*/ 0x4e22b,
            /*call_site_expected_target_va*/ 0x7ef838,
            /*text_p_offset/text_p_vaddr*/ 0x4000, 0x0,
            /*already-resolved code + zeroed g_w1 state*/
            "554889e54157415641554154534883ec3848894dc0488955c84189ff8b055106000083f8020f84c0010000b901000000"
            "31c0f00fb10d390600000f85a00100004989f6488d0589dd1f00488b1848891db7050000488d0578da1f004c8b184c89"
            "1dae050000488d0567dc1f004c8b104c8915a5050000488d0586d91f004c8b084c890d9c050000488d058dd91f004c8b"
            "004c890593050000488d05a4df1f00488b3848893d8a050000488d054bdc1f00488b3048893581050000488d058adf1f"
            "00488b1048891578050000488d05c1d91f00488b0848890d6f050000488d0590d91f00488b00488905660500004885db"
            "0f84cc0000004d85db0f84c30000004d85d20f84ba0000004d85c90f84b10000004d85c00f84a80000004885ff0f849f"
            "0000004885f60f84960000004885d20f848d0000004885c90f84840000004885c0747f488d05f1d81f00488b004885c0"
            "7470bfc0008000ffd0488905ab0500004885c0745d488d3def04000031f6ff15a704000085c07532488d3de404000031"
            "f6ff15ac04000085c0751f488d3dd9040000488d15ea01000031f631c9ff15700400004c89f685c07425488b3d5a0500"
            "00ff159404000048c7054905000000000000c705ab040000010000004c89f6c7059a04000002000000eb100f1f440000"
            "8b058d04000083f80275f58b058604000085c0741931c0488945d0488b45d04883c4385b415c415d415e415f5dc34181"
            "ffff0300007622488b05ed0300004489ff488b55c8488b4dc04883c4385b415c415d415e415f5dffe0488975b84489f8"
            "c1e806488945a0b8010000004c897db0c4e281f7c0488945a84531f64c8d2df80300004c8d3df9030000eb1a0f1f4000"
            "4c89efff15b20300004c8975d04885db0f8575ffffff4c8b65c84d29f40f86e70000004981fc00001000b8000010004c"
            "0f43e04c89efff1577030000833d5004000008752066662e0f1f8400000000004c89ff4c89eeff156f030000833d3004"
            "00000874eb488b45a0488d0d2b030000488b5cc17048235da87585488b0d190400008b05070400004869d01800100048"
            "8b45b0890411488b45c04c01f0488944110844896411104585e47424488b45b84c01f0488d4c111431d2660f1f440000"
            "0fb634104088341148ffc24939d475f08b05b9030000ffc083e0078905ae030000ff05ac0300004c89ffff15e3020000"
            "4c89efff15c20200004d01e64885db0f8411ffffffe981feffff488b45c8e974feffff554889e5415741564155415453"
            "4883ec28488d1dc00200004c8d35c1020000eb3f6666666666662e0f1f8400000000008b0542030000ffc083e0078905"
            "37030000ff0d390300004c8d35920200004c89f7ff15690200004889dfff15480200004889dfff1537020000833d1003"
            "000000752366666666662e0f1f8400000000004c89f74889deff152c020000833ded0200000074eb4889df488b1de902"
            "00008b05d30200004c69f018001000468b3c334a8b443308488945b8428b4433108945d44489f9c1e9064489fa83e23f"
            "b801000000488955c8c462e9f7e8488d05a601000048894dc04c236cc870ff15b70100004e8d7433144531e431db0f1f"
            "4400004d85ed75398b45d489c229da763089d9498d340e48034db84489ffff15670100004885c089c1ba000000000f4e"
            "ca01cb4885c0b801000000440f4ee07fc2488d1d930100004889dfff15520100004585e40f84d9feffffb80100000048"
            "8b4dc8c4e2f1f7c0488d0d1c010000488b55c0480944d170e9b6feffff554889e541574156535089fb8b056401000085"
            "c00f84dc000000660f1f44000083f801750b8b054b01000083f80174f58b054401000085c07417488b05150100004885"
            "c00f848f000000ffd0e982000000488d3d06010000ff15c8000000833da101000000742e4c8d35f80000004c8d3de900"
            "000066662e0f1f8400000000004c89f74c89feff15b2000000833d730100000075eb81fbff030000774c89d8c1e80648"
            "8d0d65000000488b54c170480fa3da733583e33f480fb3da488954c170488d3d97000000ff1561000000ff1583000000"
            "c70005000000b8ffffffff4883c4085b415e415f5dc3488d3d6e000000ff153800000089df4883c4085b415e415f5de9"
            "07b8a5ff9000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
            "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
            "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
            "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
            "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000000000000",
            /*known P0-only source hash*/ "9e13e060f8810f1c2dda4e25f6169f4a34deb02d889556dfeac8f4a224d7d5bf",
            /*P0 file offset*/ 0x4FE77,
            /*P0 before/after*/ "4881ff00000400b800000400480f43f8",
            "4881ff00002000b800002000480f43f8",
            // Both `call sync_segment_output` sites in download_segmented() -> w1_sync (the
            // completion barrier). Entry offset = w1_sync's offset in the blob.
            /*direct-call redirects*/
            {
                {0x4c1ae, 0x4d870, 0x4fd},
                {0x4c24a, 0x4d870, 0x4fd},
            }
        },
    };
    return kCatalogue;
}

std::vector<std::uint8_t> apply_structural(const std::vector<std::uint8_t> &data,
                                           const std::vector<StructuralPatch> &catalogue,
                                           StructuralResult *out_result, bool require_all)
{
    StructuralResult local;
    StructuralResult &result = out_result != nullptr ? *out_result : local;
    result = StructuralResult{};
    if (data.empty())
    {
        result.error = "empty input";
        return data;
    }
    const std::string source_hash =
        sha256_hex(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()));
    for (const StructuralPatch &patch : catalogue)
    {
        if (source_hash != patch.source_sha256 &&
            (patch.source_sha256_p0.empty() || source_hash != patch.source_sha256_p0))
            continue;
        std::vector<std::uint8_t> patched;
        std::string why;
        if (!try_apply_one(data, patch, &patched, &why))
        {
            if (require_all)
            {
                result.error = "patch '" + patch.id + "': " + why;
                return data;
            }
            continue;
        }
        result.changed = true;
        result.id = patch.id;
        return patched; // one structural patch per file; there is exactly one in the catalogue
    }
    return data;
}

} // namespace orbit::pegasus_patch
