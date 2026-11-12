# RedBaboon: creative brief

The bar: a screenshot that passes for a 2003-2004 first-party Xbox exclusive, and handling that feels like an arcade
ace game in the Crimson Skies tradition (forgiving, fast, stunt-driven, loud). Motto from the user: **perfection
perfected**. Every review round is graded against §12.

What the genre's best games got right (reviews and play): pick-up-and-play controls that become "second nature"
within minutes; stunts as an *escape tool* paid for from a recharging meter shared with boost; unlimited guns with
overheat plus scarce secondaries; crates dropped by kills; zeppelins as set pieces that visibly burn apart; long draw
distance; an orchestral, swashbuckling score; confident voice barks. Ace Combat adds: always know where the target
is, and every kill is announced. Our job: that, in one dense 10-minute level.

## 1. Visual target

**Time of day: late afternoon, ~16:00.** Sun 32 degrees high, from the south-west (azimuth 220). Warm key light,
cool sky fill, long readable shading on the volcano. The intro flies the player *toward* the sun over the harbour:
glint path on the sea plus lens flare in the first 4 seconds.

### Palette (0xRRGGBB)

| Element | Colours |
|---|---|
| Sky zenith / mid / horizon (away from sun) / horizon (sun side) | `2A62B8` / `5E93D6` / `C9DDEB` / `F3DCAE` |
| Sun disc / glow | `FFF8E6` / `FFD98A` (additive, falls off over 25 degrees) |
| Sea deep / mid / lagoon shallows / foam | `0E4C7A` / `1B78A8` / `35C6C0` / `F4F7F2` |
| Sand dry / wet band | `E9D29A` / `C2A56E` |
| Grass / jungle / jungle shadow | `7BB043` / `2F7A2E` / `1F5A26` |
| Volcanic rock / basalt / crater ash | `7A5E4E` / `3B3330` / `5A5552` |
| Roofs (village) / walls / lighthouse | `C4502E` terracotta / `F1E8D6` / `F5F2EA` with `C1121F` band |
| Player plane | crimson `C1121F`, shade `7A0A12`, cream trim `F2E6C9`, banana roundel `FFD23F`, spinner `D9D4C7` |
| Pirate fighters | gunmetal `3A3F47`, hazard yellow stripes `E8B923`, bone skull nose `EDE6D3` |
| Pirate aces | royal purple `5B2A86`, gold `D4A017` |
| Zeppelin | hull panels `A9AEB3` / `9AA0A6` alternating, band `1A1A1A` + `C1121F`, girders `4A4038` |
| Player tracers | core `FFFBE0`, edge `FFD23F` (additive) |
| Enemy tracers | core `FFE0D0`, edge `FF5A3C` (red-orange: incoming fire must be readable) |
| Explosion ramp | `FFF6D5` -> `FFB23F` -> `FF6A1A` -> `5A3A2A` -> smoke `3A332E` -> `8A8580` |
| Flak | burst `FFC060` flash, puff `2B2B2B` |
| UI | cream `F4E4C1`, brass `C9A227`, crimson `B3202A`, ink `1A1410`, alert `FF3B2F` |

**Rule:** saturation lives in the near field. Fog desaturates and warms the distance. No pure white or pure black on
any lit surface (max `F5F2EA`, min `1A1410`).

### Fog and haze
- Fog colour must equal the sky horizon in the view direction. Cheap fix in the vertex shader: per-vertex fog colour
  = lerp(`C9DDEB`, `F3DCAE`, max(0, dot(view_xz, sun_xz))^2), blended in the combiners. The sky dome uses the same
  formula. A seam between terrain and sky on the horizon is a fail.
- Fog start 350 m, end 6500 m, far plane 7000 m (not 1500/9000: the island must have aerial perspective, and the
  far edge must never pop).
- Sea fades to the horizon colour, not to dark blue.

### How the island reads
- **From 1500 m:** one silhouette. Volcano cone with a slow grey smoke plume (10-12 large billboards, drifting
  downwind), the canyon as a dark slash east-west, a white surf ring, a turquoise lagoon halo (shallows from terrain
  depth under each sea vertex), the harbour as a cluster of terracotta and white with a white lighthouse, the
  pirate rock tower as a dark spike NE. Cloud shadows optional.
- **From 50 m:** palms in clusters of 3-9 with varied height (8-14 m) and lean (never a grid), wet-sand band at the
  waterline, foam strip pulsing, detail texture visible (2 tiles per 16 m), rocks at the canyon walls, huts with
  roofs that overhang. The canyon walls are banded (strata: alternate `7A5E4E`/`6A4F42` every 12 m of height).

## 2. Plane and character design
- **Silhouette first.** Player: elliptical wing, long pointed nose, bubble canopy, tall tail: reads as "the hero" at
  200 m from any angle. Pirates: fat radial cowling (a dark ring at the nose), gull wings, stubby tail, slightly
  oversized (115 %) so they read at range. Aces: same body plus twin tail fins (a different shape, not just colour).
- **Cartoon proportions:** canopy and pilot head 130 % scale; the baboon's muzzle and goggles must be recognisable
  in the chase camera (muzzle `D9927A`, goggles brass `C9A227` with lens glints). Scarf: 4-6 segment ribbon with a
  sine flutter, crimson. The gorilla pilot: black head `2A2A2A`, silver brow `B8B8B8`, leather cap.
- **Livery details that sell polish:** cream cheat-line along the fuselage, banana roundel on wings, panel lines
  (dark 1-px lines in the 256 texture), exhaust stains behind the cowling, a nose number "01". Wing-root and leading
  edges slightly worn (lighter). Prop spinner shiny (specular 0.8).
- Propeller: blades below 30 rps, disc above, crossfade over 0.2 s; disc alpha 35 % with two faint blade ghosts.
- Rim light: add a sky-colour fresnel term (0.25) on planes so they separate from the jungle.

## 3. Effects, ranked by impact per cost

1. **Sea:** sun glint path (high exponent 80-120 specular + additive sparkle billboards on the glint path), lagoon
   turquoise and foam ring via sea vertex colours from terrain depth. Biggest "tropical Xbox" read.
2. **Cumulus field** at 450-900 m: 25-40 clusters of 6-12 soft billboards, lit side `FFFFFF`, shadow side `B9C6D6`.
   Flying through one: white fade to 70 % for 0.3 s + vapour wisps. Biggest free sense of speed.
3. **Explosions:** additive flash (radius 2x the fireball, 2 frames), fireball atlas 8 frames, then smoke that rises
   and grows, sparks (streaks), debris (banana peels and 3-4 tumbling plane chunks) with smoke trails, a short screen
   flash when within 150 m.
4. **Damage smoke/fire trails** behind planes (billboards every 0.05 s; black-grey; fire orange core).
5. **Blob shadows:** soft dark ellipse under each plane below 200 m altitude (fade with height), and a huge one for
   the zeppelin on sea and land. Grounds everything; makes low flight thrilling.
6. **Sun glow + lens flare:** 5-6 additive ghosts along the sun-centre line, occluded by a CPU ray test against
   terrain and zeppelin, faded over 0.15 s.
7. **Muzzle flashes** (2 per wing gun, random rotation, 1 frame) and **tracer streaks** (every bullet, 8-12 m long).
8. **Wingtip vapour** in turns above 3.5 g-equivalent; **speed lines** (thin additive streaks near the screen edge)
   only above 110 m/s.
9. **Hit sparks + flash** on targets (emissive white 80 ms), metal chips.
10. **Fake bloom:** additive glow sprites on fire, flashes, sun, tracer heads, lighthouse lamp. No render-to-texture
    bloom unless the renderer gets it cheaply.
11. **Heat haze / volumetrics / real shadows: skip.**

## 4. Camera rules
- Chase: 13 m behind, 3.2 m above the plane, looks at a point 25 m ahead of the nose. Close chase: 8 m / 2 m.
- Position spring: critically damped, settle time 0.25 s; rotation follows with 0.12 s lag; camera roll follows 70 %
  of plane roll (horizon still tilts, never fully locked).
- Vertical FOV: 58 at cruise, up to 70 at full boost (eased over 0.4 s), 54 when braking. Speed-to-FOV is the main
  sense-of-speed tool.
- Shake (positional, decaying, 25 Hz noise): hit taken 0.25 m / 0.25 s, near explosion 0.6 m / 0.5 s scaled by
  distance, boost 0.04 m continuous, guns 0.02 m. Never rotational shake above 0.5 degrees.
- Never clip into terrain or zeppelin: raycast and pull in. Look-back and target view blend over 0.2 s, not cut.
- Stunts: the camera does not roll fully with barrel rolls (cap at 40 %), so the player stays oriented.

## 5. Game feel
- Stick: radial deadzone 0.24 (worn Duke sticks drift), expo curve 1.6. Response: full roll rate after 0.12 s.
- Roll rate 200 degrees/s, pitch 75 degrees/s at the best-turn speed 70 m/s, falling to 45 at 140 m/s and 35 at
  45 m/s. Auto-level 1.0 s after release, eased.
- Rudder: 25 degrees/s, also slides the aim; brake + full turn = 180 degrees in ~2.6 s.
- Guns: 14 rounds/s alternating wings, 2 % random spread, 4 s to overheat, 2 s lockout with a steam hiss.
- **A hit must feel like a hit:** target flashes white, sparks, a metallic "tink" layered over the gun sound, a hit
  marker tick on the reticle, +score popup. **A kill:** explosion + kill banner ("SPLASHED!" text 0.8 s) + radio
  cheer + rumble + 2-frame white screen flash at 25 % if within 300 m. No hit-stop on hits. Slow-motion (0.35x for
  0.5 s) only on an engine pod kill and the zeppelin death.
- **Taking damage:** red edge vignette (not full screen), rumble, a camera kick, cockpit crunch sound, glass-crack
  sound below 25 %, engine sputter when on fire.
- Rumble (if the input layer supports it): guns = small motor 0.2 pulsed; hit taken = big motor 0.8 / 120 ms;
  near explosion = both 1.0 decaying 400 ms; boost = big motor 0.15.
- Frame rate: locked 60, or locked 30 everywhere. Never a variable 40-60.

## 6. HUD and UI (pulp 1930s, art deco)
- Colours: cream text with 1-px ink drop shadow (always), brass frames, crimson alerts. No pure green/blue
  "programmer" bars.
- Text only at integer scales (2x for body, 3x for banners, 4x title). Build a custom 8x8 font with deco shapes
  (tall narrow uppercase, thick verticals, no lowercase needed). Letter spacing 1 px at scale 1.
- Everything inside the title-safe area: 32 px from left/right, 24 px from top/bottom.
- Gauges as **brass dials** (radial needle), not bars, for health and boost; heat as a small arc on the reticle.

```
+--------------------------------------------------------------+
|  SCORE 012450                             .----.             |
|  x3 [baboon icon]                        ( RADAR)  brass ring|
|                                           '----'             |
|  [portrait] "Pirates over the harbour!"  (radio, 4 s)        |
|                                                              |
|                 [ target bracket + health tick bar ]         |
|                          o  <- lead reticle                  |
|                        --+--  gun reticle + heat arc         |
|                                                              |
|  OBJECTIVE: Down the raiders 2/4                             |
| (HEALTH dial) (BOOST dial)                  ROCKETS 6        |
+--------------------------------------------------------------+
```
- Messages slide in (0.15 s ease-out), never pop. Banners scale 1.3 -> 1.0 over 0.2 s.
- Title screen: logo over the slow island fly-by, "PRESS START" pulsing at 0.8 Hz; menu cursor a banana.

## 7. Audio direction
- Player engine: smooth inline growl (two detuned saws ~70 Hz + 3rd harmonic, putter noise low), pitch 0.8-1.35x by
  speed/throttle; boost adds a supercharger whine (sine 900 -> 1400 Hz). Pirates: lower, lumpier radial chug (45 Hz,
  more noise). Doppler on fly-bys: a fly-by within 40 m must "whoosh" (worth getting right).
- Guns: three layers (click transient, 90 Hz thump, short mechanical tail), 4 variants, pitch +-6 %, never the same
  sample twice in a row. Overheat: steam hiss + clank.
- Explosions: layered crack + low boom (40-60 Hz) + debris rattle; far ones lowpassed and delayed by distance/340.
- Music: 1930s adventure serial: brass stabs, snare rolls, upright bass/tuba, marimba for the island. Combat 150 bpm
  swing; boss: minor key, timpani, 130 bpm; victory fanfare 6 s. Duck music -6 dB under radio barks.
- Voice barks: formant "ook-ook" cheers and gorilla grunts, max one per 3 s, 4+ variants each.
- Mix: headroom, soft limiter on the master; 24 voices with priority (player gun, hits, nearby explosions first).

## 8. Mission pacing
| Time | Beat |
|---|---|
| 0:00-0:06 | Intro sweep toward the sun over the harbour; title card "RAID ON ISLA PLATANO" |
| 0:06-2:00 | Wave 1: 4 easy fighters; first kill within 30 s guaranteed (one flies straight across the nose) |
| 2:00-2:20 | Breather: radio points at the canyon rings, rings glow and the radar shows them |
| 2:20-5:00 | Wave 2 + aces; aces announced by name with a radio taunt; crates drop |
| 5:00 | Zeppelin reveal: it emerges from the cloud bank NE, music shifts, camera cut 2 s |
| 5:00-9:00 | Boss: engines one by one, each kill = slow-mo + smoke trail on the hull; escorts launch from its belly |
| 9:00 | Death: tilt, chained explosions along the hull, burning panels reveal the girders, crash and splash column |
| +10 s | Results screen with rank stamp (ink stamp animation) |

## 9. Top 15 "wow" moments for an OG Xbox veteran
1. The sun glint path on the sea with flare, first frame of the intro.
2. Turquoise lagoon and foam ring around the island.
3. Bursting through a cumulus cloud at full boost.
4. The zeppelin's shadow sliding over the sea.
5. Zeppelin hull burning away to show its frame.
6. The zeppelin crash: nose-down, chained blasts, splash column.
7. Low flight over water with own shadow and spray (spray billboards below 15 m).
8. Canyon run at 120 m/s with walls whipping past.
9. Barrel roll dodging red tracers.
10. Fly-by Doppler of a pirate passing 20 m away.
11. Kills with tumbling debris trailing smoke.
12. Volcano plume visible from everywhere.
13. Wingtip vapour in a hard turn against the sun.
14. A fluttering scarf and a visible baboon pilot.
15. Solid 60 fps with no hitches at all.

## 10. Top 10 "cheap" tells to avoid
1. Fog colour not matching the sky; visible far-plane pop.
2. Flat untextured terrain or a grid-pattern detail texture.
3. Uniform palm placement and identical rotations.
4. Particles with hard square edges or obvious sorting errors.
5. Default-looking scaled bitmap font, misaligned HUD, plain bars.
6. Enemies that are hard to see (camouflaged) or impossible to track.
7. Planes that turn like they are on rails, with no bank-to-turn or lag.
8. Hits with no feedback; kills with a single small sprite.
9. One gun sample on a fast loop (machine-gun repetition).
10. Hitches, frame drops on explosions, or tearing.

## 11. Changes we must not get wrong
- Pirate colours must not blend with the jungle: gunmetal with hazard yellow.
- Incoming fire reads differently from ours.
- Stunts must cost something (special meter).

## 12. Detail checklist (graded every review round: pass / minor / fail)
| # | Item | Pass means |
|---|---|---|
| 1 | Edges and aliasing | No crawling on wing edges, palms, rings; mipmaps on all distant textures; trilinear; anisotropy on terrain/sea if available; multisampling if pbkit allows it |
| 2 | Z-fighting | None on decals, roofs, runway, rings, foam, shadows (depth bias or offset) |
| 3 | Texture seams | No seams between terrain tiles, on the sky dome, or on the sea tiling (no visible repeat at altitude) |
| 4 | Pop-in | No objects appearing within 3 km; props fade in, never pop |
| 5 | LOD and fog transitions | Smooth; fog equal to sky at the horizon in every direction |
| 6 | Shimmer | No sparkling on detail textures, water or alpha-tested foliage in motion |
| 7 | Font and kerning | Integer scales, even spacing, shadowed, no clipped glyphs |
| 8 | HUD alignment | On an 8-px grid, inside title-safe, consistent margins and baselines |
| 9 | Animation easing | All UI and camera motion eased (no linear starts/stops); props and pilots animate |
| 10 | Sound clipping and repetition | No clipping on the master; no audible repeat patterns; no clicks at loop points |
| 11 | Camera jitter | No jitter at any speed (interpolate render state between fixed steps) |
| 12 | Frame pacing | Locked rate, even frame times, no hitches on explosions or content upload |
| 13 | Sorting | Transparent particles and clouds sorted back to front; no hard intersections with terrain (soft fade near ground) |
| 14 | Colour and contrast | Palette respected; targets readable against sky, sea and jungle |
