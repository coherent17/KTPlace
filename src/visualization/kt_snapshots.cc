// @file kt_snapshots.cc
// The snapshot file: writing it during a run, replaying it into a Visualizer

#include "visualization/kt_snapshots.h"

#include "util/kt_log.h"

#include <algorithm>
#include <type_traits>
#include <zlib.h>

namespace ktplace {

namespace {

enum class Tag : std::uint8_t {
    Stage = 'S',
    Frame = 'F',
    Bounds = 'B',
    HoldBack = 'H',
};

// gzwrite and gzread take an unsigned count, so a big array goes in pieces.
constexpr std::size_t chunkSize = std::size_t{1} << 30;

class In {
public:
    explicit In(const std::string &path) : file(path), f(gzopen(path.c_str(), "rb")) {
        if (f == nullptr) {
            ktlog.fatal("cannot open snapshot file {}", path);
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

    bool readTag(std::uint8_t &t) {
        const int c = gzgetc(f);
        if (c < 0) {
            return false;
        }
        t = static_cast<std::uint8_t>(c);
        return true;
    }

    void read(void *p, std::size_t n) {
        auto *bytes = static_cast<unsigned char *>(p);
        while (n > 0) {
            const auto want = static_cast<unsigned>(std::min(n, chunkSize));
            const int got = gzread(f, bytes, want);
            if (got <= 0) {
                ktlog.fatal("snapshot file {} stops part-way through a record", file);
            }
            bytes += got;
            n -= static_cast<std::size_t>(got);
        }
    }

    template <class T>
    T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v;
        read(&v, sizeof v);
        return v;
    }

    std::size_t getSize() {
        return static_cast<std::size_t>(get<std::uint64_t>());
    }

    std::string getString() {
        std::string s(get<std::uint32_t>(), '\0');
        read(s.data(), s.size());
        return s;
    }

    template <class T>
    std::vector<T> getVec() {
        std::vector<T> v(getSize());
        read(v.data(), v.size() * sizeof(T));
        return v;
    }

    const std::string file;

private:
    gzFile f;
};

}  // namespace

class SnapshotWriter::Out {
public:
    // Level 1: the solver waits on this, and coordinates gain little from more.
    explicit Out(const std::string &path) : file(path), f(gzopen(path.c_str(), "wb1")) {
        if (f == nullptr) {
            ktlog.fatal("cannot open snapshot file {} for writing", path);
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
            ktlog.fatal("cannot finish writing snapshot file {}", file);
        }
        f = nullptr;
    }

    void write(const void *p, std::size_t n) {
        const auto *bytes = static_cast<const unsigned char *>(p);
        while (n > 0) {
            const auto len = static_cast<unsigned>(std::min(n, chunkSize));
            if (gzwrite(f, bytes, len) != static_cast<int>(len)) {
                ktlog.fatal("cannot write snapshot file {}", file);
            }
            bytes += len;
            n -= len;
        }
    }

    template <class T>
    void put(T v) {
        static_assert(std::is_trivially_copyable_v<T>);
        write(&v, sizeof v);
    }

    void putSize(std::size_t n) {
        put<std::uint64_t>(n);
    }

    void putString(std::string_view s) {
        put<std::uint32_t>(static_cast<std::uint32_t>(s.size()));
        write(s.data(), s.size());
    }

    template <class T>
    void putVec(std::span<const T> v) {
        putSize(v.size());
        write(v.data(), v.size_bytes());
    }

private:
    const std::string file;
    gzFile f;
};

SnapshotWriter::SnapshotWriter(const std::string &path)
    : out(std::make_unique<Out>(path)), file(path) {}

SnapshotWriter::~SnapshotWriter() = default;

void SnapshotWriter::close() {
    out.reset();
}

void SnapshotWriter::beginStage(const StageVisual &stage) {
    out->put(Tag::Stage);
    out->putString(stage.stage);
    out->putString(stage.subdir);
    out->putSize(stage.cadence);
}

void SnapshotWriter::frame(const std::vector<float> &x, const std::vector<float> &y,
                           const std::array<double, 4> &die, bool withFences,
                           const VisualFrame &what) {
    out->put(Tag::Frame);
    out->put<std::uint8_t>(withFences ? 1 : 0);
    out->putVec<double>(die);
    out->putString(what.series);
    out->putSize(what.step);
    out->putSize(what.numSteps);
    out->putString(what.note);
    out->put<double>(what.hpwl);
    out->put<double>(what.hpwlInitial);
    out->put<double>(what.overflow);
    out->put<std::uint8_t>(what.mandatory ? 1 : 0);
    out->putVec<float>(x);
    out->putVec<float>(y);
    ++frames;
}

void SnapshotWriter::boundsCurve(std::string_view name, std::string_view title,
                                 std::span<const BoundsSample> curve) {
    static_assert(sizeof(BoundsSample) == 2 * sizeof(double));
    out->put(Tag::Bounds);
    out->putString(name);
    out->putString(title);
    out->putVec(curve);
}

void SnapshotWriter::holdBack(std::size_t n) {
    out->put(Tag::HoldBack);
    out->putSize(n);
}

std::size_t replaySnapshots(const std::string &path, Visualizer &viz, const Graph &graph,
                            const constraintMgr *fences) {
    In in(path);
    std::size_t frames = 0;
    std::uint8_t t = 0;
    while (in.readTag(t)) {
        switch (static_cast<Tag>(t)) {
            case Tag::Stage: {
                StageVisual stage;
                stage.stage = in.getString();
                stage.subdir = in.getString();
                stage.cadence = in.getSize();
                viz.beginStage(std::move(stage));
                break;
            }
            case Tag::Frame: {
                const bool withFences = in.get<std::uint8_t>() != 0;
                const std::vector<double> die = in.getVec<double>();
                if (die.size() != 4) {
                    ktlog.fatal("snapshot file {}: a frame's die is not a box", path);
                }
                VisualFrame what;
                what.series = in.getString();
                what.step = in.getSize();
                what.numSteps = in.getSize();
                what.note = in.getString();
                what.hpwl = in.get<double>();
                what.hpwlInitial = in.get<double>();
                what.overflow = in.get<double>();
                what.mandatory = in.get<std::uint8_t>() != 0;
                const std::vector<float> x = in.getVec<float>();
                const std::vector<float> y = in.getVec<float>();
                viz.frame(graph, x, y, {die[0], die[1], die[2], die[3]},
                          withFences ? fences : nullptr, what);
                ++frames;
                break;
            }
            case Tag::Bounds: {
                const std::string name = in.getString();
                const std::string title = in.getString();
                viz.boundsCurve(name, title, in.getVec<BoundsSample>());
                break;
            }
            case Tag::HoldBack:
                viz.holdBack(in.getSize());
                break;
            default:
                ktlog.fatal("snapshot file {}: unknown record '{}'", path, static_cast<char>(t));
        }
    }
    return frames;
}

}  // namespace ktplace
