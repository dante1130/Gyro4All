#pragma once
// Naughty Dog camera control views (nd_engine.cpp): the on-foot pitch setting's table and the heading turn. No game
// memory access, unit-tested.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace gyro4all::naughtydog {

// The view's pitch (degrees) at CameraControlStrafe's pitch setting 0.0, 0.1, ... 1.0 (measured in game on the active
// control; 0 looks up, 1 down, not linear).
inline constexpr std::array<double, 11> kStrafePitchDegrees = {59.9, 38.1, 23.5, 12.7, 3.6, -5.5, -17.7, -31.8, -46.7, -59.8, -68.1};

// The view's pitch at a setting (clamped to 0..1).
inline double strafe_degrees(double setting) {
    const double at = std::clamp(setting, 0.0, 1.0) * 10;
    const size_t i = std::min<size_t>(static_cast<size_t>(at), kStrafePitchDegrees.size() - 2);
    return kStrafePitchDegrees[i] + (at - static_cast<double>(i)) * (kStrafePitchDegrees[i + 1] - kStrafePitchDegrees[i]);
}

// The setting for a view pitch (clamped to the table's range).
inline double strafe_setting(double degrees) {
    if (degrees >= kStrafePitchDegrees.front()) return 0;
    for (size_t i = 0; i + 1 < kStrafePitchDegrees.size(); ++i) {
        const double hi = kStrafePitchDegrees[i], lo = kStrafePitchDegrees[i + 1];
        if (degrees >= lo) return (static_cast<double>(i) + (hi - degrees) / (hi - lo)) / 10;
    }
    return 1;
}

// Turns a direction about the vertical (Y) so that its heading atan2(x, z) changes by `degrees`.
inline void turn_heading(float& x, float& z, double degrees) {
    const double a = degrees * 3.14159265358979323846 / 180, c = std::cos(a), s = std::sin(a);
    const double nx = c * x + s * z, nz = -s * x + c * z;
    x = static_cast<float>(nx);
    z = static_cast<float>(nz);
}

}  // namespace gyro4all::naughtydog
