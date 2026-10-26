#define BOOST_TEST_MODULE ktplace_geometry
#define BOOST_TEST_DYN_LINK
#include "geometry/kt_bbox.h"
#include "geometry/kt_interval.h"

#include <boost/test/unit_test.hpp>

using namespace ktplace;

BOOST_AUTO_TEST_SUITE(ktplace_geometry)

BOOST_AUTO_TEST_CASE(an_empty_box_takes_the_first_point_exactly) {
    BBox box = BBox::getEmpty();
    BOOST_TEST(box.isEmpty());
    BOOST_TEST(box.getHalfPerimeter() == 0.0);
    box.extend(3.0, -2.0);
    BOOST_TEST(!box.isEmpty());
    BOOST_TEST(box.xlo == 3.0);
    BOOST_TEST(box.xhi == 3.0);
    BOOST_TEST(box.ylo == -2.0);
    BOOST_TEST(box.yhi == -2.0);
    BOOST_TEST(box.getHalfPerimeter() == 0.0);
}

BOOST_AUTO_TEST_CASE(extending_by_points_and_boxes_grows_to_cover_them) {
    BBox box = BBox::getEmpty();
    box.extend(1.0, 5.0);
    box.extend(4.0, 2.0);
    BOOST_TEST(box.getWidth() == 3.0);
    BOOST_TEST(box.getHeight() == 3.0);
    BOOST_TEST(box.getHalfPerimeter() == 6.0);
    box.extend(BBox{-1.0, 0.0, 2.0, 10.0});
    BOOST_TEST(box.xlo == -1.0);
    BOOST_TEST(box.ylo == 0.0);
    BOOST_TEST(box.xhi == 4.0);
    BOOST_TEST(box.yhi == 10.0);
}

BOOST_AUTO_TEST_CASE(of_builds_a_box_from_corner_and_size) {
    const BBox box = BBox::of(2.0, 3.0, 4.0, 5.0);
    BOOST_TEST(box.xhi == 6.0);
    BOOST_TEST(box.yhi == 8.0);
    BOOST_TEST(box.getArea() == 20.0);
    BOOST_TEST(box.getCenter().x == 4.0);
    BOOST_TEST(box.getCenter().y == 5.5);
    BOOST_TEST(box.getX().lo == 2.0);
    BOOST_TEST(box.getY().hi == 8.0);
}

BOOST_AUTO_TEST_CASE(containment_includes_the_edges) {
    const BBox box{0.0, 0.0, 10.0, 10.0};
    BOOST_TEST(box.contains(0.0, 10.0));
    BOOST_TEST(box.contains(5.0, 5.0));
    BOOST_TEST(!box.contains(10.5, 5.0));
}

BOOST_AUTO_TEST_CASE(boxes_that_only_touch_do_not_overlap) {
    const BBox a{0.0, 0.0, 10.0, 10.0};
    BOOST_TEST(a.isOverlapping(BBox{5.0, 5.0, 15.0, 15.0}));
    BOOST_TEST(!a.isOverlapping(BBox{10.0, 0.0, 20.0, 10.0}));
    BOOST_TEST(!a.isOverlapping(BBox{0.0, 10.0, 10.0, 20.0}));
}

BOOST_AUTO_TEST_CASE(the_intersection_of_disjoint_boxes_is_empty) {
    const BBox a{0.0, 0.0, 10.0, 10.0};
    const BBox overlap = a.getIntersection(BBox{5.0, -5.0, 15.0, 5.0});
    BOOST_TEST(overlap.xlo == 5.0);
    BOOST_TEST(overlap.ylo == 0.0);
    BOOST_TEST(overlap.xhi == 10.0);
    BOOST_TEST(overlap.yhi == 5.0);
    BOOST_TEST(a.getIntersection(BBox{20.0, 20.0, 30.0, 30.0}).isEmpty());
}

BOOST_AUTO_TEST_CASE(an_interval_contains_its_ends_and_the_tolerance_widens_it) {
    const Interval span{1.0, 3.0};
    BOOST_TEST(span.getCenter() == 2.0);
    BOOST_TEST(span.contains(1.0));
    BOOST_TEST(span.contains(3.0));
    BOOST_TEST(!span.contains(3.1));
    BOOST_TEST(span.contains(3.1, 0.2));
}

BOOST_AUTO_TEST_SUITE_END()
