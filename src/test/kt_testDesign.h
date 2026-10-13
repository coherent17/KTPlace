// @file kt_testDesign.h
// Shared helpers for the suites that run whole stages on a synthetic design:
// a scratch directory, a tiny Bookshelf design written into it, and a check
// for the ktlog.fatal paths.
//
// Header-only, and included after the suite's BOOST_TEST_MODULE definition,
// because the helpers use Boost.Test assertions.

#pragma once

#include "util/kt_log.h"

#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace ktplace::testing {

class ScratchDir {
public:
    explicit ScratchDir(const std::string &tag) {
        static int counter = 0;
        path =
            std::filesystem::temp_directory_path() /
            ("ktplace_" + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(++counter));
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;

    [[nodiscard]] std::string str() const {
        return path.string();
    }
    [[nodiscard]] std::filesystem::path file(const std::string &name) const {
        return path / name;
    }

    void write(const std::string &name, const std::string &content) const {
        std::filesystem::create_directories(file(name).parent_path());
        std::ofstream out(file(name));
        BOOST_REQUIRE(out.is_open());
        out << content;
    }

private:
    std::filesystem::path path;
};

/// Where writeBookshelfDesign() puts the design. It lives in its own directory,
/// because that directory's name is what the reader takes the design name from.
inline std::filesystem::path designDir(const ScratchDir &dir) {
    return dir.file("tiny");
}

/// Four movable cells, a pad on each side, and two nets, so the solver has
/// real work to do on a die that is large enough to spread into.
inline void writeBookshelfDesign(const ScratchDir &dir) {
    std::string nodes = "UCLA nodes 1.0\n\nNumNodes : 6\nNumTerminals : 2\n\n";
    for (int i = 0; i < 4; ++i) {
        nodes += "\tc" + std::to_string(i) + "\t1.0\t2.0\n";
    }
    nodes += "\tpadA\t1.0\t2.0\tterminal\n";
    nodes += "\tpadB\t1.0\t2.0\tterminal\n";
    dir.write("tiny/tiny.nodes", nodes);

    std::string nets = "UCLA nets 1.0\n\nNumNets : 2\nNumPins : 8\n\n";
    nets += "NetDegree : 4\tn0\n";
    nets += "\tpadA I\t: 0.0\t0.0\n";
    for (int i = 0; i < 3; ++i) {
        nets += "\tc" + std::to_string(i) + " O\t: 0.0\t0.0\n";
    }
    nets += "NetDegree : 4\tn1\n";
    nets += "\tc3 I\t: 0.0\t0.0\n";
    nets += "\tpadB O\t: 0.0\t0.0\n";
    nets += "\tc0 I\t: 0.0\t0.0\n";
    nets += "\tc1 O\t: 0.0\t0.0\n";
    dir.write("tiny/tiny.nets", nets);

    std::string pl = "UCLA pl 1.0\n\n";
    for (int i = 0; i < 4; ++i) {
        pl += "c" + std::to_string(i) + "\t" + std::to_string(i * 2) + "\t0 : N\n";
    }
    pl += "padA\t0\t0 : N /FIXED\n";
    pl += "padB\t8\t0 : N /FIXED\n";
    dir.write("tiny/tiny.pl", pl);

    dir.write("tiny/tiny.scl", R"(UCLA scl 1.0

NumRows : 1

CoreRow Horizontal
  Coordinate   :  0
  Height       :  2
  Sitewidth    :  1
  Sitespacing  :  1
  SubrowOrigin :  0 NumSites :  20
End
)");
}

/// Require that `fn` raises a FatalError whose message contains `needle`, and
/// that the same text reached stderr as a fatal record.
inline void expectFatal(const std::function<void()> &fn, const std::string &needle) {
    std::ostringstream captured;
    std::streambuf *original = std::cerr.rdbuf(captured.rdbuf());
    std::string message;
    bool raised = false;
    try {
        fn();
    } catch (const FatalError &e) {
        raised = true;
        message = e.message();
    } catch (...) {
        std::cerr.rdbuf(original);
        throw;
    }
    std::cerr.rdbuf(original);

    BOOST_CHECK_MESSAGE(raised, "expected a FatalError containing \"" << needle << "\"");
    BOOST_CHECK_MESSAGE(message.find(needle) != std::string::npos,
                        "FatalError did not contain \"" << needle << "\"; got: " << message);
    const std::string console = captured.str();
    BOOST_CHECK_MESSAGE(
        console.find("[fatal]") != std::string::npos && console.find(needle) != std::string::npos,
        "stderr did not carry the fatal record; got: " << console);
}

inline std::size_t countNonEmptyLines(const std::filesystem::path &file) {
    std::ifstream in(file);
    std::size_t count = 0;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) {
            ++count;
        }
    }
    return count;
}

}  // namespace ktplace::testing
