// @file kt_gif.cc// Indexed canvas, 5x7 font, and GIF89a writer. See kt_gif.h.


#include "kt_gif.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace ktplace {

namespace {

// --- 5x7 font -------------------------------------------------------------
//
// Column-major: five bytes per glyph, one per column, bit 0 the top row. This
// is the classic 5x7 cell used by small monochrome LCD libraries, covering
// printable ASCII 0x20-0x7E.
constexpr int kFontFirst = 0x20;
constexpr int kFontLast = 0x7E;
constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kGlyphAdvance = 6;  // 5 columns plus one of spacing

constexpr std::uint8_t kFont[][kGlyphW] = {
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

constexpr std::size_t kFontEntries = (sizeof(kFont) / sizeof(kFont[0]));

/// LSB-first bit packer: GIF codes go out least-significant bit first.
class BitWriter {
public:
    void put(std::uint32_t code, int bits) {
        for (int i = 0; i < bits; ++i) {
            acc_ |= ((code >> i) & 1u) << nbits_;
            if (++nbits_ == 8) {
                out_.push_back(static_cast<std::uint8_t>(acc_));
                acc_ = 0;
                nbits_ = 0;
            }
        }
    }

    /// Pad the final partial byte with zeros, as the format requires.
    void flush() {
        if (nbits_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(acc_));
            acc_ = 0;
            nbits_ = 0;
        }
    }

    std::vector<std::uint8_t> &bytes() {
        return out_;
    }

private:
    std::vector<std::uint8_t> out_;
    std::uint32_t acc_ = 0;
    int nbits_ = 0;
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
    : w_(w > 0 ? w : 1), h_(h > 0 ? h : 1), palette_(palette) {
    if (palette_.size() > 256) {
        palette_.resize(256);
    }
    px_.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0);
}

Canvas::Canvas(int w, int h, const std::vector<std::uint8_t> &indices,
               const std::vector<Rgb> &palette)
    : w_(w > 0 ? w : 1), h_(h > 0 ? h : 1), px_(indices), palette_(palette) {
    if (palette_.size() > 256) {
        palette_.resize(256);
    }
    const std::size_t want = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
    if (px_.size() != want) {
        px_.resize(want, 0);
    }
    // Drop any index the (possibly truncated) palette cannot address.
    const std::uint8_t top = palette_.empty() ? 0 : static_cast<std::uint8_t>(palette_.size() - 1);
    for (std::uint8_t &v : px_) {
        if (v > top) {
            v = 0;
        }
    }
}

int Canvas::nearest(const Rgb &c) const {
    int best = 0;
    long bestD = -1;
    for (std::size_t i = 0; i < palette_.size(); ++i) {
        const long dr = static_cast<long>(c.r) - palette_[i].r;
        const long dg = static_cast<long>(c.g) - palette_[i].g;
        const long db = static_cast<long>(c.b) - palette_[i].b;
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
    if (x < 0 || y < 0 || x >= w_ || y >= h_ || palette_.empty()) {
        return;
    }
    Rgb dst;
    if (alpha >= 1.0) {
        dst = src;
    } else {
        const std::uint8_t here = px_[static_cast<std::size_t>(y) * w_ + x];
        const Rgb &bg = palette_[here < palette_.size() ? here : 0];
        const double a = alpha < 0.0 ? 0.0 : alpha;
        dst.r = static_cast<std::uint8_t>(src.r * a + bg.r * (1.0 - a) + 0.5);
        dst.g = static_cast<std::uint8_t>(src.g * a + bg.g * (1.0 - a) + 0.5);
        dst.b = static_cast<std::uint8_t>(src.b * a + bg.b * (1.0 - a) + 0.5);
    }
    px_[static_cast<std::size_t>(y) * w_ + x] = static_cast<std::uint8_t>(nearest(dst));
}

void Canvas::clear(int idx) {
    std::fill(px_.begin(), px_.end(), static_cast<std::uint8_t>(idx));
}

void Canvas::fillRect(int x, int y, int w, int h, int idx, double alpha) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= palette_.size()) {
        return;
    }
    const Rgb &c = palette_[static_cast<std::size_t>(idx)];
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(w_, x + w);
    const int y1 = std::min(h_, y + h);
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

int Canvas::textWidth(const std::string &s, int scale) {
    const int sc = scale > 0 ? scale : 1;
    return static_cast<int>(s.size()) * kGlyphAdvance * sc;
}

void Canvas::text(int x, int y, const std::string &s, int idx, int scale) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= palette_.size()) {
        return;
    }
    const int sc = scale > 0 ? scale : 1;
    int penX = x;
    for (const char ch : s) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < kFontFirst || u > kFontLast) {
            penX += kGlyphAdvance * sc;
            continue;
        }
        const std::uint8_t *glyph = kFont[u - kFontFirst];
        for (int col = 0; col < kGlyphW; ++col) {
            for (int row = 0; row < kGlyphH; ++row) {
                if (((glyph[col] >> row) & 1u) == 0u) {
                    continue;
                }
                fillRect(penX + col * sc, y + row * sc, sc, sc, idx);
            }
        }
        penX += kGlyphAdvance * sc;
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
    constexpr std::uint32_t kMaxCode = 4096;            // 12-bit ceiling

    BitWriter bw;
    bw.put(clearCode, minCodeSize + 1);

    // Dictionary keyed by (prefix << 8) | nextByte. A prefix is any code, not
    // just a byte, so the table has to span 4096 codes by 256 suffixes.
    std::vector<std::uint32_t> dict(1u << 20, 0);
    std::uint32_t next = firstFree;
    int codeSize = minCodeSize + 1;

    auto resetDict = [&]() {
        std::fill(dict.begin(), dict.end(), 0u);
        next = firstFree;
        codeSize = minCodeSize + 1;
    };

    std::uint32_t prefix = pixels[0];
    for (std::size_t i = 1; i < pixels.size(); ++i) {
        const std::uint32_t k = pixels[i];
        const std::uint32_t slot = (prefix << 8) | k;
        if (dict[slot] != 0) {
            prefix = dict[slot];
            continue;
        }
        bw.put(prefix, codeSize);
        if (next < kMaxCode) {
            dict[slot] = next++;
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
    putSubBlocks(body, bw.bytes());
    return body;
}

// --- GIF container --------------------------------------------------------

bool writeGif(const std::string &path, const std::vector<Canvas> &frames, int delayCs) {
    if (frames.empty() || path.empty()) {
        return false;
    }
    // Every frame must agree on the palette, because the colour table is written
    // once for the whole file.
    const std::size_t npal = frames.front().palette().size();
    if (npal < 1 || npal > 256) {
        return false;
    }
    const int w = frames.front().width();
    const int h = frames.front().height();
    for (const Canvas &c : frames) {
        if (c.width() != w || c.height() != h || c.palette().size() != npal) {
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

    const std::vector<Rgb> &pal = frames.front().palette();
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

    const std::uint32_t delay = delayCs > 0 ? static_cast<std::uint32_t>(delayCs) : 0u;
    for (const Canvas &c : frames) {
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
        const std::vector<std::uint8_t> body = gifCompress(c.pixels(), bitsPerPixel);
        f.insert(f.end(), body.begin(), body.end());
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
