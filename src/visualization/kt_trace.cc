// @file kt_trace.cc
// The trace file: writing it during a run, replaying it into a Visualizer

#include "visualization/kt_trace.h"

#include "util/kt_log.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <zlib.h>

namespace ktplace {

namespace {

constexpr char kMagic[8] = {'K', 'T', 'T', 'R', 'A', 'C', 'E', '\0'};

enum Tag : std::uint8_t {
    kStage = 'S',
    kFrame = 'F',
    kBounds = 'B',
    kHoldBack = 'H',
};

// gzwrite and gzread take an unsigned count, so a big array goes in pieces.
constexpr std::size_t kChunk = std::size_t{1} << 30;

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
                ktlog.fatal("trace {} stops part-way through a record", file);
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

    std::size_t size() {
        const auto n = get<std::uint64_t>();
        if (n > (std::uint64_t{1} << 34)) {
            ktlog.fatal("trace {} holds an implausible count ({})", file, n);
        }
        return static_cast<std::size_t>(n);
    }

    std::string str() {
        std::string s(get<std::uint32_t>(), '\0');
        raw(s.data(), s.size());
        return s;
    }

    template <class T>
    std::vector<T> vec() {
        std::vector<T> v(size());
        raw(v.data(), v.size() * sizeof(T));
        return v;
    }

    const std::string file;

private:
    gzFile f;
};

}  // namespace

class TraceWriter::Out {
public:
    // Level 1: the solver waits on this, and coordinates gain little from more.
    explicit Out(const std::string &path) : file(path), f(gzopen(path.c_str(), "wb1")) {
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
        if (f != nullptr && gzclose(f) != Z_OK) {
            ktlog.fatal("cannot finish writing trace {}", file);
        }
        f = nullptr;
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

    void str(std::string_view s) {
        put<std::uint32_t>(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
    }

    template <class T>
    void vec(std::span<const T> v) {
        size(v.size());
        raw(v.data(), v.size_bytes());
    }

private:
    const std::string file;
    gzFile f;
};

TraceWriter::TraceWriter(const std::string &path) : out(std::make_unique<Out>(path)), file(path) {
    out->raw(kMagic, sizeof kMagic);
    out->put<std::uint32_t>(kTraceVersion);
}

TraceWriter::~TraceWriter() = default;

void TraceWriter::close() {
    out.reset();
}

void TraceWriter::beginStage(const StageVisual &stage) {
    out->put<std::uint8_t>(kStage);
    out->str(stage.stage);
    out->str(stage.subdir);
    out->size(stage.cadence);
}

void TraceWriter::frame(const std::vector<float> &x, const std::vector<float> &y,
                        const std::array<double, 4> &die, bool withFences,
                        const VisualFrame &what) {
    out->put<std::uint8_t>(kFrame);
    out->put<std::uint8_t>(withFences ? 1 : 0);
    out->vec<double>(die);
    out->str(what.series);
    out->size(what.step);
    out->size(what.numSteps);
    out->str(what.note);
    out->put<double>(what.hpwl);
    out->put<double>(what.hpwlInitial);
    out->put<double>(what.overflow);
    out->put<std::uint8_t>(what.mandatory ? 1 : 0);
    out->vec<float>(x);
    out->vec<float>(y);
    ++frames;
}

void TraceWriter::boundsCurve(std::string_view name, std::string_view title,
                              std::span<const BoundsSample> curve) {
    static_assert(sizeof(BoundsSample) == 2 * sizeof(double));
    out->put<std::uint8_t>(kBounds);
    out->str(name);
    out->str(title);
    out->vec(curve);
}

void TraceWriter::holdBack(std::size_t n) {
    out->put<std::uint8_t>(kHoldBack);
    out->size(n);
}

std::size_t replayTrace(const std::string &path, Visualizer &viz, const Graph &graph,
                        const constraintMgr *fences) {
    In in(path);
    char magic[sizeof kMagic];
    in.raw(magic, sizeof magic);
    if (std::memcmp(magic, kMagic, sizeof kMagic) != 0) {
        ktlog.fatal("{} is not a ktplace trace", path);
    }
    if (const auto version = in.get<std::uint32_t>(); version != kTraceVersion) {
        ktlog.fatal("trace {} is version {}; this build reads version {}", path, version,
                    kTraceVersion);
    }

    std::size_t frames = 0;
    std::uint8_t t = 0;
    while (in.tag(t)) {
        switch (t) {
            case kStage: {
                StageVisual stage;
                stage.stage = in.str();
                stage.subdir = in.str();
                stage.cadence = in.size();
                viz.beginStage(std::move(stage));
                break;
            }
            case kFrame: {
                const bool withFences = in.get<std::uint8_t>() != 0;
                const std::vector<double> die = in.vec<double>();
                if (die.size() != 4) {
                    ktlog.fatal("trace {}: a frame's die is not a box", path);
                }
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
                viz.frame(graph, x, y, {die[0], die[1], die[2], die[3]},
                          withFences ? fences : nullptr, what);
                ++frames;
                break;
            }
            case kBounds: {
                const std::string name = in.str();
                const std::string title = in.str();
                viz.boundsCurve(name, title, in.vec<BoundsSample>());
                break;
            }
            case kHoldBack:
                viz.holdBack(in.size());
                break;
            default:
                ktlog.fatal("trace {}: unknown record '{}'", path, static_cast<char>(t));
        }
    }
    return frames;
}

}  // namespace ktplace
