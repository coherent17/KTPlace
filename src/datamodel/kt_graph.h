/**
 * @file kt_graph.h
 * @brief Graph data structure for placement netlist
 */

#ifndef KT_GRAPH_H
#define KT_GRAPH_H

#include <cstddef>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>

namespace ktplace {
namespace core {

/**
 * @brief Vertex types in the placement graph
 */
enum class VertexType { Cell, Net };

/**
 * @brief Edge (Pin) direction
 */
enum class PinDirection { Input, Output };

/**
 * @brief Graph vertex data
 */
class Vertex {
public:
    std::size_t id = 0;
    std::string name;
    VertexType type = VertexType::Cell;

    // Cell-specific properties
    double width = 0.0;
    double height = 0.0;
    double x = 0.0;
    double y = 0.0;
    bool isTerminal = false;
    bool isFixed = false;

    // Net-specific properties
    double weight = 1.0;

    // Connected edges
    std::vector<std::size_t> outEdges;
    std::vector<std::size_t> inEdges;
};

/**
 * @brief Graph edge (Pin) data
 */
class Edge {
public:
    std::size_t id = 0;
    std::size_t source = 0;  // Cell vertex ID
    std::size_t target = 0;  // Net vertex ID
    PinDirection direction = PinDirection::Input;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

/**
 * @brief Graph structure for placement netlist
 * 
 * Represents the placement netlist as a directed bipartite graph:
 * - Cell vertices: represent circuit cells
 * - Net vertices: represent nets (connections)
 * - Edges: represent pins connecting cells to nets
 */
class Graph {
public:
    /// Constructor
    Graph();

    /// Destructor
    ~Graph();

    // Vertex operations
    std::size_t addVertex(VertexType type, const std::string &name);
    bool hasVertex(const std::string &name) const;
    std::size_t getVertexId(const std::string &name) const;
    Vertex &getVertex(std::size_t id);
    const Vertex &getVertex(std::size_t id) const;
    VertexType getVertexType(std::size_t id) const;
    std::size_t getNumVertices() const;
    std::size_t getNumVertices(VertexType type) const;

    // Edge operations
    std::size_t addEdge(std::size_t source, std::size_t target,
                        PinDirection direction = PinDirection::Input);
    std::size_t addEdge(const std::string &sourceName, const std::string &targetName,
                        PinDirection direction = PinDirection::Input);
    Edge &getEdge(std::size_t id);
    const Edge &getEdge(std::size_t id) const;
    std::size_t getNumEdges() const;

    // Access edges by vertex
    const std::vector<std::size_t> &getOutEdges(std::size_t vertexId) const;
    const std::vector<std::size_t> &getInEdges(std::size_t vertexId) const;

    // Name lookups
    void setName(std::size_t vertexId, const std::string &name);
    const std::string &getName(std::size_t vertexId) const;

    // Clear all data
    void clear();

private:
    std::vector<Vertex> vertices;
    std::vector<Edge> edges;
    std::unordered_map<std::string, std::size_t> nameToVertexId;

    std::size_t nextVertexId = 0;
    std::size_t nextEdgeId = 0;
};

}  // namespace core
}  // namespace ktplace

#endif  // KT_GRAPH_H
