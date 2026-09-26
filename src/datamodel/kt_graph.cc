/**
 * @file kt_graph.cc
 * @brief Implementation of Graph
 */

#include "datamodel/kt_graph.h"
#include <stdexcept>
#include <cstdio>
#include <algorithm>

namespace ktplace {

Graph::Graph() = default;
Graph::~Graph() = default;

std::size_t Graph::addVertex(VertexType type, const std::string &name) {
    if (nameToVertexId.find(name) != nameToVertexId.end()) {
        throw std::runtime_error("Vertex " + name + " already exists");
    }

    Vertex v;
    v.id = nextVertexId++;
    v.name = name;
    v.type = type;

    std::size_t id = v.id;
    vertices.push_back(v);
    nameToVertexId[name] = id;

    return id;
}

bool Graph::hasVertex(const std::string &name) const {
    return nameToVertexId.find(name) != nameToVertexId.end();
}

std::size_t Graph::getVertexId(const std::string &name) const {
    auto it = nameToVertexId.find(name);
    if (it == nameToVertexId.end()) {
        throw std::runtime_error("Vertex " + name + " not found");
    }
    return it->second;
}

Vertex &Graph::getVertex(std::size_t id) {
    if (id >= vertices.size() || vertices[id].id != id) {
        throw std::runtime_error("Invalid vertex ID");
    }
    return vertices[id];
}

const Vertex &Graph::getVertex(std::size_t id) const {
    if (id >= vertices.size() || vertices[id].id != id) {
        throw std::runtime_error("Invalid vertex ID");
    }
    return vertices[id];
}

VertexType Graph::getVertexType(std::size_t id) const {
    return getVertex(id).type;
}

std::size_t Graph::getNumVertices() const {
    return vertices.size();
}

std::size_t Graph::getNumVertices(VertexType type) const {
    return std::count_if(vertices.begin(), vertices.end(), [type](const Vertex &v) {
        return v.type == type;
    });
}

std::size_t Graph::addEdge(std::size_t source, std::size_t target, PinDirection direction) {
    // Validate vertices exist
    if (source >= vertices.size() || target >= vertices.size()) {
        throw std::runtime_error("Invalid vertex ID for edge");
    }

    // Validate bipartite structure: source must be Cell, target must be Net
    if (vertices[source].type != VertexType::Cell || vertices[target].type != VertexType::Net) {
        throw std::runtime_error("Edge must connect Cell (source) to Net (target)");
    }

    Edge e;
    e.id = nextEdgeId++;
    e.source = source;
    e.target = target;
    e.direction = direction;

    std::size_t id = e.id;
    edges.push_back(e);

    // Update vertex edge lists
    vertices[source].outEdges.push_back(id);
    vertices[target].inEdges.push_back(id);

    return id;
}

std::size_t Graph::addEdge(const std::string &sourceName, const std::string &targetName,
                           PinDirection direction) {
    std::size_t source = getVertexId(sourceName);
    std::size_t target = getVertexId(targetName);
    return addEdge(source, target, direction);
}

Edge &Graph::getEdge(std::size_t id) {
    if (id >= edges.size()) {
        throw std::runtime_error("Invalid edge ID");
    }
    return edges[id];
}

const Edge &Graph::getEdge(std::size_t id) const {
    if (id >= edges.size()) {
        throw std::runtime_error("Invalid edge ID");
    }
    return edges[id];
}

std::size_t Graph::getNumEdges() const {
    return edges.size();
}

const std::vector<std::size_t> &Graph::getOutEdges(std::size_t vertexId) const {
    return getVertex(vertexId).outEdges;
}

const std::vector<std::size_t> &Graph::getInEdges(std::size_t vertexId) const {
    return getVertex(vertexId).inEdges;
}

void Graph::setName(std::size_t vertexId, const std::string &name) {
    Vertex &v = getVertex(vertexId);
    if (nameToVertexId.find(name) != nameToVertexId.end() && nameToVertexId.at(name) != vertexId) {
        throw std::runtime_error("Vertex name " + name + " already exists");
    }
    nameToVertexId.erase(v.name);
    v.name = name;
    nameToVertexId[name] = vertexId;
}

const std::string &Graph::getName(std::size_t vertexId) const {
    return getVertex(vertexId).name;
}

void Graph::clear() {
    vertices.clear();
    edges.clear();
    nameToVertexId.clear();
    nextVertexId = 0;
    nextEdgeId = 0;
}

}  // namespace ktplace
