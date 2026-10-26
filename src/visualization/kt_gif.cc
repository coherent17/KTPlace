// @file kt_gif.cc// Indexed frame and GIF89a writer. See kt_gif.h.


#include "kt_gif.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <oneapi/tbb/parallel_for.h>

namespace ktplace {

namespace {

/// LSB-first bit packer: GIF codes go out least-significant bit first.
class BitWriter {
public:
    void put(std::uint32_t code, int bits) {
        for (int i = 0; i < bits; ++i) {
            acc |= ((code >> i) & 1u) << nbits;
            if (++nbits == 8) {
                buffer.push_back(static_cast<std::uint8_t>(acc));
                acc = 0;
                nbits = 0;
            }
        }
    }

    /// Pad the final partial byte with zeros, as the format requires.
    void flush() {
        if (nbits > 0) {
            buffer.push_back(static_cast<std::uint8_t>(acc));
            acc = 0;
            nbits = 0;
        }
    }

    std::vector<std::uint8_t> &getBytes() {
        return buffer;
    }

private:
    std::vector<std::uint8_t> buffer;
    std::uint32_t acc = 0;
    int nbits = 0;
};

void put16(std::vector<std::uint8_t> &v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}

void putStr(std::vector<std::uint8_t> &v, const char *s) {
    while (*s != '\0') {
        v.push_back(static_cast<std::uint8_t>(*s++));
    }
}

/// Wrap @p data in GIF's sub-block framing: a length byte (1-255) then payload.
void putSubBlocks(std::vector<std::uint8_t> &v, const std::vector<std::uint8_t> &data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const std::size_t n = std::min<std::size_t>(255, data.size() - off);
        v.push_back(static_cast<std::uint8_t>(n));
        v.insert(v.end(), data.begin() + static_cast<long>(off),
                 data.begin() + static_cast<long>(off + n));
        off += n;
    }
    v.push_back(0);  // block terminator
}

}  // namespace

// --- Canvas ---------------------------------------------------------------

Canvas::Canvas(int w, int h, const std::vector<std::uint8_t> &indices,
               const std::vector<Rgb> &palette)
    : pixelWidth(w > 0 ? w : 1),
      pixelHeight(h > 0 ? h : 1),
      pixelIndex(indices),
      colorTable(palette) {
    if (colorTable.size() > 256) {
        colorTable.resize(256);
    }
    const std::size_t want =
        static_cast<std::size_t>(pixelWidth) * static_cast<std::size_t>(pixelHeight);
    if (pixelIndex.size() != want) {
        pixelIndex.resize(want, 0);
    }
    // Drop any index the (possibly truncated) palette cannot address.
    const std::uint8_t top =
        colorTable.empty() ? 0 : static_cast<std::uint8_t>(colorTable.size() - 1);
    for (std::uint8_t &v : pixelIndex) {
        if (v > top) {
            v = 0;
        }
    }
}

// --- LZW ------------------------------------------------------------------

std::vector<std::uint8_t> gifCompress(const std::vector<std::uint8_t> &pixels, int minCodeSize) {
    std::vector<std::uint8_t> body;
    if (minCodeSize < 2 || minCodeSize > 8) {
        minCodeSize = 8;
    }
    if (pixels.empty()) {
        return body;
    }

    const std::uint32_t clearCode = 1u << minCodeSize;  // 256
    const std::uint32_t eoiCode = clearCode + 1u;       // 257
    const std::uint32_t firstFree = eoiCode + 1u;       // 258
    constexpr std::uint32_t maxCode = 4096;             // 12-bit ceiling

    BitWriter bw;
    bw.put(clearCode, minCodeSize + 1);

    constexpr std::uint32_t slots = 1u << 13;
    constexpr std::uint32_t empty = 0xffffffffu;
    std::vector<std::uint32_t> keys(slots, empty);
    std::vector<std::uint16_t> codes(slots, 0);
    std::uint32_t next = firstFree;
    int codeSize = minCodeSize + 1;

    const auto home = [](std::uint32_t key) {
        return (key * 2654435761u) >> (32 - 13);
    };
    const auto find = [&](std::uint32_t key) {
        std::uint32_t h = home(key);
        while (keys[h] != empty && keys[h] != key) {
            h = (h + 1) & (slots - 1);
        }
        return h;
    };
    auto resetDict = [&]() {
        std::ranges::fill(keys, empty);
        next = firstFree;
        codeSize = minCodeSize + 1;
    };

    std::uint32_t prefix = pixels[0];
    for (std::size_t i = 1; i < pixels.size(); ++i) {
        const std::uint32_t k = pixels[i];
        const std::uint32_t key = (prefix << 8) | k;
        const std::uint32_t at = find(key);
        if (keys[at] == key) {
            prefix = codes[at];
            continue;
        }
        bw.put(prefix, codeSize);
        if (next < maxCode) {
            keys[at] = key;
            codes[at] = static_cast<std::uint16_t>(next++);
            // Widen one code later than the naive "next no longer fits" bound.
            // Code (1 << codeSize) may be handed out while the width is still
            // codeSize bits, because that code is only ever *emitted* after this
            // bump. Decoders bump on the same schedule, so widening as soon as
            // next > maxCode desynchronises the stream and they reject it.
            if (next > (1u << codeSize) && codeSize < 12) {
                ++codeSize;
            }
        } else {
            bw.put(clearCode, codeSize);
            resetDict();
        }
        prefix = k;
    }
    bw.put(prefix, codeSize);
    bw.put(eoiCode, codeSize);
    bw.flush();
    putSubBlocks(body, bw.getBytes());
    return body;
}

// --- GIF container --------------------------------------------------------

bool writeGif(const std::string &path, const std::vector<Canvas> &frames, int delayCs) {
    if (frames.empty() || path.empty()) {
        return false;
    }
    // Every frame must agree on the palette, because the colour table is written
    // once for the whole file.
    const std::size_t npal = frames.front().getPalette().size();
    if (npal < 1 || npal > 256) {
        return false;
    }
    const int w = frames.front().getWidth();
    const int h = frames.front().getHeight();
    for (const Canvas &c : frames) {
        if (c.getWidth() != w || c.getHeight() != h || c.getPalette().size() != npal) {
            return false;
        }
    }

    // A non-power-of-two table would be padded by the format, so round up and
    // pad with black to keep the indices meaningful. The LZW minimum code size
    // is also never below 2: a 2-colour image is encoded with 2 bits, not 1, so
    // the colour table and the code size stay consistent with each other.
    int bitsPerPixel = 2;
    while ((1 << bitsPerPixel) < static_cast<int>(npal)) {
        ++bitsPerPixel;
    }
    if (bitsPerPixel > 8) {
        return false;
    }
    const int tableSize = 1 << bitsPerPixel;

    std::vector<std::uint8_t> f;
    putStr(f, "GIF89a");
    put16(f, static_cast<std::uint32_t>(w));
    put16(f, static_cast<std::uint32_t>(h));
    // Global colour table present, 8-bit colour resolution, table size.
    f.push_back(static_cast<std::uint8_t>(0x80 | (7u << 4) |
                                          (static_cast<std::uint32_t>(bitsPerPixel) - 1u)));
    f.push_back(0);  // background colour index
    f.push_back(0);  // pixel aspect ratio

    const std::vector<Rgb> &pal = frames.front().getPalette();
    for (int i = 0; i < tableSize; ++i) {
        const Rgb c = i < static_cast<int>(pal.size()) ? pal[static_cast<std::size_t>(i)] : Rgb{};
        f.push_back(c.r);
        f.push_back(c.g);
        f.push_back(c.b);
    }

    if (frames.size() > 1) {
        // Netscape application extension: loop forever.
        f.push_back(0x21);
        f.push_back(0xFF);
        f.push_back(0x0B);
        putStr(f, "NETSCAPE2.0");
        f.push_back(0x03);
        f.push_back(0x01);
        put16(f, 0);  // repeat count 0 == infinite
        f.push_back(0x00);
    }

    std::vector<std::vector<std::uint8_t>> bodies(frames.size());
    tbb::parallel_for(std::size_t{0}, frames.size(), [&](std::size_t i) {
        bodies[i] = gifCompress(frames[i].getPixels(), bitsPerPixel);
    });

    const std::uint32_t delay = delayCs > 0 ? static_cast<std::uint32_t>(delayCs) : 0u;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        f.push_back(0x21);  // graphic control extension
        f.push_back(0xF9);
        f.push_back(0x04);
        // Packed field: bits 0-2 reserved (must be 0), bits 3-5 disposal method,
        // bit 6 user input, bit 7 transparent flag. Disposal 1 ("leave as is") is
        // right here because every frame paints the whole canvas opaque, so there
        // is nothing to erase between frames.
        f.push_back(static_cast<std::uint8_t>(1u << 3));
        put16(f, delay);
        f.push_back(0);  // transparent colour index, unused
        f.push_back(0x00);

        f.push_back(0x2C);  // image descriptor
        put16(f, 0);
        put16(f, 0);
        put16(f, static_cast<std::uint32_t>(w));
        put16(f, static_cast<std::uint32_t>(h));
        f.push_back(0x00);  // no local colour table, not interlaced

        f.push_back(static_cast<std::uint8_t>(bitsPerPixel));
        f.insert(f.end(), bodies[i].begin(), bodies[i].end());
    }
    f.push_back(0x3B);  // trailer

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char *>(f.data()), static_cast<long>(f.size()));
    out.close();
    return true;
}

}  // namespace ktplace
