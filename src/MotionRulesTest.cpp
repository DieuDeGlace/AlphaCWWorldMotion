#include "MotionRules.h"
#include <cassert>
#include <cstdio>
int main() {
    auto leaf = [](int, int, int) { return 8; };
    assert(motion::corner(8, 8, 20, leaf) == 1);
    assert(motion::corner(0, 8, 20, leaf) == 0);
    assert(motion::corner(32, 8, 20, leaf) == 0);
    assert(motion::corner(8, 32, 20, leaf) == 0);
    auto wood = [](int x, int y, int z) { return x == 7 && y == 7 && z == 19 ? 7 : 8; };
    assert(motion::corner(8, 8, 20, wood) == 0);
    assert(motion::corner(9, 8, 20, wood) == 1);
    auto rock = [](int, int, int) { return 6; };
    auto unknown = [](int, int, int) { return -1; };
    assert(motion::corner(8, 8, 20, rock) == 0);
    assert(motion::corner(8, 8, 20, unknown) == 0);
    auto road = [](int, int, int) { return 11; };
    assert(motion::corner(8, 8, 20, road) == 0);
    auto roadContact = [](int x, int y, int z) { return x == 7 && y == 7 && z == 19 ? 11 : 8; };
    assert(motion::corner(8, 8, 20, roadContact) == 0);
    // Independently admitted neighboring chunks stay joined even at different fades.
    assert(motion::corner(32, 8, 20, leaf) * 1.f == motion::corner(0, 8, 20, leaf) * .25f);
    for (int t = 0; t < 100000; t += 17) {
        auto a = motion::leaves(t, .12f), b = motion::clouds(t, 32);
        assert(std::abs(a.x) <= .120001f && std::abs(a.y) <= .078001f);
        assert(std::abs(b.x) <= 32.0001f && std::abs(b.y) <= 12.8001f);
    }
    puts(
        "PASS: wood/rock/unknown anchors, chunk-boundary pins, leaf materials, bounded wind/cloud motion.");
}
