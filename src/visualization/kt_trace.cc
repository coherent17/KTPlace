// @file kt_trace.cc
// The trace file: writing it during a run, replaying it into a Visualizer

#include "visualization/kt_trace.h"

#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_visualizer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <type_traits>
#include <zlib.h>

namespace ktplace {

namespace {

constexpr char kMagic[8] = {'K', 'T', 'T', 'R', 'A', 'C', 'E', '\0'};

enum Tag : std::uint8_t {
    kOptions = 'O',
    kNetlist = 'N',
    kFences = 'R',
    kStage = 'S',
    kFrame = 'F',
    kDensity = 'D',
    kBounds = 'B',
    kHoldBack = 'H',
    kFinalImage = 'I',
    kFinish = 'E',
    kEnd = 'Z',
};

constexpr std::uint8_t kFixed = 1u << 0;
constexpr std::uint8_t kTerminal = 1u << 1;

/// The file stops inside a record: a run that died before it could close its
/// trace (a fatal error exits without unwinding), not a malformed one.
struct Truncated {};

// gzwrite and gzread take an unsigned count, so a big array goes in pieces.
constexpr std::size_t kChunk = std::size_t{1} << 30;

/// Reads the file back, failing loudly on anything short or malformed: a trace
/// that is drawn wrong is worse than one that is not drawn.
class In {
public:
    explicit In(const std::string &path) : file(path), f(gzopen(path.c_str(), "rb")) {
        if (f == nullptr) {
            ktlog.fatal("cannot open trace {}", path);
        }
        (void)gzbuffer(f, 1u << 20);
    }
    ~In() {
        if (f != nullptr) {
            gzclose(f);
        }
    }
    In(const In &) = delete;
    In &operator=(const In &) = delete;

    /// The next tag, or false at a clean end of file.
    bool tag(std::uint8_t &t) {
        const int c = gzgetc(f);
        if (c < 0) {
            return false;
        }
        t = static_cast<std::uint8_t>(c);
        return true;
    }

    void raw(void *p, std::size_t n) {
        auto *bytes = static_cast<unsigned char *>(p);
        while (n > 0) {
            const auto want = static_cast<unsigned>(std::min(n, kChunk));
            const int got = gzread(f, bytes, want);
            if (got <= 0) {
                throw Truncated{};
            }
            bytes += got;
            n -= static_cast<std::size_t>(got);
        }
    }

    template <class T>
    T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v;
        raw(&v, sizeof v);
        return v;
    }

    /// An array length or a step count. Settings that are plain numbers, a byte
    /// cap say, are read with get<> instead, since nothing bounds them.
    std::size_t size() {
        const auto n = get<std::uint64_t>();
        // A count no array in a placement comes near is a corrupt file, and
        // trusting it would allocate the machine's memory before failing.
        if (n > (std::uint64_t{1} << 34)) {
            ktlog.fatal("trace {} holds an implausible count ({})", file, n);
        }
        return static_cast<std::size_t>(n);
    }

    std::string str() {
        const auto n = get<std::uint32_t>();
        std::string s(n, '\0');
        raw(s.data(), n);
        return s;
    }

    template <class T>
    std::vector<T> vec() {
        std::vector<T> v(size());
        raw(v.data(), v.size() * sizeof(T));
        return v;
    }

    std::array<double, 4> box() {
        std::array<double, 4> b{};
        raw(b.data(), sizeof b);
        return b;
    }

    const std::string file;

private:
    gzFile f;
};

/// The one netlist the trace describes, rebuilt as a Graph the drawing code can
/// read. Names are not recorded -- nothing draws them -- so they are made up.
struct Netlist {
    Graph graph;
    constraintMgr fences;
    bool haveGraph = false;
    bool haveFences = false;

    const constraintMgr *fencesFor(bool with) const {
        return with && haveFences ? &fences : nullptr;
    }
};

void readNetlist(In &in, Netlist &net) {
    const std::vector<double> w = in.vec<double>();
    const std::vector<double> h = in.vec<double>();
    const std::vector<double> x = in.vec<double>();
    const std::vector<double> y = in.vec<double>();
    const std::vector<std::uint8_t> flags = in.vec<std::uint8_t>();
    const std::size_t n = w.size();
    if (h.size() != n || x.size() != n || y.size() != n || flags.size() != n) {
        ktlog.fatal("trace {}: cell arrays of different lengths", in.file);
    }
    net.graph.clear();
    for (std::size_t v = 0; v < n; ++v) {
        Vertex &cell = net.graph.getCell(net.graph.addCell("c" + std::to_string(v)));
        cell.width = w[v];
        cell.height = h[v];
        cell.x = x[v];
        cell.y = y[v];
        cell.isFixed = (flags[v] & kFixed) != 0;
        cell.isTerminal = (flags[v] & kTerminal) != 0;
    }
    net.haveGraph = true;
}

void readFences(In &in, Netlist &net) {
    const std::size_t regions = in.size();
    for (std::size_t r = 0; r < regions; ++r) {
        std::string name = in.str();
        const std::vector<double> corners = in.vec<double>();
        std::vector<Point> points;
        points.reserve(corners.size() / 2);
        for (std::size_t i = 0; i + 3 < corners.size(); i += 4) {
            points.push_back(Point{corners[i], corners[i + 1]});
            points.push_back(Point{corners[i + 2], corners[i + 3]});
        }
        (void)net.fences.addRegion(std::move(name), points);
    }
    net.haveFences = true;
}

void requireGraph(const In &in, const Netlist &net) {
    if (!net.haveGraph) {
        ktlog.fatal("trace {}: a snapshot comes before the netlist it draws", in.file);
    }
}

}  // namespace

// ---------------------------------------------------------------------------

/// Appends to the file. A write that fails is fatal for the same reason the
/// reader is strict: a trace with a hole in it draws a run that did not happen.
class TraceWriter::Out {
public:
    explicit Out(const std::string &path) : file(path), f(gzopen(path.c_str(), "wb1")) {
        // Level 1: the solver waits on this, and placement coordinates do not
        // compress much harder at the higher levels.
        if (f == nullptr) {
            ktlog.fatal("cannot open trace {} for writing", path);
        }
        (void)gzbuffer(f, 1u << 20);
    }
    ~Out() {
        close();
    }
    Out(const Out &) = delete;
    Out &operator=(const Out &) = delete;

    void close() {
        if (f != nullptr) {
            if (gzclose(f) != Z_OK) {
                ktlog.warning("trace {} may be incomplete: closing it failed", file);
            }
            f = nullptr;
        }
    }

    void raw(const void *p, std::size_t n) {
        const auto *bytes = static_cast<const unsigned char *>(p);
        while (n > 0) {
            const auto len = static_cast<unsigned>(std::min(n, kChunk));
            if (gzwrite(f, bytes, len) != static_cast<int>(len)) {
                ktlog.fatal("cannot write trace {}", file);
            }
            bytes += len;
            n -= len;
        }
    }

    template <class T>
    void put(T v) {
        static_assert(std::is_trivially_copyable_v<T>);
        raw(&v, sizeof v);
    }

    void size(std::size_t n) {
        put<std::uint64_t>(n);
    }

    void str(const std::string &s) {
        put<std::uint32_t>(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
    }

    template <class T>
    void vec(const std::vector<T> &v) {
        size(v.size());
        raw(v.data(), v.size() * sizeof(T));
    }

    void box(const std::array<double, 4> &b) {
        raw(b.data(), sizeof b);
    }

    const std::string file;

private:
    gzFile f;
};

TraceWriter::TraceWriter(const std::string &path, const PlotOptions &options)
    : out(std::make_unique<Out>(path)), file(path), animate(options.animate) {
    out->raw(kMagic, sizeof kMagic);
    out->put<std::uint32_t>(kTraceVersion);

    // The settings the run would have drawn with, so a replay with none of its
    // own draws what the run would have. The directory is not recorded: where
    // the pictures go is the replay's to say.
    out->put<std::uint8_t>(kOptions);
    out->put<std::uint8_t>(options.animate ? 1 : 0);
    out->size(options.frameBudget);
    out->put<double>(options.frameZoom);
    out->size(options.gifByteCap);
    out->put<std::int32_t>(options.frameDelayCs);
    out->put<std::int32_t>(options.blendFrames);
    out->put<double>(options.finalZoom);
    out->size(options.finalHold);
    out->put<std::uint8_t>(options.writePpm ? 1 : 0);
}

TraceWriter::~TraceWriter() {
    // No end marker: a writer destroyed without close() is a run that stopped
    // early, and its trace has to say so.
    if (out != nullptr) {
        out->close();
    }
}

void TraceWriter::close() {
    if (out == nullptr) {
        return;
    }
    out->put<std::uint8_t>(kEnd);
    out->close();
    out.reset();
}

void TraceWriter::describe(const Graph &graph, const constraintMgr *fences) {
    if (!haveNetlist) {
        const std::size_t n = graph.getNumCells();
        std::vector<double> w(n), h(n), x(n), y(n);
        std::vector<std::uint8_t> flags(n);
        for (std::size_t v = 0; v < n; ++v) {
            const Vertex &cell = graph.getCell(v);
            w[v] = cell.width;
            h[v] = cell.height;
            x[v] = cell.x;
            y[v] = cell.y;
            flags[v] = static_cast<std::uint8_t>((cell.isFixed ? kFixed : 0) |
                                                 (cell.isTerminal ? kTerminal : 0));
        }
        out->put<std::uint8_t>(kNetlist);
        out->vec(w);
        out->vec(h);
        out->vec(x);
        out->vec(y);
        out->vec(flags);
        haveNetlist = true;
    }
    if (fences != nullptr && !haveFences) {
        out->put<std::uint8_t>(kFences);
        out->size(fences->numRegions());
        for (std::size_t r = 0; r < fences->numRegions(); ++r) {
            const Region &region = *fences->region(static_cast<int>(r));
            out->str(region.name);
            std::vector<double> corners;
            corners.reserve(region.rects.size() * 4);
            for (const Rect &rc : region.rects) {
                corners.insert(corners.end(), {rc.lo.x, rc.lo.y, rc.hi.x, rc.hi.y});
            }
            out->vec(corners);
        }
        haveFences = true;
    }
}

void TraceWriter::beginStage(StageVisual stage) {
    out->put<std::uint8_t>(kStage);
    out->str(stage.stage);
    out->str(stage.subdir);
    out->size(stage.cadence);
}

void TraceWriter::frame(const Graph &graph, const std::vector<float> &x,
                        const std::vector<float> &y, const std::array<double, 4> &die,
                        const constraintMgr *fences, const VisualFrame &what) {
    describe(graph, fences);
    out->put<std::uint8_t>(kFrame);
    out->put<std::uint8_t>(fences != nullptr ? 1 : 0);
    out->box(die);
    out->str(what.series);
    out->size(what.step);
    out->size(what.numSteps);
    out->str(what.note);
    out->put<double>(what.hpwl);
    out->put<double>(what.hpwlInitial);
    out->put<double>(what.overflow);
    out->put<std::uint8_t>(what.mandatory ? 1 : 0);
    out->vec(x);
    out->vec(y);
    ++frames;
}

void TraceWriter::densityMap(const DensityField &f) {
    out->put<std::uint8_t>(kDensity);
    out->str(f.series);
    out->size(f.step);
    out->str(f.note);
    out->put<double>(f.overflow);
    out->box(f.die);
    out->box({f.x0, f.y0, f.dx, f.dy});
    out->size(f.nbx);
    out->size(f.nby);
    out->vec(f.occ);
    out->vec(f.avail);
    std::vector<double> flat;
    flat.reserve(f.blockages.size() * 4);
    for (const std::array<double, 4> &b : f.blockages) {
        flat.insert(flat.end(), b.begin(), b.end());
    }
    out->vec(flat);
}

void TraceWriter::boundsCurve(const std::string &name, const std::string &title,
                              const std::vector<BoundsSample> &curve) {
    out->put<std::uint8_t>(kBounds);
    out->str(name);
    out->str(title);
    std::vector<double> flat;
    flat.reserve(curve.size() * 2);
    for (const BoundsSample &s : curve) {
        flat.push_back(s.upper);
        flat.push_back(s.lower);
    }
    out->vec(flat);
}

void TraceWriter::holdBack(std::size_t n) {
    out->put<std::uint8_t>(kHoldBack);
    out->size(n);
}

void TraceWriter::finalImage(const Graph &graph, const constraintMgr *fences, double hpwl) {
    describe(graph, fences);
    // The final still draws the committed placement, which by now is not the one
    // the netlist record was taken from.
    const std::size_t n = graph.getNumCells();
    std::vector<double> x(n), y(n);
    for (std::size_t v = 0; v < n; ++v) {
        x[v] = graph.getCell(v).x;
        y[v] = graph.getCell(v).y;
    }
    out->put<std::uint8_t>(kFinalImage);
    out->put<std::uint8_t>(fences != nullptr ? 1 : 0);
    out->put<double>(hpwl);
    out->vec(x);
    out->vec(y);
}

bool TraceWriter::finish(const Graph &graph, const std::vector<float> &x,
                         const std::vector<float> &y, const std::array<double, 4> &die,
                         const constraintMgr *fences, double hpwl) {
    describe(graph, fences);
    out->put<std::uint8_t>(kFinish);
    out->put<std::uint8_t>(fences != nullptr ? 1 : 0);
    out->box(die);
    out->put<double>(hpwl);
    out->vec(x);
    out->vec(y);
    return false;
}

// ---------------------------------------------------------------------------

namespace {
TraceReplay replayRecords(In &in, const std::string &outDir, Visualizer &viz,
                          const std::function<void(PlotOptions &)> &adjust, TraceReplay &result);
}  // namespace

TraceReplay replayTrace(const std::string &path, const std::string &outDir, Visualizer &viz,
                        const std::function<void(PlotOptions &)> &adjust) {
    ScopedTimer timer("trace-replay");
    In in(path);
    char magic[sizeof kMagic];
    in.raw(magic, sizeof magic);
    if (std::memcmp(magic, kMagic, sizeof kMagic) != 0) {
        ktlog.fatal("{} is not a ktplace trace", path);
    }
    const auto version = in.get<std::uint32_t>();
    if (version != kTraceVersion) {
        ktlog.fatal("trace {} is version {}; this build reads version {}", path, version,
                    kTraceVersion);
    }

    TraceReplay result;
    try {
        return replayRecords(in, outDir, viz, adjust, result);
    } catch (const Truncated &) {
        ktlog.warning(
            "trace {} stops part-way through a record: the run did not finish, and "
            "only what it recorded before that was drawn",
            path);
        return result;
    }
}

namespace {

TraceReplay replayRecords(In &in, const std::string &outDir, Visualizer &viz,
                          const std::function<void(PlotOptions &)> &adjust, TraceReplay &result) {
    const std::string &path = in.file;
    std::uint8_t t = 0;
    if (!in.tag(t) || t != kOptions) {
        ktlog.fatal("trace {} does not start with its picture settings", path);
    }
    PlotOptions &opts = result.options;
    opts.animate = in.get<std::uint8_t>() != 0;
    opts.frameBudget = static_cast<std::size_t>(in.get<std::uint64_t>());
    opts.frameZoom = in.get<double>();
    opts.gifByteCap = static_cast<std::size_t>(in.get<std::uint64_t>());
    opts.frameDelayCs = in.get<std::int32_t>();
    opts.blendFrames = in.get<std::int32_t>();
    opts.finalZoom = in.get<double>();
    opts.finalHold = static_cast<std::size_t>(in.get<std::uint64_t>());
    opts.writePpm = in.get<std::uint8_t>() != 0;
    if (adjust) {
        adjust(opts);
    }
    opts.dir = outDir;
    viz.configure(outDir, opts);

    Netlist net;
    while (in.tag(t)) {
        switch (t) {
            case kNetlist:
                readNetlist(in, net);
                break;
            case kFences:
                readFences(in, net);
                break;
            case kStage: {
                StageVisual stage;
                stage.stage = in.str();
                stage.subdir = in.str();
                stage.cadence = in.size();
                viz.beginStage(std::move(stage));
                break;
            }
            case kFrame: {
                requireGraph(in, net);
                const bool withFences = in.get<std::uint8_t>() != 0;
                const std::array<double, 4> die = in.box();
                VisualFrame what;
                what.series = in.str();
                what.step = in.size();
                what.numSteps = in.size();
                what.note = in.str();
                what.hpwl = in.get<double>();
                what.hpwlInitial = in.get<double>();
                what.overflow = in.get<double>();
                what.mandatory = in.get<std::uint8_t>() != 0;
                const std::vector<float> x = in.vec<float>();
                const std::vector<float> y = in.vec<float>();
                viz.frame(net.graph, x, y, die, net.fencesFor(withFences), what);
                ++result.frames;
                break;
            }
            case kDensity: {
                DensityField f;
                f.series = in.str();
                f.step = in.size();
                f.note = in.str();
                f.overflow = in.get<double>();
                f.die = in.box();
                const std::array<double, 4> grid = in.box();
                f.x0 = grid[0];
                f.y0 = grid[1];
                f.dx = grid[2];
                f.dy = grid[3];
                f.nbx = in.size();
                f.nby = in.size();
                f.occ = in.vec<double>();
                f.avail = in.vec<double>();
                if (f.occ.size() != f.nbx * f.nby || f.avail.size() != f.occ.size()) {
                    ktlog.fatal("trace {}: a density field does not match its grid", path);
                }
                const std::vector<double> flat = in.vec<double>();
                for (std::size_t i = 0; i + 3 < flat.size(); i += 4) {
                    f.blockages.push_back({flat[i], flat[i + 1], flat[i + 2], flat[i + 3]});
                }
                viz.densityMap(f);
                break;
            }
            case kBounds: {
                const std::string name = in.str();
                const std::string title = in.str();
                const std::vector<double> flat = in.vec<double>();
                std::vector<BoundsSample> curve;
                curve.reserve(flat.size() / 2);
                for (std::size_t i = 0; i + 1 < flat.size(); i += 2) {
                    curve.push_back({flat[i], flat[i + 1]});
                }
                viz.boundsCurve(name, title, curve);
                break;
            }
            case kHoldBack:
                viz.holdBack(in.size());
                break;
            case kFinalImage: {
                requireGraph(in, net);
                const bool withFences = in.get<std::uint8_t>() != 0;
                const double hpwl = in.get<double>();
                const std::vector<double> x = in.vec<double>();
                const std::vector<double> y = in.vec<double>();
                if (x.size() != net.graph.getNumCells() || y.size() != x.size()) {
                    ktlog.fatal("trace {}: the final placement does not match the netlist", path);
                }
                for (std::size_t v = 0; v < x.size(); ++v) {
                    net.graph.getCell(v).x = x[v];
                    net.graph.getCell(v).y = y[v];
                }
                viz.finalImage(net.graph, net.fencesFor(withFences), hpwl);
                break;
            }
            case kFinish: {
                requireGraph(in, net);
                const bool withFences = in.get<std::uint8_t>() != 0;
                const std::array<double, 4> die = in.box();
                const double hpwl = in.get<double>();
                const std::vector<float> x = in.vec<float>();
                const std::vector<float> y = in.vec<float>();
                result.animationWritten =
                    viz.finish(net.graph, x, y, die, net.fencesFor(withFences), hpwl);
                break;
            }
            case kEnd:
                result.complete = true;
                return result;
            default:
                ktlog.fatal("trace {}: unknown record '{}'", path, static_cast<char>(t));
        }
    }
    ktlog.warning(
        "trace {} has no end marker: the run did not finish, and only what it "
        "recorded was drawn",
        path);
    return result;
}

}  // namespace

}  // namespace ktplace
