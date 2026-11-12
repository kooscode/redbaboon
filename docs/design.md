# RedBaboon: design

A 3D arcade dogfighting game for the original Xbox, built with nxdk (pbkit for 3D, SDL3 from nxdk-sdl3 for the
controller and timers, the AC'97 driven directly for sound).
Pulp 1930s alternate history, inspired by the feel of Crimson Skies: High Road to Revenge (arcade handling, stunts,
sky pirates, a zeppelin to bring down) but with its own world, characters and level.

## 1. The pitch

You are **Captain "Red" Baboon**, a baboon flying ace in goggles and a long red scarf, in a crimson Spitfire-style
fighter. The **Silverback Syndicate**, gorilla sky pirates, are raiding the banana plantations of **Isla Platano**.
Your guns fire bananas (yellow tracers), your rockets are banana rockets. Shoot the pirates down, fly through the
canyon rings for bonus points, and bring down their flagship, the zeppelin **Silverback**.

Tone: bright, sunny, cartoon-pulp. Saturated colours, warm late-afternoon light, big explosions, banana peels as
debris. The creative brief (docs/creative/brief.md) sets the palette, camera, game-feel numbers and the detail
checklist every review is graded against; where it is more specific than this file, it wins.

## 2. The level: "Raid on Isla Platano" (one level, 8-12 minutes)

The map is 8 km x 8 km. A large volcanic island in the middle (peak ~600 m) with a deep canyon cutting through it
east-west, beaches, palm groves, a harbour village with a lighthouse on the south shore, the pirates' rock-tower base
on a small island to the north-east, small islets around, and open sea to the edges. Flight is limited to a 10 km
square and 2000 m altitude: past the edge a radio message turns you back, then the plane auto-turns.

Mission flow (game/mission.cpp drives it; each phase shows an objective line and radio messages):

1. **Intro** (4 s): camera sweeps around the player's plane flying over the harbour. Radio: "Pirates over the
   harbour, Red! Get 'em!"
2. **Wave 1**: 4 pirate fighters (gunmetal with hazard-yellow stripes) attacking over the harbour. They are easy (slow
   turn, poor aim).
3. **Canyon run** (optional, runs in parallel): 8 stunt rings along the canyon; each ring +250 points, all 8 +2000.
4. **Wave 2**: 6 fighters including 2 aces (purple with twin tail fins, faster, better aim, use stunts to evade).
   Destroyed planes may drop a crate (banana crate = repair 35 %, rocket crate = +4 rockets) that floats down slowly;
   fly through it.
5. **Boss**: the zeppelin Silverback arrives from the north-east (180 m long, cruising 15 m/s in a wide circle over
   the island at 400 m). 4 gondola turrets fire flak/tracers at the player within 900 m. 4 engine pods are the
   targets: each has its own health and smokes when damaged, burns when destroyed. Up to 3 escort fighters at a
   time keep launching from it (6 in total). When all 4 engines are gone the zeppelin tilts nose down, burns along
   its length with chained explosions and crashes into the sea: **victory**.
6. **Victory**: slow-motion camera on the falling zeppelin, then the results screen: kills, rings, stunts, accuracy,
   time, score, and a rank (Chimp, Monkey, Baboon, Silverback).
7. **Defeat**: health 0 = the plane catches fire and goes down (or hits the ground/sea). The player has 3 lives;
   respawn in the air near the harbour with 2 s invulnerability. After the last life: "Shot down!" screen with
   Retry / Quit.

## 3. Controls (Xbox controller, Crimson Skies-like arcade scheme)

| Input | Action |
|---|---|
| Left stick | Pitch (up = nose down, inverted option in pause menu) and turn: left/right banks AND turns the plane (auto-coordinated yaw); the plane levels its wings when released |
| Right stick Y | Throttle: up = boost, down = brake/slow for tight turns. Boost and stunts share one recharging meter |
| Right stick X | Rudder (flat yaw, for fine aim) |
| Right trigger | Banana machine guns (hold; heat meter, overheats for 2 s) |
| Left trigger | Banana rocket (8; mild homing toward the locked target) |
| A | Stunt: with the left stick up = loop, down = Immelmann turn (half loop + roll, a fast 180), left/right = barrel roll (dodges: bullets miss during the roll) |
| B | Cycle target (nearest in front first) |
| X | Hold: target view (camera looks at the locked target from behind the player) |
| Y | Look back |
| White / Black | Camera: chase / close chase |
| Start | Pause menu (Resume, Invert Y, Restart, Quit) |
| Back | Show mission objective and the score so far |

## 4. Shared conventions (src/core/)

- `core/vmath.h`: Vec3, Quat, Mat4, Rng, PackARGB. World is right-handed, +Y up, sea level y = 0, metres.
  Objects face -Z. Mat4 is row-major with row vectors (p' = p * M); "first A then B" = A * B.
- `core/geometry.h`: Vertex (pos, normal, uv, ARGB colour; 36 bytes), MeshData (16-bit indexed triangle list),
  ImageData (ARGB, power-of-two sizes).
- Colours are 0xAARRGGBB everywhere.
- Time step: the game updates with a fixed 1/60 s step (accumulator), rendering once per frame.

## 5. Rendering (src/render/, Xbox GPU via pbkit)

Interface: `render/render.h`. Implementation: `render/render_xbox.cpp` plus Cg shaders in `render/shaders/`
(compiled by nxdk's cgc + vp20compiler/fp20compiler into `.inl` files under `build/` at build time).

Look, within what the NV2A (GeForce 3 class, 233 MHz, 4 texture stages, register combiners) does well:
- 640x480, 32-bit colour, 24-bit depth, 60 fps target (never below 30).
- Fog equals the sky horizon in every view direction (sun-side horizon warmer); fog 350-6500 m, far plane 7000 m.
- Per-vertex sun lighting (diffuse + ambient + Blinn-Phong specular) in the vertex shader; per-vertex linear fog
  blended in the combiners. Hemisphere ambient (sky colour from above, ground bounce from below) for planes.
- Sky dome with a zenith-horizon gradient, a soft sun disc and glow; the fog colour matches the horizon so the
  world fades into the sky.
- Sea: big alpha-blended plane, scrolling ripple texture, strong sun specular glint, darker colour far away.
- Terrain: vertex colours (sand, grass, jungle, rock, by height and slope) times a tiling detail texture; mipmapped.
- Planes: textured livery, specular metal highlights, spinning propeller blades that turn into a translucent disc
  at speed, hit flash (emissive), smoke and fire trails when damaged.
- Particles: billboards for smoke, fire, explosions, muzzle flashes, banana-peel debris, clouds; streaks for banana
  tracers and wingtip vapour trails in hard turns.
- 2D overlay: HUD, menus and text with a built-in bitmap font.
- Budget: ~60k triangles and ~300 draw calls a frame; textures 256x256 (title 512x128), all swizzled and
  mipmapped where seen at a distance. Total GPU memory for meshes + textures < 16 MB.

## 6. Audio (src/audio/)

Interface: `audio/audio.h`. Everything is synthesised at start-up into 16-bit mono sample buffers (no sound files):
short bright effects at 32 kHz, voices and music at 22 kHz, explosions, splashes and engine loops at 16 kHz; 4.3 MB.
`Init` opens the device and builds the bank on a background thread during the content upload: the title theme and
menu sounds first (0.33 s in xemu), the rest after (1.1 s in all in xemu, 50 ms on a PC); main.cpp calls
`WaitUntilLoaded` before play. Synthesis uses no libm in its loops (table sine, polynomial sin/cos/exp, SSE sqrt),
and the explosions are one blast rendered once and replayed at different speeds.
- `synth.cpp`: building blocks (band-limited oscillators, biquad/SVF filters, envelopes, Karplus-Strong strings,
  modal "struck object" synthesis, seamless loop cross-fades).
- `sfx.cpp`: the effects, 1-4 variants each (never the same twice in a row, pitch jitter): banana gun (click, 90 Hz
  thump, pop, mechanical tail), rocket whoosh, metal "tink" hits, explosions (crack, 40-60 Hz boom, roar, debris
  rattle), the zeppelin's long blast, splash, chimes, stunt swoosh, warning, overheat hiss + clank, glass crack,
  engine sputter, radio blip, cockpit crunch, menu marimba. Monkey cheers/screeches and gorilla grunts are formant
  voices (sawtooth glottal source, three vowel formant filters, pitch glides, roughness, "k" releases).
- Engines: pirates a lumpy 9-cylinder radial chug (45 Hz firing, noise), the player a smooth inline growl (detuned
  saws ~70 Hz + 3rd harmonic, low putter); idle and full-throttle loops cross-faded by throttle, pitch 0.8-1.35x,
  a supercharger whine (900-1400 Hz) with boost, Doppler, distance lowpass, a wind "whoosh" for fly-bys within
  45 m. Only the 6 loudest engines play.
- `music.cpp`: a swing band (clarinet, trumpets, trombones, upright bass, marimba, tenor banjo, drums, timpani)
  and the tracks in a compact note notation with chord symbols; walking bass, banjo comping, brass stabs and drum
  patterns are generated from the chords. TITLE (F major, 160 bpm AABA), COMBAT (G minor, 150 bpm swing, jungle
  toms), BOSS (C minor, 130 bpm, trombone ostinato, timpani), VICTORY (6 s fanfare), DEFEAT (sad trombone).
- `mixer.cpp`: 48 kHz stereo S16: 24 effect voices with priority stealing, 32 engine slots, 40 music voices from
  a sample-accurate sequencer with two slots for cross-fades, music ducking, a limiter and a soft clip on the
  master. Integer per-sample mixing, ~1.3 ms per audio second on a PC, ~19 ms in xemu (0.1 ms per 60 Hz frame
  with music).
- `audio.cpp`: the game-thread side (3D gain, pan, distance lowpass, distance/340 m/s delay for explosions,
  Doppler, voice barks limited to one per 3 s). It never blocks: effects go through a bounded lock-free queue,
  everything else through atomics. `audio_xbox.cpp` (with `audio_apu.cpp`) is the only platform-specific code: it
  drives the AC'97 PCM-out bus master itself (descriptor lists in page-aligned memory; not nxdk's XAudio, whose
  static descriptor list can straddle a page and stall the controller depending on the link layout), and a
  time-critical feeder thread keeps six 512-frame buffers (64 ms) queued, counting free buffers from the
  controller's current descriptor index every 4 ms.
- Mix: engines and fight music sit low (engine -5 dB, combat/boss music -5 dB), boost adds +3 dB and the whine;
  explosions carry a 2-6 kHz crack over the 40-60 Hz boom; KILL_STING on kills; ZEPPELIN_DEATH (8 s: three blasts,
  a huge splash, 30-60 Hz rumble, creaking metal) is the loudest sound; kills, blasts and radio calls duck the
  music 6 dB; a make-up gain of 1.4 into the limiter aims at -16 LUFS for a mission. Nothing is ever cut: every
  voice fades over at least 40 ms, a fanfare's last chord rings out over 1.5 s, a removed or silenced engine over
  0.6 s; a new track fades in over 0.3 s (0.8 s after a fanfare) while the old one fades out; on a screen change
  (results, pause, title, restart) the game calls `FadeOutEffects` (0.6 s).
- Music flow: the title theme plays from the loading screen on; VICTORY plays out, last notes and all, then the
  title theme follows under the results (`SetMusicNext`).
- The zeppelin's death: `EngineType::ZEPPELIN_FALL` is a looped sound bed (fire, sputtering engines, groans, then a
  rising wind and whistle), driven like an engine with throttle = progress of the fall and heard from far away;
  `Sfx::ZEPPELIN_DEATH` on impact. A silenced engine keeps its pitch and tone while it fades.
- xemu (0.8.136 and 0.8.139 checked) never sends the AC'97 PCM-out to the host: it plays only what the MCPX APU's
  voice processor makes. So in xemu (detected by its AC'97 not keeping the global control register) the device
  plays the same mix through one looping APU voice instead (`audio_apu.cpp`); on a console it stays on the AC'97.
  xemu's "Use DSP" setting must be off (its default): with it on, the voice goes to a GP DSP with no program. The
  serial log says which path is active. Checked by sending only xemu's stream to a private PipeWire null sink and
  recording that sink's monitor (a stream node target falls back to the default microphone). Sound is checked on
  the host: `make audio_render` writes WAV files and levels to `build/audio/`.

## 7. Content (src/content/)

Interface: `content/content.h`. All meshes and textures are generated by code at start-up, with fixed seeds.
- The player's plane: a Spitfire-like fighter (elliptical wings, long nose, bubble canopy) in crimson with cream
  trim; the baboon pilot's head (muzzle, goggles, red scarf) visible in the cockpit. ~2-3k triangles.
- Pirate fighters: chunkier radial-engine planes (stubby fuselage, gull wings), green or purple with stripes,
  a gorilla pilot head.
- The zeppelin: a long silver-grey hull with fabric panel texture, fins, gondola, turrets, 4 engine pods, a skull
  and banana emblem.
- The island terrain (height field, 257 x 257 samples over 8 km, cut into 8 x 8 tiles), sea, palm trees, huts,
  lighthouse, rocks, the pirate tower base, stunt rings, crates, bananas and banana rockets.
- Textures: liveries, terrain detail, water, particles, clouds, HUD atlas, the title logo.
- `tools/preview` renders meshes and textures to PNG on the host so they can be checked without the Xbox
  (`make preview`: `build/preview/`).

## 8. Game (src/game/, src/platform/, src/main.cpp)

- `main.cpp`: start-up (video, SDL, audio, content upload), the fixed-step loop, state machine (title, briefing,
  playing, paused, results).
- Flight model (`flight.cpp`): arcade. Speed 45-140 m/s (cruise 75, boost 120). Turn rate depends on speed (best
  around 70 m/s). Gravity trades speed in climbs and dives (energy), no real stall: below 45 m/s the nose drops.
  Automatic wing levelling when the stick is released. Stunts are scripted manoeuvres blended in.
- AI (`ai.cpp`): states Patrol, Attack (lead pursuit with aim error by skill), Evade (break turn / barrel roll when
  shot at or when the player is on its tail), Extend (fly away to gain distance), Avoid ground. Re-evaluates
  targets every few seconds; enemies sometimes attack the harbour instead of the player (wave 1).
- Weapons (`weapons.cpp`): bullets are simulated points (speed 600 m/s + plane velocity, life 1.5 s) drawn as
  streaks; ray-sphere hits against planes (and engines/turrets); rockets with mild homing; flak bursts from the
  zeppelin turrets.
- Damage: health per plane; smoke below 50 %, fire below 25 %, explosion + debris at 0; hit flash; collisions
  with ground, sea and the zeppelin hull destroy the plane.
- Camera (`camera.cpp`): smooth chase camera with lag and a little shake on hits/boost; target view; look back;
  cinematic cameras for intro and victory.
- HUD (`hud.cpp`): health bar, boost meter, gun heat, rocket count, score, lives, a radar circle (top right),
  target bracket with its health bar and a lead reticle, an arrow toward an off-screen target, objective line,
  radio messages with a portrait icon, hit markers, red screen flash on damage, low-health warning.
- Menus: title screen (logo over a slow fly-by of the island), pause, results, defeat.

## 9. Build and test

- `make` builds `build/RedBaboon/default.xbe`; README.md lists every target. Everything a build writes goes under
  `build/`.
- `tools/xemu/screenshots.sh BIN_DIR OUT_DIR [WAIT] [SHOTS] [INTERVAL] [PAD_SCRIPT]` boots it in xemu on OXOS,
  takes screenshots, saves the serial log (DbgPrint) and stops; a lock makes concurrent runs take turns
  (docs/xemu.md). `make hardware` runs it on a real console (docs/hardware.md).
- Autoplay: a file `autoplay.txt` next to `default.xbe` (D:\) starts the autopilot instead of the title. Its lines
  are `key=value` (`#` starts a comment, unknown keys are ignored):

  | Line | Mission seed |
  |---|---|
  | none, or an empty file | the clock, new for every mission (as in normal play) |
  | `seed=fixed` | `5eed1234`: the same mission every run (test recordings) |
  | `seed=0x1234ABCD` or `seed=305441741` | that seed |

  Each mission logs its `seed XXXXXXXX` on the serial port.
- `make test` builds and runs the host tests (flight model, headless mission simulation, seed parser) with the null
  renderer and null audio device; `make preview` and `make audio_render` export the content and the sound for
  checking by eye and ear.
- C++ style: docs/cpp_style.md.
