// @file kt_gif.h// Self-contained indexed-colour canvas and GIF89a writer// RePlAce renders each global-placement iteration to a JPEG with CImg and then// shells out to ImageMagick (`makegif.sh`: `convert -delay 20 cGP2D*.jpg// animated.gif`). CImg is not vendored in that repository -- `module/CImg` is// empty and the code is behind `ENABLE_CIMG_LIB` -- so reproducing it here// would mean adding a header-only imaging library plus libjpeg to a build that// currently plots with none, and still needing an external `convert`.// A placement snapshot is a good fit for GIF's native format: flat colour over// a handful of categories (movable, fenced, fixed macro, pad, overlay), no// gradients, no anti-aliasing. So this writes GIF directly. The output is the// same artifact RePlAce produces -- one animated GIF of the iteration sequence// -- with no external process and no new link-time dependencies.// LZW compression is here because nothing in the standard library provides it.


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

// One indexed-colour frame of a GIF: palette indices, row-major, and the palette.

class Canvas {
public:
    Canvas(int w, int h, const std::vector<std::uint8_t> &indices, const std::vector<Rgb> &palette);

    int getWidth() const {
        return pixelWidth;
    }
    int getHeight() const {
        return pixelHeight;
    }
    const std::vector<std::uint8_t> &getPixels() const {
        return pixelIndex;
    }
    const std::vector<Rgb> &getPalette() const {
        return colorTable;
    }

private:
    int pixelWidth = 0;
    int pixelHeight = 0;
    std::vector<std::uint8_t> pixelIndex;
    std::vector<Rgb> colorTable;
};

// GIF's variable-width LZW, as a sequence of data sub-blocks ready to be// appended to a file.

std::vector<std::uint8_t> gifCompress(const std::vector<std::uint8_t> &pixels, int minCodeSize);

// Write the canvases as one animated GIF89a.// A single canvas yields a still image. The Netscape looping extension is// emitted so that multi-frame files loop forever in a browser, which is what// makes the sequence readable as an animation.// one size, and a single frame yields a still image

bool writeGif(const std::string &path, const std::vector<Canvas> &frames, int delayCs = 20);

}  // namespace ktplace
