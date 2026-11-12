// Music: synthesised band instruments and the tracks, compiled at start-up into note lists the mixer's sequencer
// plays (music.cpp, docs/design.md §6).
#pragma once

#include "audio/audio.h"
#include "audio/synth.h"

namespace audio {

// Sequencer resolution: ticks per quarter note (divisible by 3 for swing and triplets).
constexpr int TICKS_PER_BEAT = 24;

// Channels per song: a new note on a channel releases the note playing on it.
constexpr int MUSIC_CHANNELS = 18;

// One note: when it starts, on which channel, which sample at which rate and level, and when it is released.
struct NoteEvent {
    uint32_t tick;
    uint32_t lengthTicks;      // released after this many ticks; 0: plays to the end of the sample
    const Sample* sample;
    uint32_t step;             // source frames per output frame, 16.16 fixed point
    uint16_t gainL;            // 0..32767
    uint16_t gainR;
    uint32_t releaseFrames;    // fade-out length at note-off (output frames)
    uint8_t channel;
};

// A compiled track.
struct Song {
    std::vector<NoteEvent> events;     // sorted by tick
    uint32_t lengthTicks = 0;
    uint32_t loopTick = 0;             // where a looping song restarts
    uint32_t loopEvent = 0;            // first event at or after loopTick
    bool loops = false;
    float bpm = 120.0f;
};

// Renders what the TITLE track needs (every instrument but the trombones, the drums) and compiles it. Once only.
void BuildTitleMusic();

// Renders the rest and compiles every other track (calls BuildTitleMusic first). Once only; may run on a thread
// other than the mixer's: each track becomes playable when SongReady says so.
void BuildMusic();

// True when a track's samples and notes are complete (always for NONE).
bool SongReady(Music music);

// The compiled track (an empty song for Music::NONE).
const Song& GetSong(Music music);

// Bytes held by the instrument samples.
uint32_t MusicBankBytes();

// Problems found in the track data (a part whose length does not match its chords), or "" when there are none.
const char* MusicDataErrors();

} // namespace audio
