#pragma once
#include <cstdint>
#include <vector>

// Procedural Persian-style rug, straight RGBA.  Every knot becomes a
// knot_px x knot_px block with a little shading, which gives a woven look.
std::vector<uint8_t> generate_rug(int& width, int& height, int knots_w = 240,
                                  int knots_h = 150, int knot_px = 3);
