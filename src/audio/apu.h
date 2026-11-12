// Output through the MCPX APU (audio_apu.cpp), the path xemu plays to the host: one looping stereo voice whose buffer
// the device fills from the mixer. Used only where the AC'97 output is not heard (xemu); see audio_xbox.cpp.
#pragma once

#include <stdint.h>

namespace audio {

// Frames in the APU voice's looping buffer.
constexpr int APU_RING_FRAMES = 4096;

// Sets up and starts the voice over a silent buffer. False when memory could not be had.
bool ApuOpen();

// Frames written and not yet played. When the voice has run past the written frames, writing carries on from its
// position and this returns 0.
int ApuQueuedFrames();

// How many times the voice had run past the written frames.
int ApuCatchUps();

// Appends `frames` stereo frames (interleaved S16) after the last written ones.
void ApuWrite(const int16_t* data, int frames);

// The voice's play position (frame in the ring), for the diagnostics line.
uint32_t ApuPlayPosition();

} // namespace audio
