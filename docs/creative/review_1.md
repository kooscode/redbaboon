# Review 1: first full build on hardware

Material: `captures/hw/game_auto2.mp4` (485 s, autopilot, real Xbox, 640x480 stretched to 16:9), `game_title2.mp4`,
serial log `captures/serial/serial-20261111-013344.log`. Timestamps below are **video time**. Mission time = video time
- 14 s until the zeppelin's slow-motion. Autoplay skips the results screen, so that screen is not reviewed.

**Score: 5.5 / 10.** The frame is clean, it holds 60 fps, and some moments already look like a real Xbox game: the
burning zeppelin frame, lens flare, the cumulus banks, the brass gauges. As a *game* it fails the veteran test
because **you cannot see what you shoot or what you kill**. Enemies are a few pixels hidden under a red bullseye, and a
kill is a text banner with no visible explosion. The terrain also reads as untextured green clay, and the props are
red cubes. Right now this is a well-made tech demo with a good HUD, not a 2003 exclusive.

## What works (must not regress)
- Locked 60 fps for the whole run: 43 of 44 stat lines are at 18-19 ms worst frame, 25-82k triangles.
- Lens flare ghosts and sun glow (5:10). The sun-side warm horizon `F3DCAE`-ish band (0:40, 2:25).
- Cumulus banks (5:00-5:05): shaded, layered, great sense of altitude. Keep.
- The zeppelin's hull burning away to show the girders (5:50-6:10). This is the signature shot. Keep and build it up.
- Sea colour and haze toward the horizon (0:20, 3:45). Lighthouse silhouette on its islet (3:45).
- Brass dial gauges, the radar ring, radio box with portrait, the "SPLASHED!" banner style, the title logo (title
  video 0:20+) and the title fly-by through the canyon.
- Wingtip vapour streaks (2:20, 5:10). The crimson livery with cream tips and roundels reads well.
- Radio script tone ("Keep her in the air, you apes!") is right.

## §12 detail checklist

| # | Item | Grade | Evidence |
|---|---|---|---|
| 1 | Edges and aliasing | **fail** | Coastlines are stair-stepped saw teeth following the 31 m terrain grid (5:05 right side, 6:20-6:25 foam ring as white triangles). Stunt ring edges crawl (1:20). Wing edges are acceptable. |
| 2 | Z-fighting | pass | None seen on roofs, rings or the zeppelin. |
| 3 | Texture seams | minor | Canyon walls: a texture projected from the top, stretched into vertical streaks (1:30-2:00 left wall). No tile seams seen. |
| 4 | Pop-in | pass | Nothing popped in the sampled frames. Recheck with a human pilot at speed. |
| 5 | LOD/fog transitions | minor | The fog matches the sky toward the sun. Away from the sun the far sea is darker than the horizon (3:45 left). |
| 6 | Shimmer | minor | Jungle/terrain shows blotchy dark-green noise that swims at low altitude (6:05-6:15). Look at the 960 px frames: no detail texture visible at all. |
| 7 | Font and kerning | minor | The deco font is good and the shadow is right. But stacked popups overprint each other: "ALL RINGS +2000 / RING +250" (2:20) and "KILL +500 / TURRET +300" (4:53) collide. |
| 8 | HUD alignment | **fail** | The objective line sits at y≈400 of 480, right across the player's wings (every frame, e.g. 0:40, 5:05). The off-screen target chevron is a tiny red tick lost near the gauges (5:00-5:05). The target bracket is a filled bullseye that **covers the target** (3:45, 3:44 burst). |
| 9 | Animation easing | minor | Popups fade nicely. The camera loses the plane in loops: the plane slides half off the bottom of the screen for ~1 s (2:20 burst, 2:25, 4:05, 5:35). |
| 10 | Sound clipping/repetition | minor | Peak −4.6 dBFS, no clipping. But there is a **hard dropout of ~100 ms at 4:28.3** (−61/−71 dB). Loudness range is only 2.5 LU: a flat wall of sound. |
| 11 | Camera jitter | pass | 30 fps bursts (2:20, 3:44, 5:00) show steady framing, no jitter. |
| 12 | Frame pacing | minor | One 27 ms frame at the player's death (log 01:36:52, video ≈2:42). Everything else is ≤19 ms. |
| 13 | Sorting | pass | Clouds and smoke sort correctly. Smoke billboards clip hard into the terrain where the zeppelin crashed (6:20). |
| 14 | Colour/contrast | **fail** | The shadow side of the canyon is near-black `1A2A1A` (1:30-2:05). The props are unlit red cubes on green (2:25). At combat range enemies are invisible (3:44). |

## Ranked fixes (highest impact first)

### 1. You must see what you shoot (game + render + content)
At 3:44-3:46 the ace kill happens at ~500 m: the enemy is 3-4 px wide, the red bullseye bracket hides it, there are
no visible tracers, and the kill shows **no visible explosion**, only the banner.
- **game/weapons**: cut the bullet life so the effective range is 450 m (600 m/s + plane speed, life 0.65 s).
  **game/ai**: aces and fighters fight at 80-350 m: they turn in, they do not snipe or extend beyond 600 m.
- **game/hud**: replace the filled bullseye with **4 open corner brackets** 6 px outside the target's projected
  bounding box (min 14x14 px), 2 px lines, crimson `B3202A`, distance text *below* the brackets. Nothing ever draws
  on top of the target's pixels. The lead reticle is a hollow 10 px circle.
- **render/content**: enemies get a rim term (fresnel 0.35, colour `FFF2D0`) and the pirate livery's hazard-yellow
  `E8B923` must cover ≥30 % of the top surface. They should be readable as shapes at 300 m.
- **Tracers**: 10 m streaks, 0.35 m wide, additive, core `FFFBE0`. Every *second* bullet makes a streak, so streams
  are visible from the chase cam. Enemy tracers `FF5A3C`.
- **Kill explosion** (game/effects): fireball scale 18 m at the start, growing to 30 m over 0.6 s. Flash billboard
  50 m for 2 frames. 6 tumbling debris chunks with smoke trails for 2-3 s. A burning wreck falls with a fire trail
  (the plane mesh with emissive `FF6A1A` at 0.6 and black smoke) all the way to impact, then a ground/sea explosion
  and a splash column. At 500 m that is 30-40 px: visible.

### 2. Camera too close, plane too big (game/camera.cpp) — my brief's number was wrong
With `CHASE_BACK 13` and `FOV_CRUISE 58`, the 11 m wingspan fills ~57 % of the screen width (0:40). It hides the
reticle area and the targets.
- `CHASE_BACK = 21`, `CHASE_UP = 5.0`, `LOOK_AHEAD = 60`. Aim for a wingspan of 32-36 % of the screen width, with the
  plane's centre at y = 62 % of the height (below the reticle, not over it). Keep `CLOSE_BACK = 12`, `CLOSE_UP = 2.8`
  for the close view.
- Loops/Immelmanns: during a stunt, aim the camera at the plane (not 60 m ahead) and keep `ROLL_FOLLOW_STUNT` at 0.4.
  The plane must stay within the central 60 % of the screen (2:20, 4:05 and 5:35 fail).

### 3. Terrain looks like untextured clay (content + render)
At 0:40 and 6:05 there is no visible detail texture, the slopes are a flat green gradient, and the volcano is green to
the top with a small grey cap.
- **Detail texture**: make it visible: a 256x256 texture tiled every 24 m, contrast ±18 % around mid grey,
  multiplied in the combiners (×2 modulate). Add a second, larger "macro" variation (a 256 tile every 600 m, ±10 %)
  to kill the camo blotches.
- **Vertex colours by slope/height**: slopes over 35° → rock `7A5E4E`/`6A4F42` strata (8-12 m bands). Above 380 m →
  ash `5A5552`, blending to basalt `3B3330` at the rim. Jungle `2F7A2E` with ±12 % noise per vertex, and grass
  `7BB043` only on flats below 120 m.
- **The volcano needs its plume**: 10-12 billboards, 60-120 m, `8A8580` → `C9C4BE` at the top, drifting downwind at
  4 m/s. It is the island's landmark from everywhere and is currently missing.
- **Canyon walls**: project the texture along the wall (triplanar or a U = horizontal distance), not from above.
  Ambient on shadowed walls ≥ 0.45 of the lit value (the black wall at 1:30 must become `4A3A30`-ish), plus a sky-fill
  term.

### 4. Props are red cubes (content)
The harbour village at 2:25 is ~30 identical red boxes in a grid-like spread, and the pier is a stick.
- Huts: a box with white walls `F1E8D6` + a terracotta hip roof `C4502E` with a 0.6 m overhang, 3 sizes,
  random yaw ±15° off the street direction, in 2-3 rows along the shore, plus 4-6 larger buildings (warehouse, church
  tower) near the pier. Pier: deck + pilings, 40 m. Moored boats (3-4 simple hulls).
- Palms: clusters of 3-9 with lean and height variance, densest along the beach. At 2:25 the beach is bare.

### 5. Coastline and shore foam (content + render)
The saw-tooth shoreline and foam triangles (5:05, 6:20) are the cheapest-looking thing on screen.
- Foam: make it a strip mesh that follows the 0 m contour (marching squares on the height field, smoothed with
  Chaikin ×2), 6-10 m wide, alpha falling off seaward, its V scrolled at 0.3 Hz for a pulse. Not per-terrain-triangle.
- Beach: raise the terrain resolution near the coast, or smooth the heights around 0 m (a ±3 m blend band) so the
  sand line is a curve. A wet-sand band `C2A56E` 4 m wide above the water.

### 6. Audio: a flat wall of sound, and one dropout (audio)
- **Dropout at 4:28.3** (≈ mission 4:14, during the boss intro): ~100 ms of silence. Likely the BOSS music switch or a
  mixer underrun while the zeppelin spawns. Find it and fix it. The rule: no glitch ever.
- Loudness range 2.5 LU: the engine drone + music fill 20-300 Hz at −15..−25 dB all the time (spectrogram), so kills
  and explosions do not stand out (2:25 kill: no level change). Changes:
  - Player engine −6 dB at cruise; let boost add +3 dB and the whine.
  - Music −4 dB under combat, and duck it −6 dB for 0.6 s on every kill and big explosion.
  - Explosions: +6 dB relative to now, with a 40-60 Hz boom of 0.5 s *and* a 2-6 kHz crack transient (the spectrum
    shows almost nothing above 2 kHz at kills).
  - Kill sting: a short brass/xylophone stab (0.4 s) on each kill, layered with the banner.
- Target: a loudness range of 6-9 LU over the mission, integrated loudness −16 LUFS (currently −16.9: fine).

### 7. HUD layout (game/hud.cpp)
- Move the objective line to the top centre, under the score row (y = 40, centred). Nothing at y 330-420 except
  reticles and popups.
- Popups: stack them with a 14 px line pitch and queue them (max 2 visible). Never overprint (2:20, 4:53).
- Off-screen target arrow: 18 px brass-outlined crimson triangle on an ellipse at 80 % of the safe area, pulsing,
  with the distance under it. The current 6 px red tick (5:00) is invisible.
- Gun heat arc on the reticle: not seen; add it if it is missing.

### 8. Pacing: two dead minutes in wave 1 (game/mission + ai)
Wave 1 starts at 0:20; the first kill is at 0:22, then nothing until 2:24. The autopilot flew the canyon, but the 3
remaining fighters never came after the player (1:05-2:20: radar dots, no attacks).
- Enemies with no attack target for 10 s go for the player wherever he is. One follows the player into the canyon
  (it is a great moment).
- Wave 1 should take 60-90 s, not 2:15.

### 9. Zeppelin finale (game/zeppelin + effects)
- The burn-through is good, but the fire is ~40 small sprites spread like confetti (6:00-6:10). Use 8-10 big
  fireballs (15-25 m) rolling along the hull, plus 3 black smoke columns 40-80 m wide that rise.
- It sinks *at the beach* (6:20). Steer the death dive to open water ≥300 m off the coast, then: a splash column
  (40 billboards, 60 m tall, white `F4F7F2`), a final big explosion, then the hull sinks in 4 s with a steam cloud.
- Log: "zeppelin circling at 596 m". The design says 400 m. Lower it to 400 so it is framed against the island, not
  the sky.
- Slow motion on the engine kills was not visible in the samples. Check 0.35x for 0.5 s on each engine kill.

### 10. Propeller disc rim (content)
The prop disc has a hard yellow-green outline ring (0:20, 5:10, every chase frame). It reads as a second reticle.
Fade the disc alpha to 0 at the rim (radial alpha: 0.35 at the centre → 0 at r = 1.4 m), no outline. Two faint blade
ghosts.

### 11. Smoke trails of damaged planes (game/effects)
At 4:45 the trails are single strings of small dots. Make them 3-6 m billboards emitted every 0.04 s, growing to 12 m
over 2 s, alpha 0.6 → 0, dark `3A332E` for damaged and black for burning, with a fire core near the plane.

### 12. Shadows (render)
No plane or zeppelin shadows anywhere (low flight at 1:30 over the canyon water). Add a blob shadow under planes below
200 m, and the zeppelin's ellipse (180x30 m, alpha 0.35) projected straight down on the sea and land.

### 13. Sea detail (render)
The sea is a good colour but static. Add a scrolling ripple normal/detail at 2 scales and sparkle billboards on the
sun glint path (0:20 has the glint; it must sparkle). Fade the far sea to the horizon colour in every direction.

### 14. Speed lines (game/effects)
They read as rain (2:20 burst: scattered white dashes in the sky). Only above 110 m/s, only within 12 % of the screen
edges, 8-12 lines, alpha 0.25, and stretched along the velocity vector.

### 15. Loading screen (game/menus)
A plain progress bar on black. Use the title logo, a brass bar, and a rotating propeller icon. It takes 12 s: show a
tip line ("Barrel rolls dodge bullets").

## Next round
Send a 60-90 s **human-flown** clip (dogfight at <300 m, a kill up close, the canyon, one loop), stills of the
harbour at 50 m and the island from 1500 m, and the results screen.
