// Dark Sector: keyboard / mouse button prompts (button_prompts 3, or automatic
// with no controller connected), drawn on the game's own PC keycap.
//
// The button icons are textures in Cache.Xbox360\B.Texture.cache (a zip; the
// small ones stored uncompressed, already in the GPU's tiled layout):
// Interface/Graphics/PlatformKeys/Xbox360A_colo.png.1 etc., 64x64 32-bit
// (A R G B bytes, Tiled2D), and the PC version's blank key,
// PCSingleKeySmall_colo.png.1, in the same format. At startup the keycap is
// untiled, the bound key (or mouse button) is drawn on it for each pad
// button (prompt_art's strokes, as WET's keyboard prompts, following the
// player's bindings), and it is tiled again. When the game reads those
// textures' bytes from the cache (NtReadFile), the keycap versions are
// written over them in its buffer. Bindings changed in F1 show at the next
// start (the game loads its icons once).
//
// The shoulder, trigger and stick icons (XboxLB/RB_colo 128x64, XboxLTrigger/
// RTrigger_colo and AnalogStickL/R_colo 64x64) are DXT5 with their mipmaps,
// compressed in the zip (method 64): chunks of [compressed size][16384] (big
// endian), each LZF. The game unpacks each chunk with sub_828041C8
// (lzf_decompress(in, in size, out, out size)); that function is overridden
// below: a chunk of one of those icons is recognised by its bytes and the
// keycap version (all mip levels) written over what it unpacked.
#include "game.h"
#if GAME_DS
#include "kernel/kernel.h"
#include "log.h"
#include "options.h"
#include "ppc_context.h"
#include "prompt_art.h"

#include <atomic>
#include <cmath>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace wet::prompts
{
    int WantedArtIndex();
    bool KeyboardLabelFor(wet::prompt_art::Symbol sym, wet::prompt_art::Label& label);
}

namespace
{
    using namespace wet::prompt_art;

    struct Patch
    {
        uint64_t offset = 0;          // in B.Texture.cache
        std::vector<uint8_t> bytes;   // the texture as the game reads it
        std::string name;
        bool logged = false;
    };
    std::mutex s_lock;
    std::vector<Patch> s_patches;
    bool s_ready = false;

    // A compressed icon: its chunks as stored, and the keycap version unpacked.
    struct DxtPatch
    {
        std::string name;
        std::vector<std::vector<uint8_t>> chunks;   // each chunk's LZF bytes
        std::vector<uint8_t> bytes;                 // the whole texture, all mips (16384 per chunk)
        bool logged = false;
    };
    std::vector<DxtPatch> s_dxtPatches;
    std::atomic<bool> s_haveDxt{ false };

    uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

    // LZF (liblzf's format, as sub_828041C8).
    bool Lzf(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out)
    {
        size_t i = 0;
        while (i < inLen)
        {
            unsigned c = in[i++];
            if (c < 32)
            {
                if (i + c + 1 > inLen)
                    return false;
                out.insert(out.end(), in + i, in + i + c + 1);
                i += c + 1;
                continue;
            }
            size_t len = c >> 5;
            if (len == 7)
            {
                if (i >= inLen)
                    return false;
                len += in[i++];
            }
            if (i >= inLen)
                return false;
            size_t off = ((c & 31) << 8 | in[i++]) + 1;
            if (off > out.size())
                return false;
            for (size_t k = 0; k < len + 2; k++)
                out.push_back(out[out.size() - off]);
        }
        return true;
    }

    uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
    uint32_t Le32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

    // texture_address::Tiled2D (prompts.cpp): byte offset of element (x, y).
    uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitch, unsigned bpbLog2)
    {
        uint32_t outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6;
        uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
        uint32_t oib = (outer | inner) << bpbLog2;
        uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
        return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 15) | (((oib >> 4) & 1) << 5) |
               (((oib >> 5) & 7) << 8) | (oib >> 8 << 12);
    }

    // The zip's entries: name -> (data offset, stored size, method).
    struct Entry { uint64_t offset; uint32_t size; uint16_t method; };
    bool FindEntries(std::FILE* f, std::vector<std::pair<std::string, Entry>>& wanted)
    {
        std::fseek(f, 0, SEEK_END);
        long len = std::ftell(f);
        long tail = std::min<long>(len, 0x10000 + 22);
        std::vector<uint8_t> t(static_cast<size_t>(tail));
        std::fseek(f, len - tail, SEEK_SET);
        if (std::fread(t.data(), 1, t.size(), f) != t.size())
            return false;
        long eocd = -1;
        for (long i = long(t.size()) - 22; i >= 0; i--)
            if (Le32(&t[size_t(i)]) == 0x06054B50)
            {
                eocd = i;
                break;
            }
        if (eocd < 0)
            return false;
        uint32_t cdSize = Le32(&t[size_t(eocd) + 12]), cdOffset = Le32(&t[size_t(eocd) + 16]);
        std::vector<uint8_t> cd(cdSize);
        std::fseek(f, long(cdOffset), SEEK_SET);
        if (std::fread(cd.data(), 1, cd.size(), f) != cd.size())
            return false;
        int found = 0;
        for (size_t p = 0; p + 46 <= cd.size() && Le32(&cd[p]) == 0x02014B50;)
        {
            uint16_t method = Le16(&cd[p + 10]), nameLen = Le16(&cd[p + 28]), extraLen = Le16(&cd[p + 30]),
                     commentLen = Le16(&cd[p + 32]);
            uint32_t csize = Le32(&cd[p + 20]), local = Le32(&cd[p + 42]);
            std::string name(reinterpret_cast<const char*>(&cd[p + 46]), nameLen);
            for (auto& [want, e] : wanted)
                if (name == want)
                {
                    uint8_t lh[30];
                    std::fseek(f, long(local), SEEK_SET);
                    if (std::fread(lh, 1, 30, f) == 30 && Le32(lh) == 0x04034B50)
                    {
                        e = { uint64_t(local) + 30 + Le16(lh + 26) + Le16(lh + 28), csize, method };
                        found++;
                    }
                }
            p += 46 + nameLen + extraLen + commentLen;
        }
        return found == int(wanted.size());
    }

    // The keycap with the bound key (or mouse) drawn on it.
    std::vector<Rgba> KeycapWith(const std::vector<Rgba>& keycap, const Label& label)
    {
        std::vector<Seg> segs;
        std::vector<Box> fills;
        float width = 0;
        LabelStrokes(label, 32.0f, 30.5f, 42.0f, 38.0f, segs, fills, width);
        std::vector<Rgba> px = keycap;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
            {
                float c = Coverage(float(x), float(y), segs, width, 32, 30.5f, 0, &fills);
                if (c <= 0)
                    continue;
                Rgba& p = px[size_t(y * 64 + x)];
                p.r = uint8_t(p.r + (225 - p.r) * c);
                p.g = uint8_t(p.g + (225 - p.g) * c);
                p.b = uint8_t(p.b + (228 - p.b) * c);
                p.a = std::max<uint8_t>(p.a, uint8_t(255 * c));
            }
        return px;
    }

    // Half size, colours weighted by alpha.
    std::vector<Rgba> Half(const std::vector<Rgba>& px, int w, int h)
    {
        int hw = std::max(w / 2, 1), hh = std::max(h / 2, 1);
        std::vector<Rgba> out(size_t(hw * hh));
        for (int y = 0; y < hh; y++)
            for (int x = 0; x < hw; x++)
            {
                int r = 0, g = 0, b = 0, a = 0;
                for (int k = 0; k < 4; k++)
                {
                    const Rgba& p = px[size_t(std::min(y * 2 + (k >> 1), h - 1) * w + std::min(x * 2 + (k & 1), w - 1))];
                    r += p.r * p.a; g += p.g * p.a; b += p.b * p.a; a += p.a;
                }
                out[size_t(y * hw + x)] = a ? Rgba{ uint8_t(r / a), uint8_t(g / a), uint8_t(b / a), uint8_t(a / 4) }
                                            : Rgba{ 0, 0, 0, 0 };
            }
        return out;
    }

    // Writes a w x h DXT5 icon and its mipmaps over `tex`, laid out as the
    // GPU has them: 32x32-block tiles (pitch 32 blocks), level 0 at 0, level 1
    // at 16384, the levels of 16 pixels or less packed in the tile at 32768
    // (texture_util::GetPackedMipOffset). 8-in-16 byte order.
    void WriteDxt5Mips(std::vector<Rgba> px, int w, int h, std::vector<uint8_t>& tex)
    {
        int log2w = int(std::ceil(std::log2(w))), log2h = int(std::ceil(std::log2(h)));
        int log2size = std::min(log2w, log2h);
        int packedBase = log2size > 4 ? log2size - 4 : 0;
        for (int mip = 0; w >= 4 && h >= 4; mip++)
        {
            uint32_t base;
            int ox = 0, oy = 0;
            if (log2size > 4 + mip)
                base = uint32_t(mip) * 16384;   // not packed (one 32x32-block tile each here)
            else
            {
                base = uint32_t(packedBase) * 16384;
                int packed = mip - packedBase;
                if (packed < 3)
                    (log2w > log2h ? oy : ox) = 16 >> packed;
                else
                {
                    int off = (1 << (std::max(log2w, log2h) - packedBase)) >> (packed - 2);
                    if (off < 4)
                        break;   // packed in depth; too small to matter
                    (log2w > log2h ? ox : oy) = off;
                }
            }
            int bw = w / 4, bh = h / 4;
            std::vector<uint8_t> blocks(size_t(bw * bh * 16));
            EncodeDxt5(px.data(), w, h, blocks.data());
            for (int by = 0; by < bh; by++)
                for (int bx = 0; bx < bw; bx++)
                {
                    uint32_t o = base + Tiled2D(uint32_t(bx + ox / 4), uint32_t(by + oy / 4), 32, 4);
                    if (o + 16 > tex.size())
                        continue;
                    const uint8_t* b = &blocks[size_t((by * bw + bx) * 16)];
                    for (int k = 0; k < 16; k += 2)
                    {
                        tex[o + k] = b[k + 1];
                        tex[o + k + 1] = b[k];
                    }
                }
            px = Half(px, w, h);
            w /= 2;
            h /= 2;
        }
    }

    void Prepare(const std::string& cachePath)
    {
        s_ready = true;
        if (wet::prompts::WantedArtIndex() != 2)
            return;
        std::FILE* f = std::fopen(cachePath.c_str(), "rb");
        if (!f)
            return;
        const std::string dir = "Interface/Graphics/PlatformKeys/";
        struct Button { const char* file; Symbol sym; } kButtons[] = {
            { "Xbox360A_colo", Symbol::Cross }, { "Xbox360B_colo", Symbol::Circle },
            { "Xbox360X_colo", Symbol::Square }, { "Xbox360Y_colo", Symbol::Triangle },
        };
        struct Compressed { const char* file; Symbol sym; int w, h; } kCompressed[] = {
            { "XboxLB_colo", Symbol::L1, 128, 64 }, { "XboxRB_colo", Symbol::R1, 128, 64 },
            { "XboxLTrigger_colo", Symbol::L2, 64, 64 }, { "XboxRTrigger_colo", Symbol::R2, 64, 64 },
            { "AnalogStickL_colo", Symbol::LStick, 64, 64 }, { "AnalogStickR_colo", Symbol::RStick, 64, 64 },
        };
        std::vector<std::pair<std::string, Entry>> entries;
        entries.push_back({ dir + "PCSingleKeySmall_colo.png.1", {} });
        for (auto& b : kButtons)
            entries.push_back({ dir + b.file + ".png.1", {} });
        for (auto& b : kCompressed)
            entries.push_back({ dir + b.file + ".png.1", {} });
        if (!FindEntries(f, entries))
        {
            wet::Log("Prompts: keyboard art - the button textures weren't found in B.Texture.cache");
            std::fclose(f);
            return;
        }
        auto read = [&](const Entry& e, std::vector<uint8_t>& out) {
            out.resize(e.size);
            std::fseek(f, long(e.offset), SEEK_SET);
            return e.method == 0 && e.size == 64 * 64 * 4 && std::fread(out.data(), 1, out.size(), f) == out.size();
        };
        std::vector<uint8_t> cap;
        if (!read(entries[0].second, cap))
        {
            wet::Log("Prompts: keyboard art - the keycap isn't the expected 64x64 texture");
            std::fclose(f);
            return;
        }
        // Untile the keycap to RGBA.
        std::vector<Rgba> keycap(64 * 64);
        for (uint32_t y = 0; y < 64; y++)
            for (uint32_t x = 0; x < 64; x++)
            {
                const uint8_t* p = &cap[Tiled2D(x, y, 64, 2)];
                keycap[y * 64 + x] = { p[1], p[2], p[3], p[0] };
            }
        for (size_t i = 0; i < std::size(kButtons); i++)
        {
            const Entry& e = entries[i + 1].second;
            if (e.method != 0 || e.size != 64 * 64 * 4)
                continue;
            Label label;
            if (!wet::prompts::KeyboardLabelFor(kButtons[i].sym, label))
                continue;   // not bound: the Xbox button stays
            std::vector<Rgba> px = KeycapWith(keycap, label);
            Patch patch;
            patch.offset = e.offset;
            patch.name = kButtons[i].file;
            patch.bytes.resize(64 * 64 * 4);
            for (uint32_t y = 0; y < 64; y++)
                for (uint32_t x = 0; x < 64; x++)
                {
                    const Rgba& p = px[y * 64 + x];
                    uint8_t* d = &patch.bytes[Tiled2D(x, y, 64, 2)];
                    d[0] = p.a;
                    d[1] = p.r;
                    d[2] = p.g;
                    d[3] = p.b;
                }
            wet::Log("Prompts: keyboard art for %s: %s", kButtons[i].file,
                     label.kind == Label::Kind::Mouse ? "a mouse button" : label.text.c_str());
            s_patches.push_back(std::move(patch));
        }
        // The compressed ones: unpack, draw the keycap (centred, at the face
        // buttons' size) with all its mip levels, keep each chunk's stored bytes.
        for (size_t i = 0; i < std::size(kCompressed); i++)
        {
            const Compressed& c = kCompressed[i];
            const Entry& e = entries[1 + std::size(kButtons) + i].second;
            if (e.method != 64 || e.size < 8 || e.size > (1u << 20))
                continue;
            Label label;
            if (!wet::prompts::KeyboardLabelFor(c.sym, label))
                continue;
            std::vector<uint8_t> stored(e.size);
            std::fseek(f, long(e.offset), SEEK_SET);
            if (std::fread(stored.data(), 1, stored.size(), f) != stored.size())
                continue;
            DxtPatch patch;
            patch.name = c.file;
            bool ok = true;
            for (size_t p = 0; ok && p < stored.size();)
            {
                if (p + 8 > stored.size())
                {
                    ok = false;
                    break;
                }
                uint32_t cs = Be32(&stored[p]), us = Be32(&stored[p + 4]);
                if (us != 16384 || p + 8 + cs > stored.size())
                {
                    ok = false;
                    break;
                }
                size_t before = patch.bytes.size();
                ok = Lzf(&stored[p + 8], cs, patch.bytes) && patch.bytes.size() == before + us;
                patch.chunks.emplace_back(stored.begin() + long(p + 8), stored.begin() + long(p + 8 + cs));
                p += 8 + cs;
            }
            if (!ok || patch.bytes.size() < 3 * 16384)
            {
                wet::Log("Prompts: keyboard art - %s isn't in the expected format", c.file);
                continue;
            }
            std::vector<Rgba> icon(size_t(c.w * c.h), Rgba{ 0, 0, 0, 0 });
            std::vector<Rgba> cap = KeycapWith(keycap, label);
            int x0 = (c.w - 64) / 2, y0 = (c.h - 64) / 2;
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++)
                    icon[size_t((y + y0) * c.w + x + x0)] = cap[size_t(y * 64 + x)];
            WriteDxt5Mips(std::move(icon), c.w, c.h, patch.bytes);
            wet::Log("Prompts: keyboard art for %s: %s", c.file,
                     label.kind == Label::Kind::Mouse ? "a mouse button" : label.text.c_str());
            s_dxtPatches.push_back(std::move(patch));
        }
        s_haveDxt = !s_dxtPatches.empty();
        std::fclose(f);
    }
}

namespace wet::keyprompts
{
    bool IsTextureCache(const std::string& lowerName) { return lowerName == "b.texture.cache"; }

    // After the game read [fileOffset, +length) of B.Texture.cache into guest memory.
    void PatchRead(const std::string& cachePath, uint64_t fileOffset, uint32_t buffer, uint32_t length)
    {
        std::lock_guard g(s_lock);
        if (!s_ready)
            Prepare(cachePath);
        for (auto& p : s_patches)
        {
            uint64_t a = std::max(fileOffset, p.offset), b = std::min(fileOffset + length, p.offset + p.bytes.size());
            if (a >= b)
                continue;
            for (uint64_t i = a; i < b; i++)
                St8(buffer + uint32_t(i - fileOffset), p.bytes[size_t(i - p.offset)]);
            if (!p.logged)
            {
                p.logged = true;
                Log("Prompts: %s read by the game - keyboard art written over it", p.name.c_str());
            }
        }
    }
}

// lzf_decompress(in, in size, out, out size): returns the size unpacked.
extern "C" void __imp__sub_828041C8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_828041C8)
{
    uint32_t in = ctx.r3.u32, inLen = ctx.r4.u32, out = ctx.r5.u32;
    __imp__sub_828041C8(ctx, base);
    if (!s_haveDxt || ctx.r3.u32 != 16384)
        return;
    std::lock_guard g(s_lock);
    for (auto& p : s_dxtPatches)
        for (size_t k = 0; k < p.chunks.size(); k++)
        {
            const auto& c = p.chunks[k];
            if (c.size() != inLen || std::memcmp(base + in, c.data(), inLen) != 0)
                continue;
            wet::PrepareHostWrite(out, 16384);
            std::memcpy(base + out, p.bytes.data() + k * 16384, 16384);
            if (!p.logged)
            {
                p.logged = true;
                wet::Log("Prompts: %s unpacked by the game - keyboard art written over it", p.name.c_str());
            }
            return;
        }
}
#endif
