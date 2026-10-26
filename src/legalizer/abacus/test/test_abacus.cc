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
#define BOOST_TEST_DYN_LINK

#include "datamodel/kt_constraintMgr.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "legalizer/abacus/abacus_dm.h"
#include "legalizer/abacus/abacus_flowMgr.h"

#include <boost/test/unit_test.hpp>
#include <cmath>
#include <string>
#include <vector>

using namespace ktplace;

namespace {

constexpr double rowHeight = 10.0;
constexpr int rowCount = 10;
constexpr double rowWidth = 100.0;

void addRows(ktDM &db) {
    for (int r = 0; r < rowCount; ++r) {
        const std::size_t row = db.addRow(r * rowHeight, rowHeight, 1.0, 1.0);
        (void)db.addSubrow(row, 0.0, rowWidth);
    }
}

std::vector<std::size_t> addCells(ktDM &db, const std::string &prefix, int n, double x, double y) {
    std::vector<std::size_t> ids;
    for (int i = 0; i < n; ++i) {
        const std::size_t id = db.addCell(prefix + std::to_string(i), 1.0, rowHeight);
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

void legalize(ktDM &db) {
    abacus::flowMgr flow(db);
    flow.getDm().getOptions() = AbacusOptions{};
    flow.run();
    db.setAbacusSolution(flow.getSolution());
}

bool isEveryCellOnARow(const ktDM &db) {
    for (std::size_t id = 0; id < db.getNumCells(); ++id) {
        const auto [x, y] = db.getCellPosition(id);
        const double row = std::round(y / rowHeight);
        if (std::abs(y - row * rowHeight) > 1e-9 || row < 0.0 || row >= rowCount || x < -1e-9 ||
            x + 1.0 > rowWidth + 1e-9) {
            return false;
        }
    }
    return true;
}

std::size_t getOverlappingPairs(const ktDM &db) {
    std::size_t pairs = 0;
    for (std::size_t a = 0; a < db.getNumCells(); ++a) {
        for (std::size_t b = a + 1; b < db.getNumCells(); ++b) {
            const auto [ax, ay] = db.getCellPosition(a);
            const auto [bx, by] = db.getCellPosition(b);
            pairs += std::abs(ax - bx) < 1.0 - 1e-9 && std::abs(ay - by) < rowHeight - 1e-9;
        }
    }
    return pairs;
}

bool isInsideBox(const ktDM &db, std::size_t id, double x0, double y0, double x1, double y1) {
    const auto [x, y] = db.getCellPosition(id);
    return x >= x0 - 1e-9 && x + 1.0 <= x1 + 1e-9 && y >= y0 - 1e-9 && y + rowHeight <= y1 + 1e-9;
}

bool overlapsBox(const ktDM &db, std::size_t id, double x0, double y0, double x1, double y1) {
    const auto [x, y] = db.getCellPosition(id);
    return x < x1 - 1e-9 && x + 1.0 > x0 + 1e-9 && y < y1 - 1e-9 && y + rowHeight > y0 + 1e-9;
}

}  // namespace

struct ScopedOption {
    ScopedOption(Component component, std::string key, OptionValue value) {
        OptionDb::getInstance().set(component, std::move(key), std::move(value));
    }
    ~ScopedOption() {
        OptionDb::getInstance().clear();
    }
    ScopedOption(const ScopedOption &) = delete;
    ScopedOption &operator=(const ScopedOption &) = delete;
};

BOOST_AUTO_TEST_SUITE(ktplace_abacus)

BOOST_AUTO_TEST_CASE(members_far_outside_are_brought_into_their_fence) {
    ktDM db;
    addRows(db);
    // Fence over rows 2..5, x 20..50. Members start in the far corner, outside.
    const auto members = addCells(db, "m", 40, 90.0, 85.0);
    addFence(db, 20.0, 20.0, 50.0, 60.0, "m");

    legalize(db);
    BOOST_TEST(isEveryCellOnARow(db));
    for (const std::size_t id : members) {
        BOOST_TEST(isInsideBox(db, id, 20.0, 20.0, 50.0, 60.0), "member " << id << " outside");
    }
}

BOOST_AUTO_TEST_CASE(non_members_inside_a_fence_are_moved_out) {
    ktDM db;
    addRows(db);
    const auto members = addCells(db, "m", 5, 30.0, 30.0);
    const auto others = addCells(db, "o", 30, 30.0, 30.0);  // dropped inside the fence
    addFence(db, 20.0, 20.0, 50.0, 60.0, "m");

    legalize(db);
    BOOST_TEST(isEveryCellOnARow(db));
    for (const std::size_t id : others) {
        BOOST_TEST(!overlapsBox(db, id, 20.0, 20.0, 50.0, 60.0), "non-member " << id << " inside");
    }
    for (const std::size_t id : members) {
        BOOST_TEST(isInsideBox(db, id, 20.0, 20.0, 50.0, 60.0));
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

    legalize(db);
    BOOST_TEST(isEveryCellOnARow(db));
    BOOST_TEST(getOverlappingPairs(db) == 0u);
    for (const std::size_t id : members) {
        BOOST_TEST(isInsideBox(db, id, 10.0, 40.0, 22.0, 50.0), "member " << id << " outside");
    }
}

BOOST_AUTO_TEST_CASE(an_unfenced_design_is_legalized_as_before) {
    // No fences: every piece is open space and every cell may use it.
    ktDM db;
    addRows(db);
    const auto cells = addCells(db, "c", 50, 50.0, 50.0);
    legalize(db);
    BOOST_TEST(isEveryCellOnARow(db));
    BOOST_TEST(getOverlappingPairs(db) == 0u);
    (void)cells;
}

BOOST_AUTO_TEST_CASE(frame_count_is_abacus_own_option) {
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
        dm.run();
        return frames;
    };
    {
        const ScopedOption none(Component::Abacus, "frames", OptionValue{0});
        BOOST_TEST(framesOffered() == 2u);
    }
    const ScopedOption five(Component::Abacus, "frames", OptionValue{5});
    BOOST_TEST(framesOffered() == 2u + 5u);
}

BOOST_AUTO_TEST_SUITE_END()
