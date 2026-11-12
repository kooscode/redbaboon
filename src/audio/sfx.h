// The sound-effect bank and the engine loops, synthesised at start-up (sfx.cpp, docs/design.md §6).
#pragma once

#include "audio/audio.h"
#include "audio/synth.h"

namespace audio {

// Most variants one effect has.
constexpr int MAX_SFX_VARIANTS = 6;

// Engine loop layers: LOW is the sound at idle throttle, HIGH at full throttle (same length, played at the same
// position and cross-faded by throttle).
enum class EngineLayer {
    LOW,
    HIGH,
};

// Renders the menu sounds (once).
void BuildMenuSounds();

// Renders every effect and engine loop (once; calls BuildMenuSounds first). May run on a thread other than the
// mixer's and the game's: use an effect only when SfxReady says so, the engine loops when EnginesReady does.
void BuildSfxBank();

bool SfxReady(Sfx sfx);
bool EnginesReady();

// Number of variants of an effect (at least 1 once the bank is built).
int SfxVariantCount(Sfx sfx);

const Sample& SfxVariant(Sfx sfx, int variant);

// The looping drone of an engine type.
const Sample& EngineLoop(EngineType type, EngineLayer layer);

// The supercharger whine loop (a tone at WHINE_BASE_HZ when played at its own rate).
const Sample& WhineLoop();

// Pitch of the whine loop at its own rate.
constexpr float WHINE_BASE_HZ = 1000.0f;

// The rushing-air loop for close fly-bys.
const Sample& WindLoop();

// Bytes held by the whole bank.
uint32_t SfxBankBytes();

} // namespace audio
