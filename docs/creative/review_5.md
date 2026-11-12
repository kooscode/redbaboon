# Review 5: polish round 4 on hardware

Material: `captures/hw/game_round5.mp4` (545 s, autopilot, real Xbox, own audio), serial log
`captures/serial/serial-20261111-110811.log`, master 8956ace, plus stills of the controls briefing and the controls
screen. Timestamps are **video time**. Video ≈ mission time + 20 s, after the 6 s briefing.

**Score: 8.0 / 10** (round 4: 7.5). The look is there. The intro over the wharf (0:20-0:23) is the first frame of
this game I would put on a box back. The silver zeppelin glints with its red fins and skull roundels (5:30). Palms
no longer look like dirt. Kills at close range are big orange fireballs with a "SPLASHED!" (5:25). The fixes
created two **pacing regressions**: wave 1 is now over in 11 s, and the boss in 1:24. The finale's splash is
better on paper than on screen.

## What's fixed (must not regress)
- **Intro** (0:20-0:23): 30 m over the wharf, with the steamer, cranes, pastel houses and terracotta roofs filling ~40 %
  of the frame. Then the chase climbs out along the beach with palms in the foreground (0:23). Exactly what I asked for.
- **Briefing / controls** (0:15-0:20 and the stills): a controller diagram with gold leader lines and coloured A/B/X/Y
  glyphs. "PRESS (A) TO FLY" is big, and the pause CONTROLS page has a "(B) BACK". This answers the user's complaints.
  See #6 for the small print.
- **HULL bar**: full and green when healthy (0:50). Clear.
- **Palms**: groves read as groves at 150-600 m. The dark-speck carpet is gone (0:45-1:05, 6:35-6:55).
- **Zeppelin**: silver in every boss frame, including against the sun (5:25-5:35).
- **Stunts**: 11 stunts per mission (was 39), "LOOP +10" vs "BARREL ROLL +100" under fire. It reads as a skill now.
- **Audio**: −16.9 LUFS integrated, peak −4.7 dBFS. There is no hard cut at the results screen (see #4 for the gap).
- **Frame rate**: 59.9 fps on 48 of 51 stat lines, with two single 32-33 ms frames.

## §12 detail checklist

| # | Item | Grade | Evidence |
|---|---|---|---|
| 1 | Edges/aliasing | pass | Palms and coast are clean now. |
| 2 | Z-fighting | pass | — |
| 3 | Seams | pass | — |
| 4 | Pop-in | pass | — |
| 5 | LOD/fog | pass | — |
| 6 | Shimmer | pass | Volcano ash is broken up (4:20-4:45). |
| 7 | Font/kerning | minor | The briefing sub-labels ("PITCH / TURN", "+ STICK: LOOP / ROLL / 180", "DOWN BRAKE / L-R RUDDER") are 1x 8 px text. They are unreadable on a 480i CRT through composite. |
| 8 | HUD alignment | pass | — |
| 9 | Easing | pass | — |
| 10 | Sound | minor | 1.1 s of full silence (−50 dB) at 5:54.3-5:55.5 between "VICTORY!" and the results music (#4). |
| 11 | Camera jitter | pass | — |
| 12 | Frame pacing | minor | 2 single slow frames (32/33 ms). |
| 13 | Sorting | **fail** | The zeppelin cage **shows through the sea** after it sinks: a blue wireframe under the surface for ~8 s (5:46-5:54). |
| 14 | Colour/contrast | pass | — |

## Ranked fixes

### 1. [game] The splash is too far away to be the climax
5:45-5:55: the cut to the long shot puts the impact ~1 km away. The "80 m column" is ~60 px tall and reads as a puff
of smoke. "VICTORY!" appears ~8 s after impact (5:53), not 1 s after the peak.
- Last camera: 350-450 m from the impact point, 20 m above the sea, at a 20-30° angle to the hull's path, with the
  volcano behind. The column must reach ≥ 40 % of the screen height at its peak.
- "VICTORY!" exactly 1.0 s after the column's peak. Hold 2 s more, then the results.

### 2. [render][game] The sunken cage shows through the water
The sea is alpha-blended, so the girders under it stay visible as a blue cage (5:46-5:54).
- Hide hull parts once they are 3 m below y = 0. Or fade the cage's alpha by depth over 0-6 m, so it is gone
  within 2 s of impact.
- While sinking, cover the waterline with foam billboards `F4F7F2` (12-20, 8-15 m) and a burning slick (4-6 fire
  billboards on the water for 5 s).

### 3. [game] Pacing regression: wave 1 is over in 11 s, the boss in 1:24
- **Wave 1**: the first kill is at mission 0:12 and the last at 0:17, so the wave ends at 0:25. For a new player that's
  a stampede, and for a veteran it is no fight. The first wave teaches the controls: target **45-60 s**.
  - Spawn raiders in 2 pairs. The 2nd pair arrives 20 s after the first, from another bearing, 1.5 km out.
  - Wave-1 raiders keep their low skill but **evade once** (a break turn) when the player is behind them inside 300 m.
- **Boss**: 1:24. Engines 3 and 4 died **0.5 s apart** (mission 5:08). "Last engine: aces launched" and the kill came
  3 s later, so the pulsing last engine got no time on screen.
  - Engine health ×1.6. The **last engine is armoured until both last-wave aces are down** (it pulses orange at
    2 Hz while armoured, then glows red-hot).
  - Target boss **2:00-2:30**, with each engine kill at least 20 s after the previous one.

### 4. [audio] The gap before the results
5:54.3-5:55.5: 1.1 s of full silence between the fanfare's tail and the results music. A crossfade must overlap:
start the results theme at −inf, ramping to its level over 0.8 s, **while** the outgoing sound is still in its
0.6 s release. The bed must never drop below −35 dB during a screen change.

### 5. [game] Wave 2 fights the autopilot into the canyon
Wave 2 starts at mission 0:50, but the first wave-2 kill is at 2:43. For 85 s (1:30-2:40 video) the aces chase the
player through the canyon while he flies rings. It is a good scene, but the aces never fire on him there (no hits
on HULL, no tracers in frame). When a pirate is within 400 m behind the player in the canyon, it must shoot (tracers
past the canopy, at least 2 bursts per pass). That is the canyon-chase moment the whole level is built for.

### 6. [game] Briefing small print
- The sub-labels ("PITCH / TURN", "+ STICK: LOOP / ROLL / 180", "DOWN BRAKE / L-R RUDDER") go to 2x, or are dropped.
  The big labels already say enough; put the stunt combinations on a 2nd page reached with the right trigger or
  shown once in flight.
- The pause CONTROLS still shows **"DAMAGE"** in the dimmed HUD behind it (controls_screen.png). If that still is
  from before the HULL rename, ignore this; otherwise use the same label everywhere.
- On the Duke there are also BLACK and WHITE buttons (camera, design §3). Label them, or remove the camera function.

### 7. [render] The two slow frames
32 and 33 ms, once each in a 6 minute mission. Both are in heavy explosion moments. Below a veteran's notice, but
log which effect was alive at those frames, and check that the particle cap (merged after round 4) covers them.

### 8. [content] The pirate tower is never in a shot
The design's north-east rock tower (the pirates' base, where the zeppelin comes from) is not in any frame I
sampled. Give the boss arrival a 2 s camera cut that shows the zeppelin emerging from behind the tower and its cloud
bank. One shot sells "this is their home".

### 9. [audio] Gun sound
Still thin on the spectrogram during dogfights. The guns should be the most-heard sound in the game: −22 dB RMS at the
player while firing, with a 2-6 kHz crack layer. Check it on the user's TV.

### 10. [game] Splashed planes in the harbour
Wave 3 kills over the harbour fall into open water. At least once, a downed raider should crash into the sea beside
the wharf (scripted: the last raider of wave 3), with the splash seen from the chase cam. It makes the harbour part
of the fight.

## Happy as a clam?
**Almost. Not yet, and by one item.** Of what I listed in round 4: the intro (#1), the palms (#2), the silver zeppelin
(#3) and the audio cuts (#5) are delivered, and I'm happy with all four. The boss (#4) overshot into too short. The
splash (#7) exists, but the camera shows it from a kilometre away, with the cage visible under the water. Fix items
1-3 of this review and I am happy as a clam (8.5).

The road from 8.5 to 10, unchanged: a canyon chase where the pirates really shoot, the tower as a place, guns that
sound like the main instrument, and the user's own hands on the stick for the final feel pass.
