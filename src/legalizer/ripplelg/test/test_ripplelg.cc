// SPDX-License-Identifier: BSD-3-Clause

#define BOOST_TEST_MODULE ktplace_ripplelg
#define BOOST_TEST_DYN_LINK

#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "legalizer/ripplelg/ripplelg_dm.h"
#include "legalizer/ripplelg/ripplelg_flow.h"
#include "legalizer/ripplelg/ripplelg_flowMgr.h"

#include <boost/test/unit_test.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ktplace;

namespace {

constexpr double rowHeight = 10.0;
constexpr int rowCount = 10;
constexpr double rowWidth = 120.0;

void makeMixedClump(ktDM &db) {
    for (int r = 0; r < rowCount; ++r) {
        const std::size_t row = db.addRow(r * rowHeight, rowHeight, 1.0, 1.0);
        (void)db.addSubrow(row, 0.0, rowWidth);
    }
    std::uint64_t state = 99;
    const auto next = [&state] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 40U) / static_cast<double>(1ULL << 24U);
    };
    for (int i = 0; i < 160; ++i) {
        const bool tall = i % 5 == 0;
        const std::size_t id = db.addCell("c" + std::to_string(i), tall ? 2.0 : 3.0,
                                          tall ? 2.0 * rowHeight : rowHeight);
        db.setCellPosition(id, Point(40.0 + 30.0 * next(), 30.0 + 30.0 * next()));
    }
    for (int i = 0; i < 160; ++i) {
        const std::string net = "n" + std::to_string(i);
        (void)db.addNet(net);
        (void)db.addPin("c" + std::to_string(i), net, Point(0.5, 0.5), false);
        (void)db.addPin("c" + std::to_string((i * 37 + 11) % 160), net, Point(0.5, 0.5), true);
    }
}

void legalize(ktDM &db) {
    ripplelg::flowMgr flow(db);
    flow.run();
    db.setRipplelgSolution(flow.getSolution());
}

std::size_t getOverlaps(const ktDM &db) {
    std::size_t overlaps = 0;
    for (std::size_t a = 0; a < db.getNumCells(); ++a) {
        for (std::size_t b = a + 1; b < db.getNumCells(); ++b) {
            const Vertex &ca = db.getGraph().getCell(a);
            const Vertex &cb = db.getGraph().getCell(b);
            const Point pa = db.getCellPosition(a);
            const Point pb = db.getCellPosition(b);
            const bool apart = pa.x + ca.width <= pb.x + 1e-9 || pb.x + cb.width <= pa.x + 1e-9 ||
                               pa.y + ca.height <= pb.y + 1e-9 || pb.y + cb.height <= pa.y + 1e-9;
            overlaps += apart ? 0 : 1;
        }
    }
    return overlaps;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ktplace_ripplelg)

BOOST_AUTO_TEST_CASE(network_simplex_finds_the_cheapest_circulation) {
    ripplelg::NetworkSimplex flow(3);
    flow.addArc(0, 1, 3.0, -2.0);
    flow.addArc(1, 2, ripplelg::unlimited, 0.5);
    flow.addArc(2, 0, 2.0, 0.5);
    flow.addArc(1, 0, ripplelg::unlimited, 1.5);
    BOOST_TEST(flow.solve(1000));
    BOOST_TEST(flow.getCost() == -2.5, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(network_simplex_potentials_solve_a_fixed_order_row) {
    ripplelg::NetworkSimplex flow(3);
    const double wanted[2] = {5.0, 5.0};
    for (int c = 0; c < 2; ++c) {
        flow.addArc(static_cast<std::size_t>(c + 1), 0, 1.0, wanted[c]);
        flow.addArc(0, static_cast<std::size_t>(c + 1), 1.0, -wanted[c]);
    }
    flow.addArc(0, 1, ripplelg::unlimited, -0.0);
    flow.addArc(1, 2, ripplelg::unlimited, -4.0);
    flow.addArc(2, 0, ripplelg::unlimited, 16.0);
    BOOST_TEST(flow.solve(1000));
    const std::vector<double> &p = flow.getPotentials();
    const double x1 = p[0] - p[1];
    const double x2 = p[0] - p[2];
    BOOST_TEST(x2 - x1 >= 4.0 - 1e-9);
    BOOST_TEST(std::abs(x1 - 5.0) + std::abs(x2 - 5.0) == 4.0, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(a_mixed_height_clump_becomes_legal) {
    ktDM db;
    makeMixedClump(db);
    legalize(db);
    BOOST_TEST(getOverlaps(db) == 0u);
    BOOST_TEST(db.getDefects().empty());
    for (std::size_t c = 0; c < db.getNumCells(); ++c) {
        const Point p = db.getCellPosition(c);
        const double row = p.y / rowHeight;
        BOOST_TEST(std::abs(row - std::round(row)) < 1e-9);
        BOOST_TEST(std::abs(p.x - std::round(p.x)) < 1e-9);
        if (db.getGraph().getCell(c).height > 1.5 * rowHeight) {
            BOOST_TEST(static_cast<long>(std::round(row)) % 2 == 0,
                       "double-height cell " << c << " off the P/G parity");
        }
    }
}

BOOST_AUTO_TEST_CASE(legalization_is_deterministic) {
    ktDM first;
    makeMixedClump(first);
    legalize(first);
    ktDM second;
    makeMixedClump(second);
    legalize(second);
    for (std::size_t c = 0; c < first.getNumCells(); ++c) {
        BOOST_TEST((first.getCellPosition(c) == second.getCellPosition(c)));
    }
}

BOOST_AUTO_TEST_CASE(every_stage_offers_a_frame) {
    ktDM db;
    makeMixedClump(db);
    ripplelg::ripplelgDM dm = ripplelg::buildRipplelgDM(db);
    dm.getOptions().frames = 0;
    std::size_t frames = 0;
    dm.setFrameHook([&frames](const std::vector<float> &, const std::vector<float> &,
                              const ripplelg::FrameInfo &) {
        ++frames;
    });
    dm.run();
    BOOST_TEST(frames == 4u);
}

BOOST_AUTO_TEST_SUITE_END()
