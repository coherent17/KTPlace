// SPDX-License-Identifier: BSD-3-Clause
// @file test_abacus.cc
// Unit tests for the Abacus legalizer's fence handling.
//
// Fences are exclusive (ISPD 2015): a region's members must end up inside it and
// every other cell outside. Abacus enforces that by labelling each piece of a row
// with its region and only offering a cell pieces of its own label. These tests
// put members far outside their fence and non-members inside it -- the state a
// global placer that ignores fences hands over -- and check that legalization
// alone puts both right, without leaving anyone unplaced.

#define BOOST_TEST_MODULE ktplace_abacus

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_dm.h"
#include "legalizer/abacus/abacus_dm.h"
#include "legalizer/abacus/abacus_legalizer.h"
#include "legalizer/multirow/multirow_dm.h"
#include "util/kt_log.h"

#include <boost/test/included/unit_test.hpp>
#include <string>
#include <vector>

using namespace ktplace;

namespace {

constexpr double kRowHeight = 10.0;
constexpr int kRows = 10;
constexpr double kWidth = 100.0;

void addRows(ktDM &db) {
    for (int r = 0; r < kRows; ++r) {
        const std::size_t row = db.addRow(r * kRowHeight, kRowHeight, 1.0, 1.0);
        (void)db.addSubrow(row, 0.0, kWidth);
    }
}

std::vector<std::size_t> addCells(ktDM &db, const std::string &prefix, int n, double x, double y) {
    std::vector<std::size_t> ids;
    for (int i = 0; i < n; ++i) {
        const std::size_t id = db.addCell(prefix + std::to_string(i), 1.0, kRowHeight);
        db.setCellPosition(id, x, y);
        ids.push_back(id);
    }
    return ids;
}

/// Fence "f0" over [x0,x1] x [y0,y1], binding every cell whose name starts with
/// `prefix`. Installed after the cells exist, as the LEF/DEF reader does.
void addFence(ktDM &db, double x0, double y0, double x1, double y1, const std::string &prefix) {
    constraintMgr mgr;
    const int id = mgr.addRegion("f0", {Point{x0, y0}, Point{x1, y1}});
    (void)mgr.assignByPrefix(id, prefix, db.getGraph());
    db.setConstraints(std::move(mgr));
}

/// Legalize and commit, returning the legalizer's own result.
LegalizeResult legalize(ktDM &db) {
    abacus::abacusDM dm = abacus::buildAbacusDM(db);
    AbacusLegalizer legalizer(dm.design());
    const LegalizeResult res = legalizer.legalize();
    db.setLegalizationSolution(legalizer.solution());
    return res;
}

bool insideBox(const ktDM &db, std::size_t id, double x0, double y0, double x1, double y1) {
    const auto [x, y] = db.getCellPosition(id);
    return x >= x0 - 1e-9 && x + 1.0 <= x1 + 1e-9 && y >= y0 - 1e-9 && y + kRowHeight <= y1 + 1e-9;
}

bool overlapsBox(const ktDM &db, std::size_t id, double x0, double y0, double x1, double y1) {
    const auto [x, y] = db.getCellPosition(id);
    return x < x1 - 1e-9 && x + 1.0 > x0 + 1e-9 && y < y1 - 1e-9 && y + kRowHeight > y0 + 1e-9;
}

/// Sets an option for one scope, so a failed check cannot leak it into the
/// cases that follow.
struct ScopedOption {
    ScopedOption(const std::string &component, const std::string &key, OptionValue value) {
        OptionDb::instance().set(component, key, std::move(value));
    }
    ~ScopedOption() {
        OptionDb::instance().clear();
    }
    ScopedOption(const ScopedOption &) = delete;
    ScopedOption &operator=(const ScopedOption &) = delete;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(ktplace_abacus)

BOOST_AUTO_TEST_CASE(members_far_outside_are_brought_into_their_fence) {
    ktDM db;
    addRows(db);
    // Fence over rows 2..5, x 20..50. Members start in the far corner, outside.
    const auto members = addCells(db, "m", 40, 90.0, 85.0);
    addFence(db, 20.0, 20.0, 50.0, 60.0, "m");

    const LegalizeResult res = legalize(db);
    BOOST_TEST(res.unplaced == 0u);
    for (const std::size_t id : members) {
        BOOST_TEST(insideBox(db, id, 20.0, 20.0, 50.0, 60.0), "member " << id << " outside");
    }
}

BOOST_AUTO_TEST_CASE(non_members_inside_a_fence_are_moved_out) {
    ktDM db;
    addRows(db);
    const auto members = addCells(db, "m", 5, 30.0, 30.0);
    const auto others = addCells(db, "o", 30, 30.0, 30.0);  // dropped inside the fence
    addFence(db, 20.0, 20.0, 50.0, 60.0, "m");

    const LegalizeResult res = legalize(db);
    BOOST_TEST(res.unplaced == 0u);
    for (const std::size_t id : others) {
        BOOST_TEST(!overlapsBox(db, id, 20.0, 20.0, 50.0, 60.0), "non-member " << id << " inside");
    }
    for (const std::size_t id : members) {
        BOOST_TEST(insideBox(db, id, 20.0, 20.0, 50.0, 60.0));
    }
}

BOOST_AUTO_TEST_CASE(a_fence_whose_edge_overflows_is_filled_from_the_left) {
    // A one-row fence of 12 sites, and 12 members all aiming past its right edge.
    // Appending each at the right edge fills it after one; sliding the placed
    // clusters left into the free sites is what fits all twelve.
    ktDM db;
    addRows(db);
    const auto members = addCells(db, "m", 12, 95.0, 40.0);
    addFence(db, 10.0, 40.0, 22.0, 50.0, "m");

    const LegalizeResult res = legalize(db);
    BOOST_TEST(res.unplaced == 0u);
    BOOST_TEST(res.overlappingPairs == 0u);
    for (const std::size_t id : members) {
        BOOST_TEST(insideBox(db, id, 10.0, 40.0, 22.0, 50.0), "member " << id << " outside");
    }
}

BOOST_AUTO_TEST_CASE(an_unfenced_design_is_legalized_as_before) {
    // No fences: every piece is open space and every cell may use it.
    ktDM db;
    addRows(db);
    const auto cells = addCells(db, "c", 50, 50.0, 50.0);
    const LegalizeResult res = legalize(db);
    BOOST_TEST(res.unplaced == 0u);
    BOOST_TEST(res.overlappingPairs == 0u);
    (void)cells;
}

BOOST_AUTO_TEST_CASE(frame_cadence_is_abacus_own_option) {
    const auto framesOffered = [] {
        ktDM db;
        addRows(db);
        (void)addCells(db, "c", 30, 30.0, 30.0);
        abacus::abacusDM dm = abacus::buildAbacusDM(db);
        std::size_t frames = 0;
        dm.setFrameHook([&frames](const std::vector<float> &, const std::vector<float> &,
                                  const abacus::FrameInfo &) {
            ++frames;
        });
        (void)dm.run(dm.params());
        return frames;
    };
    // By default only the input and the legal placement.
    BOOST_TEST(framesOffered() == 2u);
    const ScopedOption every("abacus", "frame_every", OptionValue{10});
    // Plus one every ten of the thirty cells placed.
    BOOST_TEST(framesOffered() == 5u);
}

BOOST_AUTO_TEST_CASE(the_multirow_pass_takes_its_cadence_from_its_own_option) {
    ktDM db;
    addRows(db);
    BOOST_TEST(multirow::buildMultiRowDM(db).params().frameEvery == 20000u);
    const ScopedOption every("multirow", "frame_every", OptionValue{500});
    BOOST_TEST(multirow::buildMultiRowDM(db).params().frameEvery == 500u);
}

BOOST_AUTO_TEST_SUITE_END()
