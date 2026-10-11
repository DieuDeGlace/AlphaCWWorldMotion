#pragma once
#include <algorithm>
#include <cmath>

namespace motion {
struct Offset {
    float x, y;
};
inline bool foliage(int type) {
    return type == 8 || type == 11;
}
inline bool stationary(int type) {
    return type < 0 || (type != 0 && !foliage(type));
}
inline Offset leaves(double seconds, float amplitude) {
    return {amplitude * float(.7 * std::sin(seconds * 1.7) + .3 * std::sin(seconds * 3.1)),
            amplitude * .65f * float(std::sin(seconds * 1.35 + .8))};
}
inline Offset clouds(double seconds, float distance) {
    return {distance * float(std::sin(seconds * .012)),
            distance * .4f * float(std::sin(seconds * .009))};
}
// Pin chunk boundaries as well as stationary contacts. Independent chunk admission,
// distance fading and unloading cannot pull a shared boundary corner apart.
template <class Sample> float corner(int x, int y, int z, Sample sample) {
    if (x <= 0 || y <= 0 || x >= 32 || y >= 32)
        return 0;
    bool leaf = false;
    for (int dx = -1; dx <= 0; ++dx)
        for (int dy = -1; dy <= 0; ++dy)
            for (int dz = -1; dz <= 0; ++dz) {
                int type = sample(x + dx, y + dy, z + dz);
                if (stationary(type))
                    return 0;
                leaf |= foliage(type);
            }
    return leaf ? std::clamp(float(std::min({x, y, 32 - x, 32 - y})) / 3.f, 0.f, 1.f) : 0;
}
} // namespace motion
