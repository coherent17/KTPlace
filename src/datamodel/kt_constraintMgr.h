// @file kt_constraintMgr.h// Placement region ("fence") constraints// Some benchmarks, notably the ISPD 2015 mixed-size designs, fence off parts// of the die and require a named group of instances to stay inside them. In// DEF that is a `REGIONS` section of rectangles plus a `GROUPS` section binding// each region to a set of instances:// REGIONS 1 ;// - er0 ( 47200 252000 ) ( 297800 300000 ) ( ... ) + TYPE FENCE ;// END REGIONS// GROUPS 1 ;// - er0 eh0/<star>// + REGION er0 ;// END GROUPS// This class owns that geometry and the instance-to-region mapping, and knows// how to answer the two questions the placer asks: which region must this cell// be in, and where is the nearest legal position for it. The per-vertex// assignment is cached in `Vertex::regionId` so the hot loops pay only an int// compare.// Per the ISPD 2015 benchmark description, a region is "one or more rectangles// specified by pairs of coordinate points (lower-left, upper-right)", and those// rectangles "might not create a contiguous rectilinear region, i.e. in some// cases a region may be disconnected". A region is therefore stored as a union// of axis-aligned rectangles: containment is a per-rectangle test and clamping// snaps to the nearest rectangle. Notably the point pairs are *not* a polygon// traversal, so treating them as one outline is incorrect.


#pragma once

#include "datamodel/kt_graph.h"
#include "geometry/kt_bbox.h"
#include "geometry/kt_point.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace ktplace {

// One placement region: a name plus the rectangles that make it up.// A region is disconnected in general, so the rectangles are kept separately// rather than merged into a single outline. The rectangles of the ISPD 2015// fences do not overlap, so the area is their sum.

struct Region {
    std::string name;           ///< DEF region name, e.g. "er0"
    std::vector<BBox> rects;    ///< union of rectangles forming the region
    double area = 0.0;          ///< total area, database units squared
    BBox bounds;                ///< bounding box, used for cheap rejection
    std::size_t cellCount = 0;  ///< instances assigned to this region

    /// @return true when (x, y) is inside any of the rectangles
    [[nodiscard]] bool contains(double x, double y) const {
        return bounds.contains(x, y) && std::ranges::any_of(rects, [&](const BBox &r) {
                   return r.contains(x, y);
               });
    }
};

// Owns the region constraints for one design.// A default-constructed manager has no regions, so the placer can keep its fast// unconstrained path.

class constraintMgr {
public:
    /// Sentinel for "not constrained to any region".
    static constexpr int noRegion = -1;

    constraintMgr() = default;

    // Add a placement region.// upper-right corners of one rectangle

    int addRegion(std::string name, const std::vector<Point> &points);

    // Assign every instance whose name starts with @p prefix to @p regionId.// DEF writes group membership as a name pattern ("eh0/<star>"), so matching is// by prefix. Unknown names are ignored, which lets a caller assign groups// before or after loading the netlist.// Matching cells are stamped with the region id in `Vertex::regionId`,// which is what the placer's hot loops read.

    std::size_t assignByPrefix(int regionId, const std::string &prefix, Graph &graph);

    /// @return number of regions
    [[nodiscard]] std::size_t getNumRegions() const {
        return regionList.size();
    }

    /// @return the region with the given id, or nullptr when out of range
    [[nodiscard]] const Region *getRegion(int id) const {
        return (id >= 0 && static_cast<std::size_t>(id) < regionList.size()) ? &regionList[id]
                                                                             : nullptr;
    }

    /// @return all regions
    [[nodiscard]] const std::vector<Region> &getRegions() const {
        return regionList;
    }

    // Is (x, y) inside the region?

    [[nodiscard]] bool contains(int id, double x, double y) const;
    [[nodiscard]] bool containsBox(int id, const BBox &box) const;
    [[nodiscard]] bool overlapsAnyRegion(const BBox &box) const;

    // Move (x, y) to the nearest position inside its region.// Inside positions are returned unchanged. Positions outside are clamped// into the nearest rectangle of the region, so a caller can use this as a// hard fence.

    void clampToRegion(int id, double &x, double &y) const;

    // Move (x, y) out of every region it happens to sit in.// A fence is reserved for the cells assigned to it, so a cell belonging to// no region has to stay out. Positions already outside every region are// returned unchanged; a position inside is moved to the nearest point of the// boundary of the smallest region rectangle containing it, which for// touching fences may take a few passes.// Fences frequently hug the die border, so an exit that leaves the die is// no use: the caller would clamp the cell straight back in. Passing the die// bounds makes the search prefer an escape that stays on-chip.

    bool pushOutOfRegions(double &x, double &y, const BBox &die) const;

    // Count instances currently outside their region.

    [[nodiscard]] std::size_t countViolations(const std::vector<double> &positions,
                                              const std::vector<int> &regionIds) const;

private:
    /// @return true when (x, y) is inside any region
    [[nodiscard]] bool isInsideAnyRegion(double x, double y) const;

    /// @return step used to land just outside a fence, scaled to the geometry
    [[nodiscard]] double getEscapeEpsilon() const;

    std::vector<Region> regionList;
    std::unordered_map<std::string, int> idByName;
};

}  // namespace ktplace
