# Review 3: polish round 2 on hardware

Material: `captures/hw/game_round2.mp4` (565 s, autopilot, real Xbox, own audio), serial log
`captures/serial/serial-20261111-043403.log`, master 27fa500. Timestamps are **video time**; mission time ≈ video − 16.5
s (ring 0 at mission 4:08 = video 4:24.6). The loading-screen, prop-LOD and particle-cap changes came after this
recording, so they are not judged here.

**Score: 7.0 / 10** (round 2: 6.5, round 1: 5.5). I am **not** happy as a clam yet. In the air, at altitude, with
the sea in shot, this now passes for a 2003 Xbox game: 0:22 (sun-path sparkles, coast, shadowless haze), the boss
pass at 5:40-6:05, the zeppelin skeleton burning against the island at 6:40-7:00, the results panel over the live
volcano. But this round brings one **showstopper** (a half-drawn frame on screen, #1), and one mission bug that
turns the new wave 3 into nothing (#2). Up close, the land still looks like a blurred photo of clay. Fix the top 5 and
this is an 8.

## What's fixed (must not regress)
- **Frame rate:** 59.9-60.3 fps, worst 20 ms (one frame at the boss arrival). Never mind the 51-58 fps dips any more.
- **Audio level:** −17.0 LUFS integrated over the mission (target −16 ±1: OK). Peak −4.2 dBFS, no clipping. Music on
  the loading screen. The zeppelin's death blast at 7:11 (426-428 s) is the loudest broadband moment of the run.
  Right.
- **Player damage:** the cowling fire at 4:40 is the right size. "HULL CRITICAL" is short. The vignette is crimson in
  the corners only.
- **Zeppelin:** girders are now thick and charred, with glowing hot spots (6:40, 6:52). The chase camera alongside
  is a great shot. The fins are painted (5:45, 6:05).
- **Canyon:** strata, a wandering wall line and the **rock arch** (1:50, 9:00). Flying under it, through a ring, is the
  best "game" moment in the run.
- **Results:** a translucent panel over the live volcano fly-by, values counting up, the portrait and quote, the
  SILVERBACK stamp (7:15-7:25). Exactly the brief.
- **Radio:** text wraps below the objective line, no more collisions.
- **Rim light:** no more neon edges (6:30, 8:30).

## §12 detail checklist

| # | Item | Grade | Evidence |
|---|---|---|---|
| 1 | Edges/aliasing | minor | The rock/grass blend no longer steps in 31 m blocks. Canyon rim strata still read as stacked "pancakes" with hard steps (1:03 skyline, 2:00). |
| 2 | Z-fighting | pass | None seen. |
| 3 | Texture seams | minor | Pale cream **triangular wedges** stick out of the canyon walls (1:45, 2:05, 4:40, 9:00). They read as broken geometry, not scree. |
| 4 | Pop-in | pass | None seen. |
| 5 | LOD/fog | pass | Distant terrain is fine now. |
| 6 | Shimmer | pass | None. |
| 7 | Font/kerning | minor | Radio wrap breaks "GET / 'EM!" across lines (0:16). |
| 8 | HUD alignment | minor | In the intro the radio box overlaps the title card's top line ("RAID ON", 0:16-0:20). |
| 9 | Easing | pass | Results count-up, the stamp and the zeppelin burn-off are all eased. |
| 10 | Sound | minor | The victory fanfare is **cut again** by the switch to results music: hard silence at 7:13.8 (433.79-433.83 s). Before it, 15 s of the dive (6:24-6:39, 409-424 s) have only an engine/rumble bed: the boss music ends, nothing replaces it. |
| 11 | Camera jitter | **fail** | In the canyon the chase camera **dips below the water surface** (4:24.7-4:25.3; see #1). |
| 12 | Frame pacing | **fail** | **Half-drawn frames on screen**, 4:24.7-4:25.3 (#1). |
| 13 | Sorting | pass | Smoke columns and clouds sort right. |
| 14 | Colour/contrast | minor | Rocks at the waterline are **black untextured cubes** (2:05, 4:25, 9:15). The low-altitude jungle is a smeary brown/green blur (5:00, 0:40). |

## Ranked fixes

### 1. [render][game] Showstopper: half-drawn frames on screen (4:24.7-4:25.3)
Right after ring 0 ("RING +250"), for ~0.6 s, the lower part of each picture shows a frame with **only the terrain
drawn**: no water, no plane, no HUD. It is the canyon bed in olive sand, with green vapour ribbons. The split line
climbs from the bottom to the top of the screen over ~8 captured frames, then the picture recovers. It is real GPU
output (it is our scene in mid-render, not capture-card garbage), and the frame-time log shows nothing (19 ms max).
- [render]: a frame is being **scanned out while it is still being drawn**. Check the flip/swap: the front-buffer
  index, `pb_wait_for_vbl`/flip-stall ordering, and whether a frame can be presented before `pb_finished` (or triple
  buffering reusing a buffer still on screen). Rule: never draw into a buffer that is being scanned out.
- [game] (camera.cpp): the camera went **below the water surface** in the canyon, which is why the half frame shows
  the bed and no sea. `CAMERA_CLEARANCE` must also apply to the sea plane (y = 0) and canyon water: keep the camera
  ≥2.5 m above max(terrain, 0).

### 2. [game] Wave 3 ("Scar's harbour raid") never happens on screen
3:25-4:48 ("DEFEND THE HARBOUR 0/4", 80 s): the autopilot flies the canyon rings 2,600 m from the raiders. The
player then dies (mission 4:29), and **all 4 wave-3 planes "crashed into the ground" within 0.25 s** at mission 4:31.
The objective clears itself, and Scar is never seen.
- Find the bug: 4 simultaneous ground crashes right after the player's death look like a respawn-time reset or an
  AI ground-avoid failure (all in one formation near the harbour?). A pirate must never die from anything but the
  player, or a scripted hazard the player caused.
- The autopilot (also the attract demo) must go to the current objective: the harbour in wave 3, not the rings.
- Raid content: 3 banana boats in the harbour that the raiders strafe (each at 0 health = burns + sinks, −score).
  Scar flies purple-black with a white scar stripe and taunts in the radio.

### 3. [content] The harbour is still invisible
The intro (0:16-0:20 and again at 7:25) shows a 140 m mole with the lighthouse in open water and an empty coast.
The 77 huts are not in any frame I sampled.
- Move the intro camera so it starts **behind the village looking out to sea**: the village in the lower third, the
  mole and lighthouse in the middle, the plane flying over the roofs at 40 m. The village must fill ≥20 % of the
  first frame.
- Huts at a readable scale: 6-9 m wide, roofs `C4502E` with a 0.6 m overhang, a dock and 3-4 boats against the mole.
- The intro's plane is cut in half by the left screen edge at 0:18.5. Keep it inside the centre 70 %.

### 4. [content][render] Low-altitude terrain is a blurred photo
0:40, 5:00, 5:05: below 150 m the ground is soft green/brown smears with no texture. The detail texture is not
readable.
- Detail texture: ±20 % luminance, tile every 16 m (not 24 m), sharper. LOD bias −0.5 on that stage only.
- Add a second "jungle canopy" detail: 128x128, dark blobs `1F5A26` on `2F7A2E`, tiled every 8 m, faded in below
  200 m altitude. This one texture is what sells "jungle" at speed.
- Palms in the interior: 30-60 scattered palm clusters on the jungle flats near the beaches, so low flight has
  objects whipping past.

### 5. [audio] The fanfare cut and the dive's dead air
- The results music must start **after** the fanfare's release, or crossfade over 1.0 s. No hard cut (433.8 s).
- During the zeppelin's 33 s dive (from mission 6:15): bring the boss theme into a slow "triumph" variation. Or keep it
  playing at −6 dB and add a timpani roll leading into the splash. 15 s of engine drone is dead air at the game's
  climax.

### 6. [game] The zeppelin's fall is too long and too empty
Going down at mission 6:15, sunk at 6:48: 33 s. Only ~6 s of that is the burn-off; the rest is a bare cage drifting
slowly.
- Make the whole death last 18-20 s: 6 s burn-off, a 9 s dive at a nose-down angle of 25-35°, and a splash.
- During the dive: an explosion every 1.2-1.8 s along the keel, gondola and engine pods breaking off and falling
  separately (2-3 pieces with smoke trails), and the tail fin tearing off 4 s before impact.
- Camera: 3 cuts (alongside, below looking up, then the long shot of the splash with the island behind). Hold no shot
  longer than 7 s.

### 7. [content] Canyon details that look broken
- The pale cream triangular wedges on the walls (1:45, 2:05, 4:40, 9:00): remove them, or make them real scree:
  rock-coloured `6A4F42`, rounded, ≥10 triangles each, sitting on the waterline.
- Rocks at the waterline (2:05, 4:25, 9:15): black boxes. Make them lumpy (an icosphere ×1, noise-displaced), rock
  texture, 3-8 m, colour `5A4A40` with the same strata tint as the walls.
- Strata on the open hills (1:03, 2:00, 5:05): the same bands appear on gentle slopes as a staircase. Apply the bands
  only where the slope is >45°, and fade them over 5° of slope.
- The arch: thicken it to 8 m with a rounded section and the wall's strata texture (now a flat brown slab at 1:50).

### 8. [game] Intro title card and radio
At 0:16 the radio box and the "RAID ON / ISLA PLATANO" card overlap. Delay the first radio line until the card fades
(0.5 s after it ends). Wrap radio lines at word boundaries that keep short phrases together ("GET 'EM!" on one
line).

### 9. [game] Accuracy 16 %
For an arcade game, 16 % means the gun misses most of the time. That makes kills slow and the guns feel weak.
Raise the bullet hit radius vs fighters to 2.5 m (or 3 m for aces in rolls), or add 5-10 % aim assist toward the
lead circle inside 4° of the reticle. Target: 30-40 % accuracy for a decent player.

### 10. [render] Enemy planes at mid range
At 2:55-3:10 and 5:15-5:25, enemies at 300-700 m are thin dark slivers, still hard to read. Add a **contrail or
coloured smoke puff** when an ace or wave leader turns hard (wingtip vapour in its livery colour, 1.5 s), and keep
the rim term on enemies at 0.35.

### 11. [game] Respawn
Twice in this run the player died in the canyon flying rings (2:07 and 4:29 mission). The "SHOT DOWN" screen over
a smoke cloud next to a hill (2:25) works. But the respawn must place the plane over open sea toward the current
objective, not in the canyon.

## What's between this and a 10
1. Zero glitches: no half frames, no camera in water, no cut sounds (#1, #5). Non-negotiable.
2. The harbour as the heart of the level: visible, alive, defended, with the intro framing it (#2, #3).
3. Land that holds up at 50 m: canopy detail, interior palms, scree that looks like rock (#4, #7).
4. A finale that is cut like a film: 20 s, three shots, things breaking off, music swelling into the splash (#5, #6).
5. Guns that feel deadly and enemies you can read at 500 m (#9, #10).
6. Then, finally, a human at the pad: the stick feel, the stunts and the camera under real input are the last 1.5
   points, and no autopilot recording can earn them.
