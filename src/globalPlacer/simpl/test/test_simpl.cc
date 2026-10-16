// SPDX-License-Identifier: BSD-3-Clause
// @file test_simpl.cc
// Unit tests for SimPL's fence handling.
//
// Fences are exclusive (ISPD 2015): a region's members must lie inside it and
// every other cell outside. SimPL leaves its wirelength solve unconstrained and
// applies the fences to the look-ahead-legalized upper bound, which is both the
// anchors for the next solve and the placement it returns. These tests wire a
// fence's members to pads far from it -- so wirelength pulls them out -- and
// check that what SimPL hands on still keeps every member inside its fence and
// every other cell out of it.

#define BOOST_TEST_MODULE ktplace_simpl
#define BOOST_TEST_DYN_LINK

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "globalPlacer/simpl/simpl_dm.h"
#include "globalPlacer/simpl/simpl_placer.h"

#include <boost/test/unit_test.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace ktplace;
using namespace ktplace::simpl;

namespace {

constexpr double rowHeight = 10.0;
constexpr int rowCount = 40;
constexpr double rowWidth = 400.0;

// Fence: x 40..120, y 40..120 (rows 4..11), far from the pads on the right.
constexpr double fx0 = 40.0, fy0 = 40.0, fx1 = 120.0, fy1 = 120.0;

struct Built {
    std::vector<std::size_t> members, others;
};

/// A design whose fenced cells are pulled away from their fence by their nets,
/// and -- with `pullOthersIn` -- whose other cells are pulled into it.
Built buildDesign(ktDM &db, bool withFence, bool pullOthersIn = false) {
    for (int r = 0; r < rowCount; ++r) {
        const std::size_t row = db.addRow(r * rowHeight, rowHeight, 1.0, 1.0);
        (void)db.addSubrow(row, 0.0, rowWidth);
    }
    // Pads at the right edge, mid-height, outside the rows' x range.
    for (int p = 0; p < 4; ++p) {
        const std::size_t id = db.addCell("pad" + std::to_string(p), 1.0, 1.0, /*isTerminal=*/true);
        db.setCellPosition(id, rowWidth + 5.0, 150.0 + 30.0 * p);
        db.setCellFixed(id, true);
    }
    Built b;
    // Members: a chain whose every fourth cell is tied to a pad on the right.
    for (int i = 0; i < 60; ++i) {
        b.members.push_back(db.addCell("m" + std::to_string(i), 2.0, rowHeight));
    }
    // Others: a chain tied to the pads too, many enough to need spreading.
    for (int i = 0; i < 400; ++i) {
        b.others.push_back(db.addCell("o" + std::to_string(i), 2.0, rowHeight));
    }
    int net = 0;
    const auto addNet = [&](const std::string &a, const std::string &c) {
        const std::string name = "n" + std::to_string(net++);
        (void)db.addNet(name);
        (void)db.addPin(a, name, 0.0, 0.0, true);
        (void)db.addPin(c, name, 0.0, 0.0, false);
    };
    for (int i = 0; i + 1 < 60; ++i) {
        addNet("m" + std::to_string(i), "m" + std::to_string(i + 1));
    }
    for (int i = 0; i < 60; i += 4) {
        addNet("m" + std::to_string(i), "pad" + std::to_string(i % 4));
    }
    for (int i = 0; i + 1 < 400; ++i) {
        addNet("o" + std::to_string(i), "o" + std::to_string(i + 1));
    }
    for (int i = 0; i < 400; i += 25) {
        addNet("o" + std::to_string(i), "pad" + std::to_string(i % 4));
    }
    // A pad in the middle of the fence, wired to some of the other cells: their
    // nets pull them into the fence, which they must not enter.
    if (pullOthersIn) {
        const std::size_t id = db.addCell("padIn", 1.0, 1.0, /*isTerminal=*/true);
        db.setCellPosition(id, 0.5 * (fx0 + fx1), 0.5 * (fy0 + fy1));
        db.setCellFixed(id, true);
        for (int i = 5; i < 400; i += 10) {
            addNet("o" + std::to_string(i), "padIn");
        }
    }
    // A link between the two groups, so the fence is not an island.
    addNet("m0", "o0");
    if (withFence) {
        constraintMgr mgr;
        const int id = mgr.addRegion("f0", {Point{fx0, fy0}, Point{fx1, fy1}});
        (void)mgr.assignByPrefix(id, "m", db.getGraph());
        db.setConstraints(std::move(mgr));
    }
    return b;
}

/// Run SimPL and commit its answer.
SimplResult place(ktDM &db, bool useFences) {
    // No plots: this checks the solver, not what it draws.
    simpl::simplDM dm = simpl::buildSimplDM(db);
    dm.options() = SimplOptions{};
    dm.options().maxIters = 30;
    dm.options().fences = useFences;
    const SimplResult res = dm.run();
    dm.commit(db);
    return res;
}

bool inFence(double x, double y) {
    return x >= fx0 && x <= fx1 && y >= fy0 && y <= fy1;
}

}  // namespace

struct ScopedOption {
    ScopedOption(Component component, std::string key, OptionValue value) {
        OptionDb::instance().set(component, std::move(key), std::move(value));
    }
    ~ScopedOption() {
        OptionDb::instance().clear();
    }
    ScopedOption(const ScopedOption &) = delete;
    ScopedOption &operator=(const ScopedOption &) = delete;
};

struct Offered {
    std::size_t lss = 0;
    std::size_t bounds = 0;
};

Offered offered() {
    ktDM db;
    (void)buildDesign(db, /*withFence=*/false);
    simpl::simplDM dm = simpl::buildSimplDM(db);
    Offered n;
    dm.setFrameHook(
        [&n](const std::vector<float> &, const std::vector<float> &, const simpl::FrameInfo &info) {
            n.lss += info.series == "LSS" ? 1u : 0u;
        });
    dm.setBoundsHook([&n](std::span<const simpl::BoundsPoint> curve) {
        n.bounds = curve.size();
    });
    dm.options().maxIters = 30;
    (void)dm.run();
    return n;
}

BOOST_AUTO_TEST_SUITE(ktplace_simpl)

BOOST_AUTO_TEST_CASE(the_returned_placement_keeps_members_in_their_fence) {
    ktDM db;
    const Built b = buildDesign(db, /*withFence=*/true);
    const SimplResult res = place(db, /*useFences=*/true);

    BOOST_TEST(res.fenceRegions == 1u);
    BOOST_TEST(res.fenceViolations == 0u);
    std::size_t outside = 0;
    for (const std::size_t id : b.members) {
        const auto [x, y] = db.getCellPosition(id);
        outside += inFence(x, y) ? 0u : 1u;
    }
    BOOST_TEST(outside == 0u);
}

BOOST_AUTO_TEST_CASE(the_returned_placement_keeps_other_cells_out_of_the_fence) {
    // Some of the other cells are wired to a pad in the middle of the fence.
    ktDM db;
    const Built b = buildDesign(db, /*withFence=*/true, /*pullOthersIn=*/true);
    (void)place(db, /*useFences=*/true);

    std::size_t inside = 0;
    for (const std::size_t id : b.others) {
        const auto [x, y] = db.getCellPosition(id);
        inside += inFence(x, y) ? 1u : 0u;
    }
    BOOST_TEST(inside == 0u);
}

BOOST_AUTO_TEST_CASE(the_wirelength_solve_is_not_fenced) {
    // The lower bound is the unconstrained solve; fences reach it only through
    // the anchors, which the warm-up does not have. So the warm-up must come out
    // exactly the same with fences on and off. Clamping the lower bound after
    // each solve, as this placer once did, breaks that -- and cost +18.7% final
    // HPWL on the fenced ISPD 2015 designs.
    ktDM fenced;
    (void)buildDesign(fenced, /*withFence=*/true, /*pullOthersIn=*/true);
    const SimplResult on = place(fenced, /*useFences=*/true);
    ktDM open;
    (void)buildDesign(open, /*withFence=*/true, /*pullOthersIn=*/true);
    const SimplResult off = place(open, /*useFences=*/false);

    BOOST_TEST(on.fenceRegions == 1u);
    // A relative tolerance far below what a projection moves (about 35% here),
    // so a change in summation order can never fail it.
    BOOST_TEST(on.hpwlInit == off.hpwlInit, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(fences_can_still_be_turned_off_to_measure_their_cost) {
    // useFences = false is how a run measures what the constraint costs; the
    // members are then free to follow their nets out of the fence.
    ktDM db;
    const Built b = buildDesign(db, /*withFence=*/true);
    const SimplResult res = place(db, /*useFences=*/false);
    BOOST_TEST(res.fenceRegions == 0u);
    std::size_t outside = 0;
    for (const std::size_t id : b.members) {
        const auto [x, y] = db.getCellPosition(id);
        outside += inFence(x, y) ? 0u : 1u;
    }
    BOOST_TEST(outside > 0u);
}

BOOST_AUTO_TEST_CASE(snapshot_cadence_is_simpls_own_option) {
    const Offered byDefault = offered();
    BOOST_TEST(byDefault.lss > 0u);
    BOOST_TEST(byDefault.bounds > 0u);
    {
        const ScopedOption off(Component::Simpl, "trace_every", OptionValue{0});
        const Offered n = offered();
        BOOST_TEST(n.lss == 0u);
        BOOST_TEST(n.bounds == byDefault.bounds);
    }
}

BOOST_AUTO_TEST_SUITE_END()
