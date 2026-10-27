// SPDX-License-Identifier: BSD-3-Clause

#define BOOST_TEST_MODULE ktplace_abcdplace
#define BOOST_TEST_DYN_LINK

#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "detailPlacer/abcdplace/abcdplace_dm.h"
#include "detailPlacer/abcdplace/abcdplace_flowMgr.h"
#include "detailPlacer/abcdplace/abcdplace_placer.h"

#include <algorithm>
#include <boost/test/unit_test.hpp>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

using namespace ktplace;

namespace {

constexpr double rowHeight = 10.0;
constexpr double rowWidth = 200.0;

void addRows(ktDM &db, int count) {
    for (int r = 0; r < count; ++r) {
        const std::size_t row = db.addRow(r * rowHeight, rowHeight, 1.0, 1.0);
        (void)db.addSubrow(row, 0.0, rowWidth);
    }
}

void addCell(ktDM &db, const std::string &name, double width, Point at) {
    const std::size_t id = db.addCell(name, width, rowHeight);
    db.setCellPosition(id, at);
}

void addPad(ktDM &db, const std::string &name, Point at) {
    const std::size_t id = db.addCell(name, 1.0, 1.0, true);
    db.setCellPosition(id, at);
    db.setCellFixed(id, true);
}

void connect(ktDM &db, const std::string &net, const std::vector<std::string> &cells) {
    (void)db.addNet(net);
    for (const std::string &cell : cells) {
        (void)db.addPin(cell, net, Point(0.5, 0.5), false);
    }
}

void makeScrambledDesign(ktDM &db) {
    addRows(db, 8);
    std::uint64_t state = 12345;
    const auto next = [&state] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::size_t>(state >> 33U);
    };
    int id = 0;
    for (int r = 0; r < 8; ++r) {
        double x = 0.0;
        while (x + 4.0 <= rowWidth - 20.0) {
            const double width = static_cast<double>(1 + next() % 3);
            addCell(db, "c" + std::to_string(id++), width, Point(x, r * rowHeight));
            x += width + static_cast<double>(next() % 3);
        }
    }
    for (int p = 0; p < 6; ++p) {
        addPad(db, "pad" + std::to_string(p), Point(p * 39.0, -1.0));
    }
    for (int n = 0; n < id; ++n) {
        std::vector<std::string> cells{"c" + std::to_string(n), "c" + std::to_string(next() % id)};
        if (n % 3 == 0) {
            cells.push_back("c" + std::to_string(next() % id));
        }
        if (n % 17 == 0) {
            cells.push_back("pad" + std::to_string(next() % 6));
        }
        connect(db, "n" + std::to_string(n), cells);
    }
}

void place(ktDM &db) {
    abcdplace::flowMgr flow(db);
    flow.run();
    db.setAbcdplaceSolution(flow.getSolution());
}

std::vector<Point> getPositions(const ktDM &db) {
    std::vector<Point> out;
    for (std::size_t c = 0; c < db.getNumCells(); ++c) {
        out.push_back(db.getCellPosition(c));
    }
    return out;
}

std::size_t getOverlappingPairs(const ktDM &db) {
    std::size_t pairs = 0;
    const auto movable = [&db](std::size_t c) {
        return !db.getGraph().getCell(c).isFixed;
    };
    for (std::size_t a = 0; a < db.getNumCells(); ++a) {
        for (std::size_t b = a + 1; b < db.getNumCells(); ++b) {
            if (!movable(a) || !movable(b)) {
                continue;
            }
            const Point pa = db.getCellPosition(a);
            const Point pb = db.getCellPosition(b);
            const double wa = db.getGraph().getCell(a).width;
            const double wb = db.getGraph().getCell(b).width;
            const bool apart = pa.x + wa <= pb.x + 1e-9 || pb.x + wb <= pa.x + 1e-9;
            pairs += !apart && std::abs(pa.y - pb.y) < rowHeight - 1e-9 ? 1 : 0;
        }
    }
    return pairs;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ktplace_abcdplace)

BOOST_AUTO_TEST_CASE(the_assignment_is_optimal) {
    std::uint64_t state = 7;
    for (int trial = 0; trial < 20; ++trial) {
        const std::size_t n = 5;
        std::vector<double> cost(n * n);
        for (double &c : cost) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            c = static_cast<double>(state >> 40U) / 1000.0;
        }
        const std::vector<std::size_t> found = abcdplace::solveAssignment(cost, n);
        const auto total = [&](const std::vector<std::size_t> &a) {
            double sum = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                sum += cost[i * n + a[i]];
            }
            return sum;
        };
        std::vector<std::size_t> perm(n);
        std::iota(perm.begin(), perm.end(), std::size_t{0});
        double best = total(perm);
        while (std::next_permutation(perm.begin(), perm.end())) {
            best = std::min(best, total(perm));
        }
        std::vector<std::size_t> sorted = found;
        std::ranges::sort(sorted);
        BOOST_TEST((std::ranges::adjacent_find(sorted) == sorted.end()));
        BOOST_TEST(total(found) == best, boost::test_tools::tolerance(1e-9));
    }
}

BOOST_AUTO_TEST_CASE(two_far_cells_trade_places) {
    ktDM db;
    addRows(db, 1);
    addCell(db, "a", 2.0, Point(10.0, 0.0));
    addCell(db, "b", 2.0, Point(150.0, 0.0));
    addPad(db, "nearB", Point(152.0, 5.0));
    addPad(db, "nearA", Point(12.0, 5.0));
    connect(db, "na", {"a", "nearB"});
    connect(db, "nb", {"b", "nearA"});
    const double before = db.getHpwl();
    place(db);
    BOOST_TEST(db.getCellPosition(db.getCellId("a")).x > 100.0);
    BOOST_TEST(db.getCellPosition(db.getCellId("b")).x < 50.0);
    BOOST_TEST(db.getHpwl() < before);
    BOOST_TEST(db.getDefects().empty());
}

BOOST_AUTO_TEST_CASE(a_reversed_run_is_reordered) {
    ktDM db;
    addRows(db, 1);
    addCell(db, "left", 2.0, Point(50.0, 0.0));
    addCell(db, "middle", 2.0, Point(52.0, 0.0));
    addCell(db, "right", 2.0, Point(54.0, 0.0));
    addPad(db, "east", Point(190.0, 5.0));
    addPad(db, "west", Point(5.0, 5.0));
    connect(db, "toEast", {"left", "east"});
    connect(db, "toWest", {"right", "west"});
    const double before = db.getHpwl();
    place(db);
    BOOST_TEST(db.getHpwl() < before);
    BOOST_TEST(db.getCellPosition(db.getCellId("right")).x <
               db.getCellPosition(db.getCellId("left")).x);
    BOOST_TEST(db.getDefects().empty());
}

BOOST_AUTO_TEST_CASE(a_scrambled_design_stays_legal_and_gets_shorter) {
    ktDM db;
    makeScrambledDesign(db);
    const double before = db.getHpwl();
    place(db);
    BOOST_TEST(db.getHpwl() < before);
    BOOST_TEST(getOverlappingPairs(db) == 0u);
    BOOST_TEST(db.getDefects().empty());
    for (std::size_t c = 0; c < db.getNumCells(); ++c) {
        const Point p = db.getCellPosition(c);
        if (db.getGraph().getCell(c).isFixed) {
            continue;
        }
        BOOST_TEST(std::abs(p.x - std::round(p.x)) < 1e-9);
        BOOST_TEST(std::abs(p.y / rowHeight - std::round(p.y / rowHeight)) < 1e-9);
    }
}

BOOST_AUTO_TEST_CASE(placement_is_deterministic) {
    ktDM first;
    makeScrambledDesign(first);
    place(first);
    ktDM second;
    makeScrambledDesign(second);
    place(second);
    BOOST_TEST(getPositions(first) == getPositions(second));
}

BOOST_AUTO_TEST_CASE(every_step_offers_a_frame) {
    ktDM db;
    makeScrambledDesign(db);
    abcdplace::abcdplaceDM dm = abcdplace::buildAbcdplaceDM(db);
    std::size_t frames = 0;
    dm.setFrameHook([&frames](const std::vector<float> &, const std::vector<float> &,
                              const abcdplace::FrameInfo &) {
        ++frames;
    });
    dm.run();
    BOOST_TEST(frames == 5u);
}

BOOST_AUTO_TEST_SUITE_END()
