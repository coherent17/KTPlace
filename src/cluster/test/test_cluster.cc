#define BOOST_TEST_MODULE ktplace_cluster
#define BOOST_TEST_DYN_LINK
#include "cluster/kt_cluster.h"
#include "cluster/kt_clusterOptions.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_graph.h"

#include <boost/test/unit_test.hpp>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace ktplace;

namespace {

constexpr double rowHeight = 10.0;

void addCell(ktDM &db, const std::string &name, double x, double y, double width = 2.0) {
    const std::size_t id = db.addCell(name, width, rowHeight);
    db.setCellPosition(id, x, y);
}

void connect(ktDM &db, const std::string &net, const std::vector<std::string> &cells) {
    (void)db.addNet(net);
    for (const std::string &cell : cells) {
        (void)db.addPin(cell, net, 0.5, 0.5, cell == cells.front());
    }
}

void buildTwoGroups(ktDM &db) {
    for (int r = 0; r < 4; ++r) {
        const std::size_t row = db.addRow(r * rowHeight, rowHeight, 1.0, 1.0);
        (void)db.addSubrow(row, 0.0, 100.0);
    }
    db.setDieArea(0.0, 0.0, 100.0, 40.0);
    for (int i = 0; i < 4; ++i) {
        addCell(db, "a" + std::to_string(i), 10.0 * i, 0.0);
        addCell(db, "b" + std::to_string(i), 10.0 * i, 20.0);
    }
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            connect(db, "na" + std::to_string(i) + std::to_string(j),
                    {"a" + std::to_string(i), "a" + std::to_string(j)});
            connect(db, "nb" + std::to_string(i) + std::to_string(j),
                    {"b" + std::to_string(i), "b" + std::to_string(j)});
        }
    }
    connect(db, "bridge", {"a3", "b0"});
}

ClusterOptions options(const std::string &algorithm) {
    ClusterOptions opts;
    opts.algorithm = algorithm;
    opts.cellsPerCluster = 4.0;
    opts.areaLimit = 1.0;
    return opts;
}

std::size_t cellId(const std::string &name) {
    return std::stoul(name.substr(1)) * 2 + (name[0] == 'b' ? 1 : 0);
}

std::uint32_t clusterOfCell(const ktCluster &clusters, const std::string &name) {
    return clusters.clusterOf().at(cellId(name));
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ktplace_cluster)

BOOST_AUTO_TEST_CASE(two_dense_groups_become_two_clusters) {
    for (const char *algorithm : {"leiden", "louvain"}) {
        ktDM db;
        buildTwoGroups(db);
        ktCluster clusters(options(algorithm));
        const ClusterStats stats = clusters.cluster(db);
        BOOST_TEST(stats.clusters == 2u, algorithm);
        BOOST_TEST(db.getGraph().getNumCells() == 2u, algorithm);
        for (const char *a : {"a1", "a2", "a3"}) {
            BOOST_TEST(clusterOfCell(clusters, a) == clusterOfCell(clusters, "a0"), a);
        }
        for (const char *b : {"b1", "b2", "b3"}) {
            BOOST_TEST(clusterOfCell(clusters, b) == clusterOfCell(clusters, "b0"), b);
        }
        BOOST_TEST(clusterOfCell(clusters, "a0") != clusterOfCell(clusters, "b0"));
    }
}

BOOST_AUTO_TEST_CASE(declustering_restores_the_netlist_and_packs_each_cluster_in_its_box) {
    ktDM db;
    buildTwoGroups(db);
    const std::size_t pins = db.getNumPins();
    const std::size_t nets = db.getNumNets();
    ktCluster clusters(options("leiden"));
    (void)clusters.cluster(db);
    BOOST_TEST(clusters.clustered());
    for (std::size_t v = 0; v < db.getGraph().getNumCells(); ++v) {
        db.setCellPosition(v, 30.0 + 40.0 * static_cast<double>(v), 10.0);
    }
    clusters.decluster(db);
    BOOST_TEST(!clusters.clustered());
    BOOST_TEST(db.getGraph().getNumCells() == 8u);
    BOOST_TEST(db.getNumPins() == pins);
    BOOST_TEST(db.getNumNets() == nets);
    BOOST_TEST(db.getGraph().getPin(0).offsetX == 0.5);

    for (const char *group : {"a", "b"}) {
        std::set<double> xs;
        double y = -1.0;
        for (int i = 0; i < 4; ++i) {
            const auto [x, cy] = db.getCellPosition(std::string(group) + std::to_string(i));
            xs.insert(x);
            y = y < 0.0 ? cy : y;
            BOOST_TEST(cy == y);
        }
        BOOST_TEST(xs.size() == 4u);
        BOOST_TEST(*xs.rbegin() - *xs.begin() == 6.0);
    }
}

BOOST_AUTO_TEST_CASE(fixed_cells_and_fence_regions_are_never_merged) {
    ktDM db;
    buildTwoGroups(db);
    db.setCellFixed("a0", true);
    db.getGraph().getCell(cellId("b1")).regionId = 3;
    ktCluster clusters(options("leiden"));
    (void)clusters.cluster(db);
    BOOST_TEST(clusterOfCell(clusters, "a0") != clusterOfCell(clusters, "a1"));
    BOOST_TEST(clusterOfCell(clusters, "b1") != clusterOfCell(clusters, "b0"));
    BOOST_TEST(clusterOfCell(clusters, "b1") != clusterOfCell(clusters, "b2"));
}

BOOST_AUTO_TEST_CASE(clustering_is_repeatable) {
    std::vector<std::uint32_t> first;
    for (int run = 0; run < 2; ++run) {
        ktDM db;
        buildTwoGroups(db);
        ktCluster clusters(options("leiden"));
        (void)clusters.cluster(db);
        if (run == 0) {
            first = clusters.clusterOf();
        } else {
            BOOST_TEST(clusters.clusterOf() == first, boost::test_tools::per_element());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
