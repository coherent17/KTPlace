// SPDX-License-Identifier: BSD-3-Clause
// @file test_dmp.cc
// Unit tests for detailed macro placement and the ICCAD 2022 Problem D macro files.

#define BOOST_TEST_MODULE ktplace_dmp
#define BOOST_TEST_DYN_LINK

#include "adaptor/kt_macroPlanIo.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_graph.h"
#include "datamodel/kt_macroPlan.h"
#include "datamodel/kt_optionDb.h"
#include "macroPlacer/dmp/dmp_dm.h"
#include "macroPlacer/dmp/dmp_flowMgr.h"

#include <boost/test/unit_test.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

using namespace ktplace;

namespace {

constexpr double macroSize = 100.0;

struct Bench {
    ktDM db;
    std::size_t macro = 0;
    MacroPlan plan;

    explicit Bench(double reach) {
        db.setDieArea(0.0, 0.0, 1000.0, 1000.0);
        macro = db.addCell("m", macroSize, macroSize, true);
        db.setCellPosition(macro, 400.0, 400.0);
        db.setCellFixed(macro, true);
        const std::size_t pad = db.addCell("pad", 0.0, 0.0, true);
        db.setCellPosition(pad, 900.0, 450.0);
        db.setCellFixed(pad, true);
        (void)db.addNet("a");
        (void)db.addPin("m", "a", 10.0, 50.0, false);
        (void)db.addPin("pad", "a", 0.0, 0.0, true);
        plan.die = {0.0, 0.0, 1000.0, 1000.0};
        plan.diePoints = {{0.0, 0.0}, {1000.0, 1000.0}};
        plan.rules.maxDisplacement = reach;
        plan.macros.push_back({.cellId = macro,
                               .master = "block",
                               .movable = true,
                               .originX = 400.0,
                               .originY = 400.0,
                               .originOrient = Orient::N});
    }

    std::size_t addBlocker(double x, double y) {
        const std::size_t id = db.addCell("blocker", macroSize, macroSize, true);
        db.setCellPosition(id, x, y);
        db.setCellFixed(id, true);
        plan.macros.push_back({.cellId = id,
                               .master = "block",
                               .movable = false,
                               .originX = x,
                               .originY = y,
                               .originOrient = Orient::N});
        return id;
    }

    void place() {
        db.setMacroPlan(plan);
        dmp::flowMgr flow(db);
        (void)flow.run();
        db.setDmpSolution(flow.solution());
    }

    [[nodiscard]] const Vertex &cell() const {
        return db.getGraph().getCell(macro);
    }
};

struct ScopedOption {
    ScopedOption(std::string key, OptionValue value) {
        OptionDb::instance().set(Component::Dmp, std::move(key), std::move(value));
    }
    ~ScopedOption() {
        OptionDb::instance().clear();
    }
    ScopedOption(const ScopedOption &) = delete;
    ScopedOption &operator=(const ScopedOption &) = delete;
};

std::string slurp(const std::filesystem::path &path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

BOOST_AUTO_TEST_CASE(flipping_mirrors_the_pin_offsets_and_back) {
    Bench bench(0.0);
    const Pin &pin = bench.db.getGraph().getPin(0);
    bench.db.setCellOrient(bench.macro, Orient::FN);
    BOOST_TEST(pin.offsetX == 90.0);
    BOOST_TEST(pin.offsetY == 50.0);
    bench.db.setCellOrient(bench.macro, Orient::S);
    BOOST_TEST(pin.offsetX == 90.0);
    BOOST_TEST(pin.offsetY == 50.0);
    bench.db.setCellOrient(bench.macro, Orient::FS);
    BOOST_TEST(pin.offsetX == 10.0);
    BOOST_TEST(pin.offsetY == 50.0);
    bench.db.setCellOrient(bench.macro, Orient::N);
    BOOST_TEST(pin.offsetX == 10.0);
    BOOST_TEST((bench.cell().orient == Orient::N));
}

BOOST_AUTO_TEST_CASE(a_macro_moves_and_flips_towards_its_net_within_the_reach) {
    Bench bench(50.0);
    const double before = bench.db.hpwl();
    bench.place();
    BOOST_TEST(bench.cell().x == 450.0);
    BOOST_TEST(bench.cell().y == 400.0);
    BOOST_TEST((bench.cell().orient == Orient::FN));
    BOOST_TEST(bench.db.hpwl() == 360.0);
    BOOST_TEST(bench.db.hpwl() < before);
    BOOST_TEST(bench.db.verifyMacros().empty());
}

BOOST_AUTO_TEST_CASE(a_fixed_neighbour_stops_the_shift_at_its_edge) {
    Bench bench(50.0);
    const std::size_t blocker = bench.addBlocker(520.0, 400.0);
    bench.place();
    BOOST_TEST(bench.cell().x == 420.0);
    BOOST_TEST((bench.cell().orient == Orient::FN));
    BOOST_TEST(bench.db.getGraph().getCell(blocker).x == 520.0);
    BOOST_TEST(bench.db.verifyMacros().empty());
}

BOOST_AUTO_TEST_CASE(a_zero_reach_still_allows_a_flip) {
    Bench bench(0.0);
    bench.place();
    BOOST_TEST(bench.cell().x == 400.0);
    BOOST_TEST(bench.cell().y == 400.0);
    BOOST_TEST((bench.cell().orient == Orient::FN));
    BOOST_TEST(bench.db.verifyMacros().empty());
}

BOOST_AUTO_TEST_CASE(the_flip_option_turns_flips_off) {
    const ScopedOption noFlip("flip", OptionValue{false});
    Bench bench(50.0);
    bench.place();
    BOOST_TEST(bench.cell().x == 450.0);
    BOOST_TEST((bench.cell().orient == Orient::N));
}

BOOST_AUTO_TEST_CASE(snapping_puts_a_macro_edge_on_the_row_and_site_grid) {
    const ScopedOption snap("snap", OptionValue{true});
    Bench bench(50.0);
    const std::size_t row = bench.db.addRow(0.0, 30.0, 10.0, 10.0);
    (void)bench.db.addSubrow(row, 0.0, 100.0);
    bench.place();
    const auto onGrid = [](double lo, double size, double pitch) {
        return std::fmod(lo, pitch) == 0.0 || std::fmod(lo + size, pitch) == 0.0;
    };
    BOOST_TEST(onGrid(bench.cell().x, macroSize, 10.0));
    BOOST_TEST(onGrid(bench.cell().y, macroSize, 30.0));
    BOOST_TEST((bench.cell().orient == Orient::FN));
    BOOST_TEST(bench.db.verifyMacros().empty());
}

BOOST_AUTO_TEST_CASE(identical_macros_swap_places_when_that_shortens_their_nets) {
    const ScopedOption noShift("shift", OptionValue{false});
    OptionDb::instance().set(Component::Dmp, "swap", OptionValue{true});
    Bench bench(700.0);
    bench.db.setCellPosition(bench.macro, 100.0, 400.0);
    bench.plan.macros[0].originX = 100.0;
    const std::size_t other = bench.db.addCell("m2", macroSize, macroSize, true);
    bench.db.setCellPosition(other, 700.0, 400.0);
    bench.db.setCellFixed(other, true);
    const std::size_t west = bench.db.addCell("west", 0.0, 0.0, true);
    bench.db.setCellPosition(west, 0.0, 450.0);
    bench.db.setCellFixed(west, true);
    (void)bench.db.addNet("b");
    (void)bench.db.addPin("m2", "b", 50.0, 50.0, false);
    (void)bench.db.addPin("west", "b", 0.0, 0.0, true);
    bench.plan.macros.push_back({.cellId = other,
                                 .master = "block",
                                 .movable = true,
                                 .originX = 700.0,
                                 .originY = 400.0,
                                 .originOrient = Orient::N});
    bench.place();
    BOOST_TEST(bench.cell().x == 700.0);
    BOOST_TEST(bench.db.getGraph().getCell(other).x == 100.0);
    BOOST_TEST(bench.db.verifyMacros().empty());
}

BOOST_AUTO_TEST_CASE(shift_gain_keeps_a_macro_in_place_unless_the_shift_clearly_pays) {
    {
        const ScopedOption strict("shift_gain", OptionValue{0.2});
        Bench bench(50.0);
        bench.place();
        BOOST_TEST(bench.cell().x == 400.0);
        BOOST_TEST((bench.cell().orient == Orient::FN));
    }
    const ScopedOption loose("shift_gain", OptionValue{0.05});
    Bench bench(50.0);
    bench.place();
    BOOST_TEST(bench.cell().x == 450.0);
    BOOST_TEST((bench.cell().orient == Orient::FN));
}

BOOST_AUTO_TEST_CASE(the_check_reports_each_broken_rule) {
    Bench bench(50.0);
    const std::size_t blocker = bench.addBlocker(520.0, 400.0);
    bench.db.setMacroPlan(bench.plan);
    BOOST_TEST(bench.db.verifyMacros().empty());
    bench.db.setCellPosition(bench.macro, 470.0, 420.0);
    bench.db.setCellOrient(blocker, Orient::S);
    const std::vector<ktDM::Defect> defects = bench.db.verifyMacros();
    BOOST_TEST(defects.size() == 3U);
    bench.db.setCellPosition(bench.macro, 1950.0, 400.0);
    BOOST_TEST(bench.db.verifyMacros().size() == 3U);
}

BOOST_AUTO_TEST_CASE(the_mlist_reads_and_writes_back_unchanged) {
    Bench bench(0.0);
    (void)bench.addBlocker(520.0, 400.0);
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "ktplace_test_dmp_roundtrip";
    std::filesystem::create_directories(dir);
    const std::string mlist =
        "VERSION 5.7 ;\n"
        "DESIGN tiny ;\n"
        "UNITS DISTANCE MICRONS 2 ;\n\n"
        "DIEAREA ( 0 0 ) ( 1000 1000 ) ;\n\n"
        "COMPONENTS 2 ;\n"
        "   - m block \n"
        "      + PLACED ( 400 400 ) FS ;\n"
        "   - blocker block \n"
        "      + FIXED ( 520 400 ) N ;\n"
        "END COMPONENTS\n\n\n\nEND DESIGN\n\n";
    std::ofstream(dir / "tiny.mlist") << mlist;
    std::ofstream(dir / "tiny.txt") << "maximum_displacement_constraint 10\n"
                                       "minimum_channel_spacing_between_macros_constraint 1\n"
                                       "macro_halo 0\n";

    MacroPlan plan =
        readMacroPlan(bench.db, (dir / "tiny.mlist").string(), (dir / "tiny.txt").string());
    BOOST_TEST(plan.design == "tiny");
    BOOST_TEST(plan.macros.size() == 2U);
    BOOST_TEST(plan.movableCount() == 1U);
    BOOST_TEST((plan.macros[0].originOrient == Orient::FS));
    BOOST_TEST(plan.rules.maxDisplacement == 20.0);
    BOOST_TEST(plan.rules.minSpacing == 2.0);

    bench.db.setMacroPlan(std::move(plan));
    BOOST_TEST(bench.db.getGraph().getPin(0).offsetY == 50.0);
    BOOST_TEST((bench.cell().orient == Orient::FS));
    writeMacroPlan(bench.db, (dir / "tiny.dmp").string());
    BOOST_TEST(slurp(dir / "tiny.dmp") == mlist);
    std::filesystem::remove_all(dir);
}
