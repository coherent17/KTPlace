// SPDX-License-Identifier: BSD-3-Clause
// @file test_ntuplace1.cc
// Unit tests for the ratio-partitioning global placer.
//
// Global placement may overlap -- legalization is a later stage -- so these do
// not assert legality. They assert the three things the partitioner is for, each
// of which a previous version silently did not do: cells stay off fixed macros
// (whitespace distribution sees obstacles, terminals included), cells are pulled
// towards the pins they connect to (terminal propagation), and every cell lands
// inside the rows rather than in the pad ring around them.

#define BOOST_TEST_MODULE ktplace_ntuplace1

#include "constraint/kt_constraintMgr.h"
#include "placer/ntuplace1/kt_ntuplace1.h"
#include "util/kt_log.h"

#include <boost/test/included/unit_test.hpp>
#include <string>
#include <vector>

using namespace ktplace;

namespace {

constexpr double kRowHeight = 10.0;
constexpr int kRows = 10;
constexpr double kWidth = 100.0;

/// kRows rows of kWidth unit sites, stacked from y = 0.
void addRows(ktDM &db) {
    for (int r = 0; r < kRows; ++r) {
        const std::size_t row = db.addRow(r * kRowHeight, kRowHeight, 1.0, 1.0);
        (void)db.addSubrow(row, 0.0, kWidth);
    }
}

/// A fixed block. Bookshelf marks fixed macros and pads as terminals, which is
/// exactly the case the placer once ignored, so these are terminals too.
std::size_t addFixed(ktDM &db, const std::string &name, double x, double y, double w, double h) {
    const std::size_t id = db.addCell(name, w, h, /*isTerminal=*/true);
    db.setCellPosition(id, x, y);
    db.setCellFixed(id, true);
    return id;
}

std::vector<std::size_t> addCells(ktDM &db, const std::string &prefix, int n) {
    std::vector<std::size_t> ids;
    for (int i = 0; i < n; ++i) {
        ids.push_back(db.addCell(prefix + std::to_string(i), 1.0, kRowHeight));
    }
    return ids;
}

void addNet(ktDM &db, const std::string &net, const std::vector<std::string> &cells) {
    (void)db.addNet(net);
    for (const std::string &c : cells) {
        (void)db.addPin(c, net, 0.0, 0.0, true);
    }
}

/// Every cell (1 wide, one row tall) lies inside the rows.
void expectInRows(ktDM &db, const std::vector<std::size_t> &ids) {
    for (const std::size_t id : ids) {
        const auto [x, y] = db.getCellPosition(id);
        BOOST_TEST(x >= -1e-9);
        BOOST_TEST(x + 1.0 <= kWidth + 1e-9);
        BOOST_TEST(y >= -1e-9);
        BOOST_TEST(y + kRowHeight <= kRows * kRowHeight + 1e-9);
    }
}

/// Small leaves, so the recursion goes deep enough on a toy design to show
/// where it sends things.
RatioPlaceParams smallLeaves() {
    RatioPlaceParams p;
    p.targetLeafCells = 4;
    return p;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ktplace_ntuplace1)

BOOST_AUTO_TEST_CASE(cells_avoid_a_macro_marked_as_terminal) {
    ktDM db;
    addRows(db);
    // A macro over the whole left half of the rows: the only room is on the right.
    addFixed(db, "macro", 0.0, 0.0, 50.0, kRows * kRowHeight);
    const auto ids = addCells(db, "c", 60);
    for (int i = 0; i + 1 < 60; ++i) {
        addNet(db, "n" + std::to_string(i), {"c" + std::to_string(i), "c" + std::to_string(i + 1)});
    }

    RatioPlacer placer(db);
    const RatioPlaceResult res = placer.place(smallLeaves());

    BOOST_TEST(res.numFixed == 1u);
    for (const std::size_t id : ids) {
        const auto [x, y] = db.getCellPosition(id);
        (void)y;
        BOOST_TEST(x >= 50.0 - 1e-9, "cell " << id << " at x=" << x << " is on the macro");
    }
    expectInRows(db, ids);
}

BOOST_AUTO_TEST_CASE(cells_follow_their_pads) {
    ktDM db;
    addRows(db);
    // Pads outside the rows, left and right, as in a Bookshelf pad ring.
    addFixed(db, "padL", -20.0, 45.0, 1.0, 1.0);
    addFixed(db, "padR", kWidth + 20.0, 45.0, 1.0, 1.0);
    // Created alternately (a0, b0, a1, b1, ...), so a split by netlist order
    // mixes the groups and only the pads can tell them apart.
    std::vector<std::size_t> a, b;
    for (int i = 0; i < 16; ++i) {
        a.push_back(db.addCell("a" + std::to_string(i), 1.0, kRowHeight));
        b.push_back(db.addCell("b" + std::to_string(i), 1.0, kRowHeight));
    }
    // Each group is a chain, and each group's ends are tied to its pad.
    for (int i = 0; i + 1 < 16; ++i) {
        addNet(db, "na" + std::to_string(i),
               {"a" + std::to_string(i), "a" + std::to_string(i + 1)});
        addNet(db, "nb" + std::to_string(i),
               {"b" + std::to_string(i), "b" + std::to_string(i + 1)});
    }
    for (int i = 0; i < 16; i += 3) {
        addNet(db, "pa" + std::to_string(i), {"padL", "a" + std::to_string(i)});
        addNet(db, "pb" + std::to_string(i), {"padR", "b" + std::to_string(i)});
    }

    RatioPlacer placer(db);
    (void)placer.place(smallLeaves());

    double meanA = 0.0, meanB = 0.0;
    for (const std::size_t id : a) {
        meanA += db.getCellPosition(id).first / 16.0;
    }
    for (const std::size_t id : b) {
        meanB += db.getCellPosition(id).first / 16.0;
    }
    BOOST_TEST(meanA < 35.0);
    BOOST_TEST(meanB > 65.0);
    expectInRows(db, a);
    expectInRows(db, b);
}

BOOST_AUTO_TEST_CASE(split_follows_free_area_and_stays_in_rows) {
    ktDM db;
    addRows(db);
    // A macro over the left quarter: the first cut (vertical, at x = 50) has
    // 25 units of free width on the left and 50 on the right, so a third of the
    // cells should land left of it and two thirds right.
    addFixed(db, "macro", 0.0, 0.0, 25.0, kRows * kRowHeight);
    const auto ids = addCells(db, "c", 300);
    // A pad far outside the rows: it must not drag the placement area with it.
    addFixed(db, "pad", 400.0, 400.0, 1.0, 1.0);

    RatioPlacer placer(db);
    RatioPlaceParams p;
    p.targetLeafCells = 16;
    (void)placer.place(p);

    std::size_t left = 0;
    for (const std::size_t id : ids) {
        const auto [x, y] = db.getCellPosition(id);
        BOOST_TEST(x >= 25.0 - 1e-9);
        BOOST_TEST(x + 1.0 <= kWidth + 1e-9);
        BOOST_TEST(y >= 0.0);
        BOOST_TEST(y + kRowHeight <= kRows * kRowHeight + 1e-9);
        if (x < 50.0) {
            ++left;
        }
    }
    const double share = static_cast<double>(left) / static_cast<double>(ids.size());
    BOOST_TEST(share > 1.0 / 3.0 - 0.06);
    BOOST_TEST(share < 1.0 / 3.0 + 0.06);
}

BOOST_AUTO_TEST_CASE(tiny_designs_do_not_fail) {
    ktDM db;
    addRows(db);
    const auto ids = addCells(db, "c", 3);
    addNet(db, "n", {"c0", "c1", "c2"});
    RatioPlacer placer(db);
    RatioPlaceParams p;
    p.targetLeafCells = 1;  // forces cuts down to single cells
    const RatioPlaceResult res = placer.place(p);
    BOOST_TEST(res.numMovable == 3u);
    expectInRows(db, ids);
}

// Fences (ISPD 2015) are exclusive: members inside their region, every other
// cell outside. Members here are wired to pads far from the fence and other
// cells to a pad inside it, so wirelength alone would put both on the wrong
// side.
BOOST_AUTO_TEST_CASE(fenced_cells_are_placed_in_their_region_and_others_kept_out) {
    ktDM db;
    addRows(db);
    // Fence over x 10..40, rows 2..7 (y 20..80).
    constexpr double fx0 = 10.0, fy0 = 20.0, fx1 = 40.0, fy1 = 80.0;
    addFixed(db, "padR", kWidth + 20.0, 50.0, 1.0, 1.0);
    addFixed(db, "padIn", 25.0, 50.0, 1.0, 1.0);
    std::vector<std::size_t> members, others;
    for (int i = 0; i < 24; ++i) {
        members.push_back(db.addCell("m" + std::to_string(i), 1.0, kRowHeight));
        others.push_back(db.addCell("o" + std::to_string(i), 1.0, kRowHeight));
    }
    for (int i = 0; i < 24; ++i) {
        addNet(db, "nm" + std::to_string(i), {"m" + std::to_string(i), "padR"});
        addNet(db, "no" + std::to_string(i), {"o" + std::to_string(i), "padIn"});
    }
    {
        constraintMgr mgr;
        const int id = mgr.addRegion("f0", {Point{fx0, fy0}, Point{fx1, fy1}});
        (void)mgr.assignByPrefix(id, "m", db.getGraph());
        db.setConstraints(std::move(mgr));
    }

    RatioPlacer placer(db);
    const RatioPlaceResult res = placer.place(smallLeaves());

    BOOST_TEST(res.fenceRegions == 1u);
    BOOST_TEST(res.fenceViolations == 0u);
    // The engine's own containment test is on the lower-left corner, and that is
    // what global placement promises; whole cells are the legalizer's job.
    const auto inside = [&](std::size_t id) {
        const auto [x, y] = db.getCellPosition(id);
        return x >= fx0 && x <= fx1 && y >= fy0 && y <= fy1;
    };
    for (const std::size_t id : members) {
        BOOST_TEST(inside(id), "member " << id << " outside its fence");
    }
    for (const std::size_t id : others) {
        BOOST_TEST(!inside(id), "non-member " << id << " inside the fence");
    }
}

BOOST_AUTO_TEST_CASE(frame_hook_sees_every_cut_and_does_not_change_the_placement) {
    const auto build = [](ktDM &db) {
        addRows(db);
        (void)addCells(db, "c", 40);
        for (int i = 0; i + 1 < 40; ++i) {
            addNet(db, "n" + std::to_string(i),
                   {"c" + std::to_string(i), "c" + std::to_string(i + 1)});
        }
    };
    ktDM plain;
    build(plain);
    RatioPlacer(plain).place(smallLeaves());

    ktDM watched;
    build(watched);
    RatioPlacer placer(watched);
    std::size_t frames = 0;
    bool wholeDesign = true;
    bool depthInRange = true;
    placer.setFrameHook([&](const std::vector<float> &xs, const std::vector<float> &ys,
                            const RatioFrameInfo &info) {
        ++frames;
        wholeDesign =
            wholeDesign && xs.size() == watched.getGraph().getNumCells() && ys.size() == xs.size();
        depthInRange = depthInRange && info.step <= info.numSteps && !info.note.empty();
    });
    const RatioPlaceResult res = placer.place(smallLeaves());

    BOOST_TEST(frames == res.cuts);
    BOOST_TEST(wholeDesign);
    BOOST_TEST(depthInRange);
    // Watching is read-only: the same cuts land the cells in the same places.
    for (std::size_t v = 0; v < plain.getGraph().getNumCells(); ++v) {
        const auto [wx, wy] = watched.getCellPosition(v);
        const auto [px, py] = plain.getCellPosition(v);
        BOOST_TEST(wx == px);
        BOOST_TEST(wy == py);
    }
}

BOOST_AUTO_TEST_SUITE_END()
