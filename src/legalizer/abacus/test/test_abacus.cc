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
#include "legalizer/kt_rowFence.h"
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

// ---- splitRowByFences ------------------------------------------------------

BOOST_AUTO_TEST_CASE(a_fence_covering_the_row_becomes_member_space) {
    // Row [0,100) x [0,10); fence 0 covers x 20..50 over the whole band.
    const std::vector<RowPiece> p =
        splitRowByFences(0.0, 100.0, 0.0, 10.0, 1.0, {FenceRect{20.0, 0.0, 50.0, 10.0, 0}});
    BOOST_TEST_REQUIRE(p.size() == 3u);
    BOOST_TEST(p[0].xlo == 0.0);
    BOOST_TEST(p[0].xhi == 20.0);
    BOOST_TEST(p[0].region == kOpenSpace);
    BOOST_TEST(p[1].xlo == 20.0);
    BOOST_TEST(p[1].xhi == 50.0);
    BOOST_TEST(p[1].region == 0);
    BOOST_TEST(p[2].xlo == 50.0);
    BOOST_TEST(p[2].xhi == 100.0);
    BOOST_TEST(p[2].region == kOpenSpace);
}

BOOST_AUTO_TEST_CASE(a_fence_reaching_partly_into_the_row_blocks_it_for_everyone) {
    // Covers only the top half of the band: a member would stick out, any other
    // cell would sit inside it, so the stretch is dropped.
    const std::vector<RowPiece> p =
        splitRowByFences(0.0, 100.0, 0.0, 10.0, 1.0, {FenceRect{60.0, 5.0, 70.0, 20.0, 0}});
    BOOST_TEST_REQUIRE(p.size() == 2u);
    BOOST_TEST(p[0].xhi == 60.0);
    BOOST_TEST(p[1].xlo == 70.0);
    BOOST_TEST(p[0].region == kOpenSpace);
    BOOST_TEST(p[1].region == kOpenSpace);
}

BOOST_AUTO_TEST_CASE(fence_edges_off_the_site_grid_are_snapped_inward) {
    // Fence edges at 20.5 and 49.5 on a 1-unit grid: the member piece shrinks to
    // [21,49] and the open pieces stop at 20 and resume at 50, so no cell can sit
    // across the boundary.
    const std::vector<RowPiece> p =
        splitRowByFences(0.0, 100.0, 0.0, 10.0, 1.0, {FenceRect{20.5, 0.0, 49.5, 10.0, 3}});
    BOOST_TEST_REQUIRE(p.size() == 3u);
    BOOST_TEST(p[0].xhi == 20.0);
    BOOST_TEST(p[1].xlo == 21.0);
    BOOST_TEST(p[1].xhi == 49.0);
    BOOST_TEST(p[1].region == 3);
    BOOST_TEST(p[2].xlo == 50.0);
}

BOOST_AUTO_TEST_CASE(adjacent_rectangles_of_one_region_merge_and_rows_without_fences_pass_through) {
    // An L-shaped region as two touching rectangles is one stretch of member space.
    const std::vector<RowPiece> p = splitRowByFences(
        0.0, 100.0, 0.0, 10.0, 1.0,
        {FenceRect{10.0, 0.0, 30.0, 10.0, 1}, FenceRect{30.0, 0.0, 40.0, 50.0, 1}});
    BOOST_TEST_REQUIRE(p.size() == 3u);
    BOOST_TEST(p[1].xlo == 10.0);
    BOOST_TEST(p[1].xhi == 40.0);
    BOOST_TEST(p[1].region == 1);
    // A row the fences do not reach is returned whole.
    const std::vector<RowPiece> q =
        splitRowByFences(0.0, 100.0, 200.0, 210.0, 1.0, {FenceRect{10.0, 0.0, 30.0, 10.0, 1}});
    BOOST_TEST_REQUIRE(q.size() == 1u);
    BOOST_TEST(q[0].xlo == 0.0);
    BOOST_TEST(q[0].xhi == 100.0);
    BOOST_TEST(q[0].region == kOpenSpace);
}

BOOST_AUTO_TEST_SUITE_END()
