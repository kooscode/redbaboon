// Noise for procedural content: hashed value noise, fractal sums and cellular (Worley) noise, optionally tiling.
// Internal to src/content (docs/design.md §7).
#pragma once

#include "content/fast_math.h"

#include <stdint.h>

namespace content {

// Integer hash of a lattice point and a seed, uniform in [0, 1).
float Hash2(int x, int y, uint32_t seed);

// Smooth value noise in [0, 1). period > 0 makes it tile every period lattice cells.
float ValueNoise(float x, float y, uint32_t seed, int period = 0);

// Fractal sum of value noise octaves (each twice the frequency, gain times the amplitude), in about [0, 1].
float Fbm(float x, float y, int octaves, uint32_t seed, int period = 0, float gain = 0.5f);

// Ridged fractal noise: sharp crests where the value noise crosses 0.5, in about [0, 1].
float RidgedFbm(float x, float y, int octaves, uint32_t seed, int period = 0);

// Cellular noise: distance to the nearest (f1) and second nearest (f2) feature point, in cell units.
void Cellular(float x, float y, uint32_t seed, int period, float& f1, float& f2);

// Hermite smoothstep between edges e0 and e1 (e0 may be above e1 for a falling step).
float SmoothStep(float e0, float e1, float x);

} // namespace content
