// SPDX-License-Identifier: BSD-3-Clause

#define BOOST_TEST_MODULE ktplace_kraftwerk2
#define BOOST_TEST_DYN_LINK

#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "globalPlacer/kraftwerk2/kraftwerk2_dm.h"
#include "globalPlacer/kraftwerk2/kraftwerk2_flowMgr.h"
#include "globalPlacer/kraftwerk2/kraftwerk2_potential.h"

#include <boost/test/unit_test.hpp>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace ktplace;

namespace {

constexpr double side = 100.0;
constexpr double rowHeight = 4.0;

constexpr int meshColumns = 20;
constexpr int meshRows = 15;
constexpr double cellWidth = 2.0;

std::string getMeshCell(int c, int r) {
    return "c" + std::to_string(r * meshColumns + c);
}

void connect(ktDM &db, const std::string &net, const std::string &a, const std::string &b) {
    (void)db.addNet(net);
    (void)db.addPin(a, net, Point(1.0, 2.0), false);
    (void)db.addPin(b, net, Point(0.5, 0.5), true);
}

void makeMesh(ktDM &db) {
    for (int r = 0; r < static_cast<int>(side / rowHeight); ++r) {
        const std::size_t row = db.addRow(r * rowHeight, rowHeight, 1.0, 1.0);
        (void)db.addSubrow(row, 0.0, side);
    }
    const Point corners[4] = {Point(40.0, 40.0), Point(60.0, 40.0), Point(40.0, 60.0),
                              Point(60.0, 60.0)};
    for (int p = 0; p < 4; ++p) {
        const std::size_t pad = db.addCell("pad" + std::to_string(p), 1.0, 1.0, true);
        db.setCellPosition(pad, corners[p]);
        db.setCellFixed(pad, true);
    }
    for (int r = 0; r < meshRows; ++r) {
        for (int c = 0; c < meshColumns; ++c) {
            const std::size_t id = db.addCell(getMeshCell(c, r), cellWidth, rowHeight);
            db.setCellPosition(id, Point(side / 2.0, side / 2.0));
        }
    }
    int net = 0;
    for (int r = 0; r < meshRows; ++r) {
        for (int c = 0; c < meshColumns; ++c) {
            if (c + 1 < meshColumns) {
                connect(db, "n" + std::to_string(net++), getMeshCell(c, r), getMeshCell(c + 1, r));
            }
            if (r + 1 < meshRows) {
                connect(db, "n" + std::to_string(net++), getMeshCell(c, r), getMeshCell(c, r + 1));
            }
        }
    }
    connect(db, "pad0", getMeshCell(0, 0), "pad0");
    connect(db, "pad1", getMeshCell(meshColumns - 1, 0), "pad1");
    connect(db, "pad2", getMeshCell(0, meshRows - 1), "pad2");
    connect(db, "pad3", getMeshCell(meshColumns - 1, meshRows - 1), "pad3");
}

std::vector<Point> place(const ktDM &db) {
    kraftwerk2::flowMgr flow(db);
    flow.run();
    return flow.getSolution().positions;
}

double getOverlapShare(const std::vector<Point> &positions, std::size_t firstCell) {
    double overlap = 0.0;
    for (std::size_t a = firstCell; a < positions.size(); ++a) {
        for (std::size_t b = a + 1; b < positions.size(); ++b) {
            const double w = std::min(positions[a].x, positions[b].x) + cellWidth -
                             std::max(positions[a].x, positions[b].x);
            const double h = std::min(positions[a].y, positions[b].y) + rowHeight -
                             std::max(positions[a].y, positions[b].y);
            overlap += std::max(0.0, w) * std::max(0.0, h);
        }
    }
    return overlap / (static_cast<double>(positions.size() - firstCell) * cellWidth * rowHeight);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ktplace_kraftwerk2)

BOOST_AUTO_TEST_CASE(the_grid_size_is_rounded_up_to_a_power_of_two) {
    BOOST_TEST(kraftwerk2::getPowerOfTwoAtLeast(1) == 1u);
    BOOST_TEST(kraftwerk2::getPowerOfTwoAtLeast(33) == 64u);
    BOOST_TEST(kraftwerk2::getPowerOfTwoAtLeast(64) == 64u);
    const kraftwerk2::Potential potential(BBox(Point(0.0, 0.0), Point(10.0, 10.0)), 20);
    BOOST_TEST(potential.getBins() == 32u);
}

BOOST_AUTO_TEST_CASE(one_cosine_mode_has_the_analytic_gradient) {
    const std::size_t n = 32;
    const double length = 80.0;
    kraftwerk2::Potential potential(BBox(Point(0.0, 0.0), Point(length, length)), n);
    std::vector<double> charge(n * n);
    for (std::size_t j = 0; j < n; ++j) {
        for (std::size_t i = 0; i < n; ++i) {
            const double x = (static_cast<double>(i) + 0.5) * length / static_cast<double>(n);
            charge[j * n + i] = std::cos(std::numbers::pi * x / length);
        }
    }
    potential.solve(charge);
    for (std::size_t i = 0; i < n; i += 5) {
        const double x = (static_cast<double>(i) + 0.5) * length / static_cast<double>(n);
        const Point gradient = potential.getGradient(Point(x, length / 3.0));
        const double expected =
            -(length / std::numbers::pi) * std::sin(std::numbers::pi * x / length);
        BOOST_TEST(gradient.x == expected, boost::test_tools::tolerance(1e-9));
        BOOST_TEST(std::abs(gradient.y) < 1e-9);
    }
}

BOOST_AUTO_TEST_CASE(the_gradient_of_a_lump_points_toward_it) {
    const std::size_t n = 32;
    kraftwerk2::Potential potential(BBox(Point(0.0, 0.0), Point(32.0, 32.0)), n);
    std::vector<double> charge(n * n, -1.0 / static_cast<double>(n * n - 4));
    for (const std::size_t b : {15 * n + 15, 15 * n + 16, 16 * n + 15, 16 * n + 16}) {
        charge[b] = 0.25;
    }
    potential.solve(charge);
    const Point right = potential.getGradient(Point(24.0, 16.0));
    const Point left = potential.getGradient(Point(8.0, 16.0));
    const Point above = potential.getGradient(Point(16.0, 24.0));
    BOOST_TEST(right.x < 0.0);
    BOOST_TEST(left.x > 0.0);
    BOOST_TEST(above.y < 0.0);
    BOOST_TEST(right.x == -left.x, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(a_clump_is_spread_until_little_overlap_is_left) {
    ktDM db;
    makeMesh(db);
    kraftwerk2::flowMgr initial(db);
    initial.getDm().getOptions().maxIters = 0;
    initial.run();
    BOOST_TEST(getOverlapShare(initial.getSolution().positions, 4) > 1.0);
    const std::vector<Point> positions = place(db);
    BOOST_TEST(getOverlapShare(positions, 4) < 0.5);
    for (std::size_t i = 4; i < positions.size(); ++i) {
        BOOST_TEST(positions[i].x >= -1e-9);
        BOOST_TEST(positions[i].x + cellWidth <= side + 1e-9);
        BOOST_TEST(positions[i].y >= -1e-9);
        BOOST_TEST(positions[i].y + rowHeight <= side + 1e-9);
    }
}

BOOST_AUTO_TEST_CASE(placement_is_deterministic) {
    ktDM db;
    makeMesh(db);
    const std::vector<Point> first = place(db);
    const std::vector<Point> second = place(db);
    BOOST_TEST(first == second);
}

BOOST_AUTO_TEST_CASE(frames_follow_trace_every) {
    ktDM db;
    makeMesh(db);
    const auto framesOffered = [&db] {
        kraftwerk2::kraftwerk2DM dm = kraftwerk2::buildKraftwerk2DM(db);
        dm.getOptions().traceEvery = 0;
        std::size_t frames = 0;
        dm.setFrameHook([&frames](const std::vector<float> &, const std::vector<float> &,
                                  const kraftwerk2::FrameInfo &) {
            ++frames;
        });
        dm.run();
        return frames;
    };
    BOOST_TEST(framesOffered() == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
