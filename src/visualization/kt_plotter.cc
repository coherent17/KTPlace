/**
 * @file kt_plotter.cc
 * @brief Implementation of the SVG/HTML/CSV placement visualization helpers
 */

#include "visualization/kt_plotter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ktplace {

namespace {

constexpr std::size_t kMaxPoints = 150000;  // max dots per frame
constexpr double kMargin = 36.0;            // image margin in pixels
constexpr double kImageW = 768.0;           // frame image size
constexpr double kImageH = 768.0;

std::string fmt(double v, int prec = 1) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

std::string sci(double v) {
    std::ostringstream os;
    os << std::scientific << std::setprecision(1) << v;
    return os.str();
}

struct ViewPort {
    double minX = 0.0, minY = 0.0, maxX = 1.0, maxY = 1.0;
    double sx = 1.0, sy = 1.0;  // units -> px
};

ViewPort makeViewPort(const Graph &g, const std::vector<float> &x, const std::vector<float> &y,
                      const BBox &dieBox) {
    const std::size_t nv = g.getNumVertices();
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = -std::numeric_limits<double>::max();
    double maxY = -std::numeric_limits<double>::max();
    for (std::size_t v = 0; v < nv; ++v) {
        minX = std::min(minX, static_cast<double>(x[v]));
        minY = std::min(minY, static_cast<double>(y[v]));
        maxX = std::max(maxX, static_cast<double>(x[v]));
        maxY = std::max(maxY, static_cast<double>(y[v]));
    }
    minX = std::min(minX, dieBox[0]);
    minY = std::min(minY, dieBox[1]);
    maxX = std::max(maxX, dieBox[2]);
    maxY = std::max(maxY, dieBox[3]);
    const double spanX = std::max(maxX - minX, 1.0);
    const double spanY = std::max(maxY - minY, 1.0);
    const double avail = kImageW - 2.0 * kMargin;
    const double sc = avail / std::max(spanX, spanY);
    ViewPort vp;
    vp.minX = minX - 0.01 * spanX;
    vp.minY = minY - 0.01 * spanY;
    vp.sx = sc;
    vp.sy = sc;
    return vp;
}

double toPxX(const ViewPort &vp, double v) {
    return kMargin + (v - vp.minX) * vp.sx;
}
double toPxY(const ViewPort &vp, double v) {
    return kImageW - kMargin - (v - vp.minY) * vp.sy;
}

}  // namespace

bool ensureDir(const std::string &dir) {
    if (dir.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

BBox fixedCellBBox(const Graph &g) {
    // The "die" is taken as the bounding box of all fixed/terminal cells.
    // Vertices store their lower-left corner, so the box must also include
    // corner + (width, height): pads anchored at the right/top rim would
    // otherwise overhang the drawn die rectangle.
    const std::size_t nv = g.getNumVertices();
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = -std::numeric_limits<double>::max();
    double maxY = -std::numeric_limits<double>::max();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell) {
            continue;
        }
        if (!vert.isFixed && !vert.isTerminal) {
            continue;
        }
        minX = std::min(minX, vert.x);
        minY = std::min(minY, vert.y);
        maxX = std::max(maxX, vert.x + std::max(vert.width, 1.0));
        maxY = std::max(maxY, vert.y + std::max(vert.height, 1.0));
    }
    if (minX == std::numeric_limits<double>::max()) {
        return {0.0, 0.0, 1.0, 1.0};
    }
    return {minX, minY, maxX, maxY};
}

void writeFrameSvg(const std::string &path, const Graph &g, const std::vector<float> &x,
                   const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                   std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                   const std::string &note) {
    const std::size_t nv = g.getNumVertices();
    const ViewPort vp = makeViewPort(g, x, y, dieBox);

    // Decimate so very large designs still produce small files.
    const std::size_t stride = (nv > kMaxPoints) ? ((nv + kMaxPoints - 1) / kMaxPoints) : 1;

    std::ofstream out(path);
    if (!out.is_open()) {
        return;
    }
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << kImageW << "\" height=\""
        << kImageH << "\" viewBox=\"0 0 " << kImageW << " " << kImageH << "\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#101418\"/>\n";
    out << "<title>step " << step << ": " << note << "</title>\n";

    const double pct = numSteps > 1
                           ? 100.0 * static_cast<double>(step) / static_cast<double>(numSteps - 1)
                           : 100.0;

    // Progress bar.
    out << "<rect x=\"" << kMargin / 3 << "\" y=\"" << kImageH - 14 << "\" width=\""
        << kImageW - 2 * kMargin / 3 << "\" height=\"6\" fill=\"#22303a\"/>\n";
    out << "<rect x=\"" << kMargin / 3 << "\" y=\"" << kImageH - 14 << "\" width=\""
        << (kImageW - 2 * kMargin / 3) * pct / 100.0 << "\" height=\"6\" fill=\"#4fc3f7\"/>\n";

    // Die (fixed-pad) frame.
    out << "<rect x=\"" << fmt(toPxX(vp, dieBox[0])) << "\" y=\"" << fmt(toPxY(vp, dieBox[3]))
        << "\" width=\"" << fmt((dieBox[2] - dieBox[0]) * vp.sx) << "\" height=\""
        << fmt((dieBox[3] - dieBox[1]) * vp.sy)
        << "\" fill=\"none\" stroke=\"#bdbdbd\" stroke-width=\"1\"/>\n";

    // Net overlay: a deterministic sample of nets drawn as translucent hulls
    // plus star joins from each pin to the net centroid, so long-span signals
    // read at a glance.  Sampled by stride so the count stays <= kMaxNets.
    std::size_t netsShown = 0;
    {
        std::vector<std::size_t> netIds;
        netIds.reserve(512);
        for (std::size_t v = 0; v < nv; ++v) {
            if (g.getVertex(v).type == VertexType::Net && !g.getVertex(v).inEdges.empty()) {
                netIds.push_back(v);
            }
        }
        const std::size_t kMaxNets = 350;
        const std::size_t nShow = std::min<std::size_t>(netIds.size(), kMaxNets);
        const std::size_t stepN =
            netIds.empty()
                ? 1
                : std::max<std::size_t>(1, netIds.size() / std::max<std::size_t>(nShow, 1));
        std::size_t shown = 0;
        out << "<g fill=\"none\" stroke=\"#8bc34a\" stroke-width=\"1\" opacity=\"0.6\">\n";
        std::vector<double> cx(x.begin(), x.end());
        std::vector<double> cy(y.begin(), y.end());
        for (std::size_t k = 0; k < netIds.size() && shown < kMaxNets; k += stepN) {
            const Vertex &net = g.getVertex(netIds[k]);
            std::vector<std::size_t> ids;
            ids.reserve(net.inEdges.size());
            for (std::size_t eid : net.inEdges) {
                ids.push_back(g.getEdge(eid).source);
            }
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            if (ids.size() < 2) {
                continue;
            }
            double minX = std::numeric_limits<double>::max();
            double minY = std::numeric_limits<double>::max();
            double maxX = -std::numeric_limits<double>::max();
            double maxY = -std::numeric_limits<double>::max();
            for (std::size_t cid : ids) {
                minX = std::min(minX, static_cast<double>(cx[cid]));
                minY = std::min(minY, static_cast<double>(cy[cid]));
                maxX = std::max(maxX, static_cast<double>(cx[cid]));
                maxY = std::max(maxY, static_cast<double>(cy[cid]));
            }
            const double mxx = 0.5 * (minX + maxX);
            const double myy = 0.5 * (minY + maxY);
            out << "<rect x=\"" << fmt(toPxX(vp, minX)) << "\" y=\"" << fmt(toPxY(vp, maxY))
                << "\" width=\"" << fmt((maxX - minX) * vp.sx) << "\" height=\""
                << fmt((maxY - minY) * vp.sy) << "\"/>\n";
            for (std::size_t cid : ids) {
                out << "<line x1=\"" << fmt(toPxX(vp, static_cast<double>(cx[cid]))) << "\" y1=\""
                    << fmt(toPxY(vp, static_cast<double>(cy[cid]))) << "\" x2=\""
                    << fmt(toPxX(vp, mxx)) << "\" y2=\"" << fmt(toPxY(vp, myy)) << "\"/>\n";
            }
            ++shown;
        }
        netsShown = shown;
        out << "</g>\n";
    }

    // Movable cells.
    out << "<g fill=\"#4fc3f7\" opacity=\"0.55\">\n";
    for (std::size_t v = 0; v < nv; v += stride) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
            continue;
        }
        const double w = std::max(1.0, vert.width * vp.sx);
        const double h = std::max(1.0, vert.height * vp.sy);
        out << "<rect x=\"" << fmt(toPxX(vp, x[v])) << "\" y=\""
            << fmt(toPxY(vp, y[v] + vert.height)) << "\" width=\"" << fmt(w, 3) << "\" height=\""
            << fmt(h, 3) << "\"/>\n";
    }
    out << "</g>\n";

    // Fixed macros: never decimate, so hard cells match the DEF floorplan.
    out << "<g fill=\"#ef5350\" opacity=\"0.9\">\n";
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell || !vert.isFixed) {
            continue;
        }
        const double w = std::max(2.0, vert.width * vp.sx);
        const double h = std::max(2.0, vert.height * vp.sy);
        out << "<rect x=\"" << fmt(toPxX(vp, x[v])) << "\" y=\""
            << fmt(toPxY(vp, y[v] + vert.height)) << "\" width=\"" << fmt(w, 3) << "\" height=\""
            << fmt(h, 3) << "\"/>\n";
    }
    // I/O pads / terminals: decimatable, there can be tens of thousands.
    for (std::size_t v = 0; v < nv; v += stride) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell || !vert.isTerminal || vert.isFixed) {
            continue;
        }
        const double w = std::max(2.0, vert.width * vp.sx);
        const double h = std::max(2.0, vert.height * vp.sy);
        out << "<rect x=\"" << fmt(toPxX(vp, x[v])) << "\" y=\""
            << fmt(toPxY(vp, y[v] + vert.height)) << "\" width=\"" << fmt(w, 3) << "\" height=\""
            << fmt(h, 3) << "\"/>\n";
    }
    out << "</g>\n";

    out << "<text x=\"" << kMargin / 3
        << "\" y=\"24\" fill=\"#ffffff\" font-family=\"monospace\" font-size=\"14\">" << "CG step "
        << step << " / " << (numSteps - 1) << " — " << note << "</text>\n";
    out << "<text x=\"" << kMargin / 3
        << "\" y=\"44\" fill=\"#90caf9\" font-family=\"monospace\" font-size=\"13\">"
        << "HPWL = " << fmt(hpwl, 3) << "  (initial " << fmt(hpwlInitial, 3) << ")</text>\n";
    if (resid > 0.0) {
        out << "<text x=\"" << kMargin / 3
            << "\" y=\"62\" fill=\"#ffeb3b\" font-family=\"monospace\" font-size=\"12\">"
            << "density overflow = " << sci(resid) << "</text>\n";
    }
    out << "<text x=\"" << kMargin / 3 << "\" y=\"" << (resid > 0.0 ? 80 : 62)
        << "\" fill=\"#8bc34a\" font-family=\"monospace\" font-size=\"12\">"
        << "nets: " << netsShown << " sampled (hull + pin stars)</text>\n";
    out << "</svg>\n";
    out.close();
}

void writeHpwlCurve(const std::string &csvPath, const std::string &svgPath,
                    const std::vector<std::pair<std::size_t, double>> &curve,
                    const std::vector<double> *residuals) {
    if (curve.empty()) {
        return;
    }

    {
        std::ofstream out(csvPath);
        if (out.is_open()) {
            out << "step,hpwl,overflow\n";
            for (std::size_t i = 0; i < curve.size(); ++i) {
                out << curve[i].first << "," << std::setprecision(10) << curve[i].second;
                if (residuals && residuals->size() == curve.size() && (*residuals)[i] > 0.0) {
                    out << "," << std::setprecision(6) << std::scientific << (*residuals)[i];
                } else {
                    out << ",";
                }
                out << "\n";
            }
            out.close();
        }
    }

    const double cw = 900.0, ch = 300.0, l = 60.0, r = 20.0, t = 30.0, b = 40.0;
    const double maxStep = static_cast<double>(curve.back().first);
    double hpwlMin = std::numeric_limits<double>::max();
    double hpwlMax = -std::numeric_limits<double>::max();
    for (const auto &[s, h] : curve) {
        hpwlMin = std::min(hpwlMin, h);
        hpwlMax = std::max(hpwlMax, h);
    }
    const double hpwlSpan = std::max(hpwlMax - hpwlMin, 1e-9);

    // Residual range (optional secondary curve, normalized to its own span).
    double rMin = 0.0, rMax = 0.0;
    bool hasResid = residuals && residuals->size() == curve.size();
    if (hasResid) {
        rMin = std::numeric_limits<double>::max();
        rMax = -std::numeric_limits<double>::max();
        for (double rv : *residuals) {
            if (rv <= 0.0)
                continue;
            rMin = std::min(rMin, rv);
            rMax = std::max(rMax, rv);
        }
        if (rMin > rMax) {
            hasResid = false;
        }
    }
    const double rSpan = hasResid ? std::max(rMax - rMin, 1e-300) : 1.0;

    std::ofstream out(svgPath);
    if (!out.is_open()) {
        return;
    }
    auto px = [&](double s) {
        return l + (s / std::max(maxStep, 1.0)) * (cw - l - r);
    };
    auto py = [&](double h) {
        return t + (1.0 - (h - hpwlMin) / hpwlSpan) * (ch - t - b);
    };
    auto pry = [&](double rv) {
        return t + (1.0 - (rv - rMin) / rSpan) * (ch - t - b);
    };

    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << cw << "\" height=\"" << ch
        << "\" viewBox=\"0 0 " << cw << " " << ch << "\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#101418\"/>\n";
    out << "<line x1=\"" << l << "\" y1=\"" << t << "\" x2=\"" << l << "\" y2=\"" << ch - b
        << "\" stroke=\"#455\" stroke-width=\"1\"/>\n";
    out << "<line x1=\"" << l << "\" y1=\"" << ch - b << "\" x2=\"" << cw - r << "\" y2=\""
        << ch - b << "\" stroke=\"#455\" stroke-width=\"1\"/>\n";

    // Baseline: initial HPWL.
    out << "<line x1=\"" << px(0.0) << "\" y1=\"" << py(curve.front().second) << "\" x2=\""
        << px(maxStep) << "\" y2=\"" << py(curve.front().second)
        << "\" stroke=\"#ef5350\" stroke-width=\"1\" stroke-dasharray=\"4,4\"/>\n";

    out << "<polyline fill=\"none\" stroke=\"#4fc3f7\" stroke-width=\"2\" points=\"";
    for (const auto &[s, h] : curve) {
        out << fmt(px(static_cast<double>(s)), 2) << "," << fmt(py(h), 2) << " ";
    }
    out << "\"/>\n";

    if (hasResid) {
        out << "<polyline fill=\"none\" stroke=\"#ffeb3b\" stroke-width=\"1.5\" "
               "stroke-dasharray=\"2,2\" points=\"";
        for (std::size_t i = 0; i < curve.size(); ++i) {
            if ((*residuals)[i] <= 0.0)
                continue;
            out << fmt(px(static_cast<double>(curve[i].first)), 2) << ","
                << fmt(pry((*residuals)[i]), 2) << " ";
        }
        out << "\"/>\n";
    }

    for (const auto &[s, h] : curve) {
        out << "<circle cx=\"" << fmt(px(static_cast<double>(s)), 2) << "\" cy=\"" << fmt(py(h), 2)
            << "\" r=\"2.5\" fill=\"" << (h <= hpwlMin + 0.01 * hpwlSpan ? "#ffeb3b" : "#4fc3f7")
            << "\"/>\n";
    }

    out << "<text x=\"" << cw / 2 - 40
        << "\" y=\"16\" fill=\"#ffffff\" font-family=\"monospace\" font-size=\"13\">"
        << "HPWL (blue) vs outer step" << (hasResid ? "  +  density overflow (yellow)" : "")
        << "</text>\n";
    if (hasResid) {
        out << "<text x=\"" << cw / 2 - 40
            << "\" y=\"30\" fill=\"#9aa\" font-family=\"monospace\" font-size=\"11\">"
            << "residual " << sci(rMax) << " -> " << sci(rMin) << "</text>\n";
    }
    out << "<text x=\"" << l - 8 << "\" y=\"" << ch - b + 18
        << "\" fill=\"#9aa\" font-family=\"monospace\" font-size=\"11\" text-anchor=\"end\">"
        << fmt(hpwlMax, 3) << " max</text>\n";
    out << "<text x=\"" << l << "\" y=\"" << py(hpwlMax) + 16
        << "\" fill=\"#9aa\" font-family=\"monospace\" font-size=\"11\">" << fmt(hpwlMin, 3)
        << " min</text>\n";
    out << "</svg>\n";
    out.close();
}

void writeGallery(const std::string &dir, const std::vector<std::string> &framePaths,
                  const std::string &csvName) {
    std::ofstream out((std::filesystem::path(dir) / "index.html").string());
    if (!out.is_open()) {
        return;
    }
    out << "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
        << "<title>KTPlace placement visualization</title>\n"
        << "<style>\n"
        << "body{font-family:sans-serif;margin:2em;background:#0b0f12;color:#ddd;}\n"
        << "h1{color:#fff;}\n"
        << ".curve img{max-width:920px;border:1px solid #333;}\n"
        << ".frame{display:inline-block;margin:6px;}\n"
        << ".frame img{max-width:360px;border:1px solid #333;background:#101418;}\n"
        << ".frame div{font-size:12px;color:#9aa;}\n"
        << "</style>\n</head>\n<body>\n"
        << "<h1>KTPlace placement visualization</h1>\n"
        << "<p>Snapshots taken during global placement. Movable cells in blue, fixed pads in red, "
        << "dashed line = initial HPWL baseline. The yellow dashed curve is the density overflow; "
        << "it dropping toward zero means cells spread across the die instead of piling up. "
        << "Green hulls / pin-stars are a sampled net overlay: each rect is a chosen net's "
        << "bounding box; the star lines join its pins to the net centroid, so long-span "
        << "signals read at a glance. "
        << "HPWL may rise at first as the spreading force opens up collapsed regions — that is "
        << "expected for global placement (see README).</p>\n";
    out << "<div class=\"curve\"><img src=\"hpwl.svg\" alt=\"HPWL curve\"></div>\n"
        << "<p>Raw data: <a href=\"" << csvName << "\">" << csvName << "</a></p>\n";
    for (const auto &f : framePaths) {
        out << "<div class=\"frame\"><img src=\"" << f << "\"><div>" << f << "</div></div>\n";
    }
    out << "</body>\n</html>\n";
    out.close();
}

}  // namespace ktplace
