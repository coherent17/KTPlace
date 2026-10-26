// @file kt_gif.cc// Indexed canvas, 5x7 font, and GIF89a writer. See kt_gif.h.


#include "kt_gif.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <oneapi/tbb/parallel_for.h>

namespace ktplace {

namespace {

// --- 5x7 font -------------------------------------------------------------
//
// Column-major: five bytes per glyph, one per column, bit 0 the top row. This
// is the classic 5x7 cell used by small monochrome LCD libraries, covering
// printable ASCII 0x20-0x7E.
constexpr int fontFirst = 0x20;
constexpr int fontLast = 0x7E;
constexpr int glyphW = 5;
constexpr int glyphH = 7;
constexpr int glyphAdvance = 6;  // 5 columns plus one of spacing

constexpr std::uint8_t font[][glyphW] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00},  // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00},  // '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14},  // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62},  // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50},  // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00},  // '\''
    {0x00, 0x1C, 0x22, 0x41, 0x00},  // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00},  // ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14},  // '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08},  // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00},  // ','
    {0x08, 0x08, 0x08, 0x08, 0x08},  // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00},  // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02},  // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46},  // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39},  // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03},  // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36},  // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00},  // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00},  // ';'
    {0x08, 0x14, 0x22, 0x41, 0x00},  // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14},  // '='
    {0x00, 0x41, 0x22, 0x14, 0x08},  // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06},  // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E},  // '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01},  // 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A},  // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31},  // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F},  // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63},  // 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07},  // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43},  // 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00},  // '['
    {0x02, 0x04, 0x08, 0x10, 0x20},  // '\\'
    {0x00, 0x41, 0x41, 0x7F, 0x00},  // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04},  // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40},  // '_'
    {0x00, 0x01, 0x02, 0x04, 0x00},  // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78},  // 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38},  // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20},  // 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F},  // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18},  // 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02},  // 'f'
    {0x0C, 0x52, 0x52, 0x52, 0x3E},  // 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78},  // 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00},  // 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00},  // 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00},  // 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00},  // 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78},  // 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78},  // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38},  // 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08},  // 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C},  // 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08},  // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20},  // 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20},  // 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C},  // 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C},  // 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C},  // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44},  // 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C},  // 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44},  // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00},  // '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00},  // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00},  // '}'
    {0x08, 0x08, 0x2A, 0x1C, 0x08},  // '~'
};

constexpr std::size_t fontEntries = (sizeof(font) / sizeof(font[0]));

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

Canvas::Canvas(int w, int h, const std::vector<Rgb> &palette)
    : pixelWidth(w > 0 ? w : 1), pixelHeight(h > 0 ? h : 1), colorTable(palette) {
    if (colorTable.size() > 256) {
        colorTable.resize(256);
    }
    pixelIndex.assign(static_cast<std::size_t>(pixelWidth) * static_cast<std::size_t>(pixelHeight),
                      0);
}

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

int Canvas::getNearest(const Rgb &c) const {
    int best = 0;
    long bestD = -1;
    for (std::size_t i = 0; i < colorTable.size(); ++i) {
        const long dr = static_cast<long>(c.r) - colorTable[i].r;
        const long dg = static_cast<long>(c.g) - colorTable[i].g;
        const long db = static_cast<long>(c.b) - colorTable[i].b;
        const long d = dr * dr + dg * dg + db * db;
        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = static_cast<int>(i);
            if (d == 0) {
                break;
            }
        }
    }
    return best;
}

void Canvas::blendPixel(int x, int y, const Rgb &src, double alpha) {
    if (x < 0 || y < 0 || x >= pixelWidth || y >= pixelHeight || colorTable.empty()) {
        return;
    }
    Rgb dst;
    if (alpha >= 1.0) {
        dst = src;
    } else {
        const std::uint8_t here = pixelIndex[static_cast<std::size_t>(y) * pixelWidth + x];
        const Rgb &bg = colorTable[here < colorTable.size() ? here : 0];
        const double a = alpha < 0.0 ? 0.0 : alpha;
        dst.r = static_cast<std::uint8_t>(src.r * a + bg.r * (1.0 - a) + 0.5);
        dst.g = static_cast<std::uint8_t>(src.g * a + bg.g * (1.0 - a) + 0.5);
        dst.b = static_cast<std::uint8_t>(src.b * a + bg.b * (1.0 - a) + 0.5);
    }
    pixelIndex[static_cast<std::size_t>(y) * pixelWidth + x] =
        static_cast<std::uint8_t>(getNearest(dst));
}

void Canvas::clear(int idx) {
    std::fill(pixelIndex.begin(), pixelIndex.end(), static_cast<std::uint8_t>(idx));
}

void Canvas::fillRect(int x, int y, int w, int h, int idx, double alpha) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= colorTable.size()) {
        return;
    }
    const Rgb &c = colorTable[static_cast<std::size_t>(idx)];
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(pixelWidth, x + w);
    const int y1 = std::min(pixelHeight, y + h);
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            blendPixel(xx, yy, c, alpha);
        }
    }
}

void Canvas::hLine(int x0, int x1, int y, int idx) {
    if (x1 < x0) {
        std::swap(x0, x1);
    }
    fillRect(x0, y, x1 - x0 + 1, 1, idx);
}

void Canvas::vLine(int y0, int y1, int x, int idx) {
    if (y1 < y0) {
        std::swap(y0, y1);
    }
    fillRect(x, y0, 1, y1 - y0 + 1, idx);
}

int Canvas::getTextWidth(const std::string &s, int scale) {
    const int sc = scale > 0 ? scale : 1;
    return static_cast<int>(s.size()) * glyphAdvance * sc;
}

void Canvas::text(int x, int y, const std::string &s, int idx, int scale) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= colorTable.size()) {
        return;
    }
    const int sc = scale > 0 ? scale : 1;
    int penX = x;
    for (const char ch : s) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < fontFirst || u > fontLast) {
            penX += glyphAdvance * sc;
            continue;
        }
        const std::uint8_t *glyph = font[u - fontFirst];
        for (int col = 0; col < glyphW; ++col) {
            for (int row = 0; row < glyphH; ++row) {
                if (((glyph[col] >> row) & 1u) == 0u) {
                    continue;
                }
                fillRect(penX + col * sc, y + row * sc, sc, sc, idx);
            }
        }
        penX += glyphAdvance * sc;
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
