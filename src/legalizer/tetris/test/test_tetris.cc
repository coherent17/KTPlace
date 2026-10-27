// SPDX-License-Identifier: BSD-3-Clause

#define BOOST_TEST_MODULE ktplace_tetris
#define BOOST_TEST_DYN_LINK

#include "datamodel/kt_constraintMgr.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "legalizer/tetris/tetris_dm.h"
#include "legalizer/tetris/tetris_flowMgr.h"

#include <boost/test/unit_test.hpp>
#include <cmath>
#include <string>
#include <vector>

using namespace ktplace;

namespace {

constexpr double rowHeight = 10.0;

void addRows(ktDM &db, int count, double width) {
    for (int r = 0; r < count; ++r) {
        const std::size_t row = db.addRow(r * rowHeight, rowHeight, 1.0, 1.0);
        (void)db.addSubrow(row, 0.0, width);
    }
}

std::size_t addCell(ktDM &db, const std::string &name, double width, Point position) {
    const std::size_t id = db.addCell(name, width, rowHeight);
    db.setCellPosition(id, position);
    return id;
}

std::vector<std::size_t> addCells(ktDM &db, const std::string &prefix, int n, Point position) {
    std::vector<std::size_t> ids;
    for (int i = 0; i < n; ++i) {
        ids.push_back(addCell(db, prefix + std::to_string(i), 1.0, position));
    }
    return ids;
}

void legalize(ktDM &db, const TetrisOptions &options = {}) {
    tetris::flowMgr flow(db);
    flow.getDm().getOptions() = options;
    flow.run();
    db.setTetrisSolution(flow.getSolution());
}

double getWidth(const ktDM &db, std::size_t id) {
    return db.getGraph().getCell(id).width;
}

bool isOnRowAndSite(const ktDM &db, std::size_t id, int rows, double rowWidth) {
    const auto [x, y] = db.getCellPosition(id);
    const double row = std::round(y / rowHeight);
    return std::abs(y - row * rowHeight) < 1e-9 && row >= 0.0 && row < rows &&
           std::abs(x - std::round(x)) < 1e-9 && x > -1e-9 &&
           x + getWidth(db, id) < rowWidth + 1e-9;
}

std::size_t getOverlappingPairs(const ktDM &db) {
    std::size_t pairs = 0;
    for (std::size_t a = 0; a < db.getNumCells(); ++a) {
        for (std::size_t b = a + 1; b < db.getNumCells(); ++b) {
            const auto [ax, ay] = db.getCellPosition(a);
            const auto [bx, by] = db.getCellPosition(b);
            const bool apart =
                ax + getWidth(db, a) <= bx + 1e-9 || bx + getWidth(db, b) <= ax + 1e-9;
            pairs += !apart && std::abs(ay - by) < rowHeight - 1e-9;
        }
    }
    return pairs;
}

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

}  // namespace

BOOST_AUTO_TEST_SUITE(ktplace_tetris)

BOOST_AUTO_TEST_CASE(a_clump_lands_on_rows_and_sites_without_overlap) {
    ktDM db;
    addRows(db, 10, 100.0);
    const auto cells = addCells(db, "c", 60, Point(50.3, 47.0));
    legalize(db);
    for (const std::size_t id : cells) {
        BOOST_TEST(isOnRowAndSite(db, id, 10, 100.0), "cell " << id << " is off a row or site");
    }
    BOOST_TEST(getOverlappingPairs(db) == 0u);
}

BOOST_AUTO_TEST_CASE(wider_cells_go_first_and_take_the_left_most_vacant_site) {
    ktDM db;
    addRows(db, 1, 40.0);
    const std::size_t narrow = addCell(db, "narrow", 1.0, Point(10.0, 0.0));
    const std::size_t wide = addCell(db, "wide", 4.0, Point(10.0, 0.0));
    legalize(db);
    BOOST_TEST(db.getCellPosition(wide).x == 6.0);
    BOOST_TEST(db.getCellPosition(narrow).x == 10.0);
}

BOOST_AUTO_TEST_CASE(placed_cells_never_move) {
    ktDM db;
    addRows(db, 1, 40.0);
    const std::size_t first = addCell(db, "first", 2.0, Point(10.0, 0.0));
    const std::size_t second = addCell(db, "second", 2.0, Point(10.5, 0.0));
    legalize(db, TetrisOptions{.widthFactor = 0.0});
    BOOST_TEST(db.getCellPosition(first).x == 8.0);
    BOOST_TEST(db.getCellPosition(second).x == 10.0);
}

BOOST_AUTO_TEST_CASE(a_fixed_macro_is_treated_like_a_placed_cell) {
    ktDM db;
    addRows(db, 3, 60.0);
    const std::size_t macro = db.addCell("macro", 20.0, 3.0 * rowHeight);
    db.setCellPosition(macro, Point(20.0, 0.0));
    db.setCellFixed(macro, true);
    const auto cells = addCells(db, "c", 30, Point(30.0, 10.0));
    legalize(db);
    BOOST_TEST(getOverlappingPairs(db) == 0u);
    for (const std::size_t id : cells) {
        const double x = db.getCellPosition(id).x;
        BOOST_TEST((x + 1.0 <= 20.0 + 1e-9 || x >= 40.0 - 1e-9),
                   "cell " << id << " is on the macro");
    }
}

BOOST_AUTO_TEST_CASE(a_cell_past_every_boundary_takes_the_nearest_free_site) {
    ktDM db;
    addRows(db, 1, 20.0);
    const auto cells = addCells(db, "c", 10, Point(19.0, 0.0));
    legalize(db);
    BOOST_TEST(getOverlappingPairs(db) == 0u);
    for (const std::size_t id : cells) {
        BOOST_TEST(isOnRowAndSite(db, id, 1, 20.0));
        BOOST_TEST(db.getCellPosition(id).x >= 10.0, "cell " << id << " was sent too far left");
    }
}

BOOST_AUTO_TEST_CASE(a_full_row_leaves_the_extra_cell_where_it_was) {
    ktDM db;
    addRows(db, 1, 10.0);
    const auto cells = addCells(db, "c", 11, Point(3.5, 0.0));
    legalize(db);
    std::size_t onRow = 0;
    for (const std::size_t id : cells) {
        onRow += isOnRowAndSite(db, id, 1, 10.0) ? 1 : 0;
    }
    BOOST_TEST(onRow == 10u);
    BOOST_TEST(db.getCellPosition(cells.back()).x == 3.5);
}

BOOST_AUTO_TEST_CASE(y_weight_trades_rows_against_sliding) {
    const auto rowOfLast = [](double yWeight) {
        ktDM db;
        addRows(db, 3, 40.0);
        (void)addCells(db, "c", 12, Point(10.0, 10.0));
        legalize(db, TetrisOptions{.yWeight = yWeight});
        return db.getCellPosition(db.getNumCells() - 1).y;
    };
    BOOST_TEST(rowOfLast(1000.0) == 10.0);
    BOOST_TEST(rowOfLast(0.001) != 10.0);
}

BOOST_AUTO_TEST_CASE(members_are_placed_inside_their_fence) {
    ktDM db;
    addRows(db, 10, 100.0);
    const auto members = addCells(db, "m", 20, Point(90.0, 85.0));
    const auto others = addCells(db, "o", 20, Point(30.0, 30.0));
    constraintMgr mgr;
    const int region = mgr.addRegion("f0", {Point(20.0, 20.0), Point(50.0, 60.0)});
    (void)mgr.assignByPrefix(region, "m", db.getGraph());
    db.setConstraints(std::move(mgr));
    legalize(db);
    for (const std::size_t id : members) {
        const auto [x, y] = db.getCellPosition(id);
        BOOST_TEST((x >= 20.0 - 1e-9 && x + 1.0 <= 50.0 + 1e-9 && y >= 20.0 - 1e-9 &&
                    y + rowHeight <= 60.0 + 1e-9),
                   "member " << id << " is outside its fence");
    }
    for (const std::size_t id : others) {
        const auto [x, y] = db.getCellPosition(id);
        BOOST_TEST(!(x < 50.0 - 1e-9 && x + 1.0 > 20.0 + 1e-9 && y < 60.0 - 1e-9 &&
                     y + rowHeight > 20.0 + 1e-9),
                   "non-member " << id << " is inside the fence");
    }
}

BOOST_AUTO_TEST_CASE(frame_count_is_tetris_own_option) {
    const auto framesOffered = [] {
        ktDM db;
        addRows(db, 10, 100.0);
        (void)addCells(db, "c", 30, Point(30.0, 30.0));
        tetris::tetrisDM dm = tetris::buildTetrisDM(db);
        std::size_t frames = 0;
        dm.setFrameHook([&frames](const std::vector<float> &, const std::vector<float> &,
                                  const tetris::FrameInfo &) {
            ++frames;
        });
        dm.run();
        return frames;
    };
    {
        const ScopedOption none(Component::Tetris, "frames", OptionValue{0});
        BOOST_TEST(framesOffered() == 2u);
    }
    const ScopedOption five(Component::Tetris, "frames", OptionValue{5});
    BOOST_TEST(framesOffered() == 2u + 5u);
}

BOOST_AUTO_TEST_SUITE_END()
