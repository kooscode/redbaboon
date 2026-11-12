# Review 2: polish round 1 on hardware

Material: `captures/hw/game_auto7.mp4` (425 s, autopilot, real Xbox, 640x480 stretched to 16:9, own audio), serial log
`captures/serial/serial-20261111-031725.log` (the run that matches this video), `game_auto3.mp4` (audio across the boss
intro), `game_title3.mp4`. Master b8248bc. Timestamps are **video time**; mission time = video time − 16 s until the
zeppelin's slow motion. Known issues on the coordinator's list (51–58 fps dips, blank ≤64 px textures, no music under
the results, 12.8 s loading) are not re-flagged. Where they explain something I saw, I say so.

**Score: 6.5 / 10** (round 1: 5.5). This is a real step. The camera framing is right. The sea now sparkles. Kills at
200–300 m read as kills, and the boss fight has moments that look like a 2003 game (4:10, 4:20). What keeps it below
7: whenever the camera sees **land up close** it looks like 2000-era PC shareware: smooth green clay, faceted canyon
walls, pixel-stepped grass/rock edges. Several effects are big but crude: the player's fire column, the pink damage
vignette, the zeppelin turning into a wireframe all at once, hair-thin debris trails. A veteran will forgive low
polygon counts. He will not forgive things that look like debug renders.

## What's fixed (must not regress)
- **Camera** (0:25-0:55, 2:00-2:40): the plane is ~30 % of the screen width and sits under the reticle. In loops the
  camera keeps the plane in frame (3:15 underside view at 195 s). Exactly right.
- **HUD**: objective at the top centre, ace-name popups stacked cleanly ("KNUCKLES +1000 / KILL +500", 3:02). The
  off-screen arrow with its distance (2:45, 4:45, 6:45) is readable. Open-corner brackets plus "ENGINE 184M" (4:15).
- **Kills you can see**: the 4:20 kill (260 s) is a proper orange fireball at close range. Kills now lift the mix by
  ~+8 dB over the bed (RMS per 0.5 s).
- **Sea**: sun-path sparkles (0:25), turquoise shallows, smooth coastline and foam (0:19, 0:25). The round 1 saw-teeth
  are gone. The canyon water has a caustic detail texture (2:45).
- **Pacing**: wave 1 is over by 1:21, the boss arrives at 3:22, the run ends at 5:28. No dead minutes.
- **The boss's audio dropout is gone**: no silence across the boss intro in auto3 or auto7. Loudness range is now
  6.1 LU (round 1: 2.5).
- **Loading screen** with the logo, a brass bar, a propeller marker and a tip (0:12). **Results screen** exists, with a
  rank stamp (5:54).
- 60 fps everywhere except the known dips. One 27 ms frame again at the player's respawn (log 03:20:37).

## §12 detail checklist

| # | Item | Grade | Evidence |
|---|---|---|---|
| 1 | Edges and aliasing | **fail** | Grass→rock/sand blend shows in **31 m pixel stair-steps** on slopes and cliff tops (6:45 right side, 1:05 ridge, 4:20 island). Coastline: pass now. |
| 2 | Z-fighting | pass | None seen. |
| 3 | Texture seams | minor | Canyon walls still have the texture projected from the top: vertical smears on the left wall (2:05-2:40). |
| 4 | Pop-in | pass | None seen. |
| 5 | LOD/fog | minor | Far LOD tiles blur into soft mush at 300-900 m (3:02 terrain behind the kill). The fog is OK. |
| 6 | Shimmer | pass | Sea sparkles are intentional and stable. |
| 7 | Font/kerning | pass | Clean everywhere. "ENGINE DOWN! +1500 / TURRET +300" stack OK. |
| 8 | HUD alignment | minor | The radio box covers the left of the objective line when both are long (3:25: "...ES, RED! HIT THE ENGINES!" collides with the objective). The results panel is not centred on its rank stamp (5:54). |
| 9 | Animation easing | minor | The zeppelin hull vanishes in one frame to the cage (between 5:28 and 5:30). The results text pops in line by line with no ease. |
| 10 | Sound | **fail** | Integrated −21.7 LUFS (title −24.4): ~5 dB too quiet vs the −16 target. The victory fanfare is **cut dead mid-note** at 349.3 s (spectrogram: sustained tones → −65 dB in one frame). |
| 11 | Camera jitter | pass | No jitter. |
| 12 | Frame pacing | minor | Only the known dips, plus 27 ms at the respawn. |
| 13 | Sorting | pass | Smoke and clouds are OK. |
| 14 | Colour/contrast | minor | The damage vignette is **pink-violet** (`D08AB0`-ish, 1:20-1:45), not crimson. Rim light on the player's wings is so hot the leading edges glow white-orange like neon (4:45 at 285 s). |

## Ranked fixes

### 1. [content][render] Terrain up close still reads as clay
0:45, 1:05, 3:02, 6:45: smooth bright `3FAF3F` green with soft blotches, facets visible on the hills, no detail. If the
detail texture is ≤64 px, the known blank-texture bug may be part of this. Re-judge after that fix, but the
colour work below is needed either way.
- Splat/colour edges: blend grass→rock by **slope per vertex with a ±6° noise jitter** and smooth over 2 vertices, so
  there are no 31 m blocks. Rock starts at 32°, full at 42°.
- Jungle canopy: darken the base to `2F7A2E`, add per-vertex ±12 % value noise at a 50 m scale and ±6 % at 15 m. Bright
  `7BB043` only on flats below 80 m near the coast.
- The detail texture must be visible at 50 m: ±18 % luminance, tiled every 24 m, mipmapped with trilinear filtering.
- Normals: smooth the shading normals over a 3x3 sample kernel. The faceted triangles on hills (6:45, 1:05) must go.

### 2. [game][render] The player's damage effects look like a bug
1:15-1:48: "HULL CRITICAL" for 33 s with a **fire sprite column as tall as the screen** below the plane and a pink
vignette around all four edges.
- Fire on the player: 3-4 fire billboards 2.5-4 m at the engine cowling, fading over 0.4 s into smoke, and a smoke
  trail of 3→10 m billboards. The fire must never cover more than ~15 % of the plane's screen area. Below 25 % hull
  it can flare up to 6 m for 0.2 s on each hit.
- Vignette: crimson `B3202A`, alpha 0 at 70 % of the screen radius → 0.35 at the corners, pulsing 1.2 Hz only below
  25 %. No hue at mid-screen.
- Hull critical should not last 30 s while flying straight: a repair crate drop at ≤25 % from the next kill, or the
  hull slowly regenerating to 30 %. The autopilot then died in the canyon at 2:45.

### 3. [game][content] Zeppelin death: the instant wireframe
5:28→5:30: the whole hull disappears in one frame and a thin black **wireframe cage** remains (5:30, 5:35). It reads
as a debug render.
- Burn away progressively: split the hull skin into 12-16 segments that burn off outward from the last engine
  killed, one segment every 0.4 s (6 s total). Each segment shows a glowing edge (emissive `FF6A1A`) for 0.3 s
  before it goes.
- Girders: 0.6-0.8 m wide (3-4 px at 300 m), charred `2A2420` with 30 % of them glowing `C04010` near the flames.
  Leave the nose cone and fins skinned, as charred `3A332E`.
- Fins are flat grey untextured polygons (4:15, 5:45). Give them the hull panel texture, the red/black band and a
  skull roundel.

### 4. [content] Harbour and palms are still missing in action
The intro (0:19) shows a white-red lighthouse alone on an empty beach; a few black sticks are the palms. The village
from review 1 #4 is not visible anywhere in the run.
- The intro must frame the harbour: 25-40 huts with terracotta roofs, a pier with boats, palm clusters along the
  beach (at least 150 palms on the south shore), the lighthouse at the end of the mole.
- Palms: 9-14 m, 6-8 fronds in an X/star cross-billboard pattern, frond colour `2F7A2E` → `7BB043` tips, trunk `8A6A4A`.
  Visible as palms (not sticks) at 300 m.

### 5. [render] Rim light too hot on the player
4:45 (285 s), 3:15: the wing leading edges glow like neon. Cap the rim term at 0.25 for the player and 0.35 for enemies,
use colour `FFF2D0`, and multiply it by (1 − sun-facing): the rim should be visible against the sky, not on lit edges.

### 6. [audio] Overall level and the fanfare cut
- Raise the master by +5 dB (target −16 LUFS integrated over a mission, ±1). The title screen is −24 LUFS: raise it
  too. Keep the soft limiter; the peak is now −5.5 dBFS, so there is room.
- The victory fanfare must play its full length (6 s) and release over 1.5 s. Never cut a voice without a release
  (≥30 ms), anywhere.
- The zeppelin's death should be the loudest moment of the game: now it sits at −17..−22 dB RMS, the same as a fighter
  kill. Add a long low rumble (30-60 Hz, 4 s), 3 big blasts staggered 0.6 s, and creaking metal (a 200-400 Hz
  pitched-down noise sweep) while the hull folds.
- Loading: 16 s of silence (0:01-0:17). Start the title theme (or a wind bed) as soon as the logo is shown.

### 7. [game][content] Debris and smoke trails look like hair
1:05, 4:00, 4:10 (245 s), 5:30: wreck/debris trails are 1-2 px dark curved lines with dots: "spider legs".
- Debris trail: billboards 1.5 m → 6 m over 1.5 s, alpha 0.5 → 0, emitted every 0.03 s, grey `5A5552`. Only 3
  of the 6 debris chunks smoke. Lifetime 2 s max.
- Downed-plane trail: black, 3 → 12 m, with a fire core for the first 1.5 s.

### 8. [content][render] Canyon walls
2:00-2:40: straight, faceted, flat-coloured walls; the left one still smeared vertically.
- Project the wall texture horizontally (U along the wall, V = height).
- Add strata bands: alternate `7A5E4E` / `6A4F42` / `8A6E58` every 8-12 m of height, with ±2 m of noise.
- Break the straight line: displace the wall vertices ±6 m by 40 m noise. Add 10-15 fallen rocks at the waterline
  and 2-3 overhangs or arches over the water (an arch to fly under is a wow moment).

### 9. [game] Results screen
5:50-5:58: a black box over a dark blue blur, with lines popping in.
- Background: the sinking zeppelin's shot kept alive behind a 60 % ink `1A1410` panel, or the island fly-by.
- Count-up: each value rolls from 0 over 0.4 s with a tick sound, lines 0.25 s apart. The rank stamp slams down
  (scale 1.6 → 1.0, 0.15 s, plus a thud) after the score.
- Add the player's portrait and a one-line quote ("Ook ook! Isla Platano is free!").

### 10. [content][render] Zeppelin scale at range
At 766 m (4:05) the zeppelin is ~120 px wide and reads like a toy. Two fixes:
- Bring its orbit to a 900 m radius so the fight happens at 150-600 m from it.
- Fill the gondola and keel with detail (windows with warm `FFD98A` glow, catwalk, 4 visible turrets with muzzle
  flashes), and give the panel texture a ±8 % per-panel tint variation. Big things read as big through small details.

### 11. [render][content] Volcano plume is a candle flame
5:30-5:35: a thin pinkish column ~10 px wide. Use 12 billboards growing 40 → 160 m up to 700 m altitude, colour
`8A8580` → `C9C4BE`, with a downwind lean of 15°. Tint only the bottom 20 % with crater glow `FF5A1F` at alpha 0.3.

### 12. [game] Mission length
5:28 total. Even for a human, this is likely 7-8 min, below the 8-12 min target. Add a 3rd wave of 4 (2 aces) that
**attacks the harbour** after the canyon (ground targets: 3 pirate boats to sink for +300 each). It also gives the
harbour a reason to exist.

### 13. [game] Radio box and objective collide
3:25: a long radio line runs into the objective line. Limit radio text to 34 characters per line (wrap to 2 lines),
or move the objective down to y = 64 while radio is showing.

### 14. [audio] Gun/tracer sound
Too faint to judge on the spectrogram (no clear broadband bursts during the 2:50-3:05 dogfight). Check that the gun
layers reach −24 dB RMS at the player, at a 14 Hz pulse.

## Next round
Same autopilot run, after the blank-texture fix, plus stills of: the harbour from 50 m and 300 m, the canyon from
inside, the island from 1500 m, and the zeppelin's death from start to splash at 10 fps.
