/**
 * @file kt_gif.h
 * @brief Self-contained indexed-colour canvas and GIF89a writer
 *
 * RePlAce renders each global-placement iteration to a JPEG with CImg and then
 * shells out to ImageMagick (`makegif.sh`: `convert -delay 20 cGP2D*.jpg
 * animated.gif`). CImg is not vendored in that repository -- `module/CImg` is
 * empty and the code is behind `ENABLE_CIMG_LIB` -- so reproducing it here
 * would mean adding a header-only imaging library plus libjpeg to a build that
 * currently plots with none, and still needing an external `convert`.
 *
 * A placement snapshot is a good fit for GIF's native format: flat colour over
 * a handful of categories (movable, fenced, fixed macro, pad, overlay), no
 * gradients, no anti-aliasing. So this writes GIF directly. The output is the
 * same artifact RePlAce produces -- one animated GIF of the iteration sequence
 * -- with no external process and no new link-time dependencies.
 *
 * Two pieces are here because nothing in the standard library provides them:
 * LZW compression, and a 5x7 bitmap font for the per-frame labels.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ktplace {

/// An RGB colour, 0-255 per channel.
struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

/**
 * @brief Indexed-colour raster image with the few primitives a placement plot
 *        needs: rectangles, lines and text.
 *
 * Alpha is resolved against whatever is already in the framebuffer by blending
 * in RGB and re-quantising to the nearest palette entry. That is not exact, but
 * the palette is flat and small, so the error is invisible -- and it keeps
 * every stored pixel a single byte, which is what GIF requires anyway.
 */
class Canvas {
public:
    /**
     * @brief Create a frame that quantises drawn colours onto @p palette.
     * @param w  image width in pixels
     * @param h  image height in pixels
     * @param palette  at most 256 colours; the image stores palette indices.
     */
    Canvas(int w, int h, const std::vector<Rgb> &palette);

    /**
     * @brief Adopt an already-quantised frame.
     *
     * Lets a caller that has already mapped pixels onto a palette -- an
     * animation assembled from several frames, which must all share one table
     * -- skip the index-to-colour search.
     *
     * @param w  image width in pixels
     * @param h  image height in pixels
     * @param indices  exactly w * h entries, one palette index per pixel,
     *                 row-major
     * @param palette  the table @p indices refer to; at most 256 colours
     */
    Canvas(int w, int h, const std::vector<std::uint8_t> &indices, const std::vector<Rgb> &palette);

    int width() const {
        return w_;
    }
    int height() const {
        return h_;
    }
    const std::vector<std::uint8_t> &pixels() const {
        return px_;
    }
    const std::vector<Rgb> &palette() const {
        return palette_;
    }

    /// Fill the whole image with palette index @p idx.
    void clear(int idx);

    /// Filled rectangle, clipped to the image. @p alpha blends with the
    /// existing pixel when it is below 1.0.
    void fillRect(int x, int y, int w, int h, int idx, double alpha = 1.0);

    /// One-pixel outline, drawn just inside the given rectangle.
    void strokeRect(int x, int y, int w, int h, int idx);

    void hLine(int x0, int x1, int y, int idx);
    void vLine(int y0, int y1, int x, int idx);

    /// Draw @p s with the built-in 5x7 font. @p scale magnifies by an integer
    /// factor. Characters outside the font's range are skipped.
    void text(int x, int y, const std::string &s, int idx, int scale = 1);

    /// Width in pixels that @p s would occupy at @p scale, including spacing.
    static int textWidth(const std::string &s, int scale = 1);

private:
    /// Blend @p src over the pixel at (x,y) and store the nearest palette index.
    void blendPixel(int x, int y, const Rgb &src, double alpha);

    /// Palette index of the colour nearest to @p c, by squared RGB distance.
    int nearest(const Rgb &c) const;

    int w_ = 0;
    int h_ = 0;
    std::vector<std::uint8_t> px_;
    std::vector<Rgb> palette_;
};

/**
 * @brief GIF's variable-width LZW, as a sequence of data sub-blocks ready to be
 *        appended to a file.
 * @param pixels  one palette index per pixel, row-major
 * @param minCodeSize  bits per pixel; 8 for a 256-colour image
 */
std::vector<std::uint8_t> gifCompress(const std::vector<std::uint8_t> &pixels, int minCodeSize);

/**
 * @brief Write the canvases as one animated GIF89a.
 *
 * A single canvas yields a still image. The Netscape looping extension is
 * emitted so that multi-frame files loop forever in a browser, which is what
 * makes the sequence readable as an animation.
 *
 * @param path  output file, created or truncated
 * @param frames  the frames to write, in order; all must share one palette and
 *                one size, and a single frame yields a still image
 * @param delayCs  per-frame delay in centiseconds
 * @return false if the file could not be opened
 */
bool writeGif(const std::string &path, const std::vector<Canvas> &frames, int delayCs = 20);

}  // namespace ktplace
