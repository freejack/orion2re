#pragma once

#include <cstdint>
#include <vector>

struct s_animation_header;

// LOOK TEST (local patch, not upstream): combat ships drawn live from Gaussian splats. See splat_ship.cpp.
namespace splat_ship {
    // True when a splat model is loaded and this combat ship is a player hull it stands in for.
    bool Covers(int16_t ship_idx);
    // Draws the ship into `dest` as a palette bitmap at any orientation, all in radians: `heading` is
    // counter-clockwise on the map from pointing right, `roll` is about the nose axis and `pitch` lifts the nose.
    // Returns false when the ship is not covered.
    bool Draw(int16_t ship_idx, int16_t engine_step, float heading, float roll, float pitch, s_animation_header* dest);
    // Draws the ship for a free camera. `camera` is the camera's axes in the world (screen right, screen up, toward
    // the eye; X right, Y up, Z toward the bottom of the flat field) and `zoom` scales the ship. Fills `pixels` with a
    // square palette bitmap `size` across, centred on the ship.
    bool Render_View(int16_t ship_idx, int16_t engine_step, float heading, float roll, float pitch, const float camera[3][3], float zoom, std::vector<uint8_t>* pixels, int32_t* size);
} // namespace splat_ship
