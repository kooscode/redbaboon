# Review 4: polish round 3 on hardware

Material: `captures/hw/game_round4.mp4` (565 s, autopilot, real Xbox, own audio), serial log
`captures/serial/serial-20261111-073445.log`, master d41c276. Timestamps are **video time**. Video ≈ mission time + 15
s: the "VICTORY!" banner (mission 8:15) is on screen at 8:30. The **briefing/controls screen is not in this recording**
(autoplay skips it), so it is not graded here. The user's own test drive ("looks ok", no complaints about stick feel)
counts for more than anything I can judge from autopilot footage.

**Score: 7.5 / 10** (round 3: 7.0). The showstopper is gone (no half-drawn frames anywhere in 565 s). Wave 3 is a real
fight with Scar (4:20-5:00), and guns now kill: accuracy went from 16 % to 58 %, with kills at 150-300 m that read
(6:10). The game now looks shippable from altitude. **Not happy as a clam yet**: three cheap tells remain that any
OG veteran spots in the first minute. The harbour is still not framed in the intro, palms at a distance look like
dead flies, and the boss fight sags for 75 s. Fix #1-#6 and this is an 8.5.

## What's fixed (must not regress)
- **Torn frames:** gone. No camera below the water in the canyon (1:30-2:15).
- **Frame rate:** 59.9 fps on 53 of 55 stat lines. Two single slow frames (31 ms at mission 2:19, 33 ms at 8:10
  during the zeppelin's fall, both with gpu wait ~12 ms). Acceptable, but see #10.
- **HUD for a new player:** the DAMAGE/BOOST bars are big and readable (0:20). The one-time tips ("R-STICK UP: BOOST
  DOWN: BRAKE", "LT: ROCKETS", "A + STICK: LOOP / ROLL / 180", 0:20-0:30) are short and in the right place (below
  the plane). Radio boxes no longer collide with the title card.
- **Wave 3:** "DEFEND THE HARBOUR 0/4" → 4/4 with Scar's taunt and "SCAR +1000" (3:50-5:00). It works.
- **Canyon:** no more wedges, lumpy boulders, rounded arch (1:30-2:15). The canyon run is now the best-looking
  stretch of the game.
- **Zeppelin finale:** ~15 s, breakaway pods and turret, three cuts (8:15 alongside, 8:20 underside against the sun,
  8:25-8:30 long shot with the island). The structure is right.
- **Audio:** −16.7 LUFS integrated (target −16 ±1), peak −4.3 dBFS, no dropouts after boot. The dive has a sound bed
  now: no dead air.
- **Results:** 20 kills, 58 %, SILVERBACK stamp, green A glyph on "CONTINUE" (8:35-8:40).

## §12 detail checklist

| # | Item | Grade | Evidence |
|---|---|---|---|
| 1 | Edges/aliasing | minor | Palm trunks/fronds at 150-400 m alias into flickering black specks (0:25-0:35, 8:55-9:20). |
| 2 | Z-fighting | pass | None seen. |
| 3 | Texture seams | pass | Canyon is clean. |
| 4 | Pop-in | pass | None seen; plane/zeppelin LOD switches are invisible. |
| 5 | LOD/fog | pass | — |
| 6 | Shimmer | minor | The volcano's ash slope shows the detail texture as a **visible repeating blotch pattern** (7:25, 7:35). |
| 7 | Font/kerning | pass | — |
| 8 | HUD alignment | pass | The bars, rockets and score are on a clean grid. |
| 9 | Easing | pass | — |
| 10 | Sound | minor | At the results screen (~8:37.5, 517.5 s) every band stops in one frame: the fanfare's last chord and the scene bed are **cut, not faded**. No silence gap, but a hard edge on the spectrogram. |
| 11 | Camera jitter | pass | — |
| 12 | Frame pacing | minor | 2 single slow frames (31/33 ms). |
| 13 | Sorting | pass | — |
| 14 | Colour/contrast | **fail** | The zeppelin's "silver" hull renders **near-black** whenever the sun is behind it (6:10, 6:45, 8:15). That is most of the boss fight. |

## Ranked fixes

### 1. [content][game] The harbour is STILL not in the intro (third round in a row)
At 0:15 the camera is at grass height beside a hillside, the plane is in profile, and the only buildings are 3 roofs
clipped by the bottom-right corner. At 0:20 gameplay starts over open sea with the mole and lighthouse alone. From
altitude (4:05) the village reads as a dotted strip.
- Intro shot 1 (0-2 s): the camera is 30 m above the wharf looking south-east along the waterfront. Townhouses fill
  the lower 40 % of the frame, the steamer and cranes are mid-frame, the lighthouse is at the end of the mole. The
  plane roars in over the camera, 15 m above the roofs.
- Shot 2 (2-4 s): chase from behind as the plane climbs out over the mole with the island behind.
- Check it with a still at 0.5 s and 2.5 s. If the village is under 25 % of the frame, it fails.

### 2. [content][render] Palms at a distance look like dead flies
0:25-0:35 and 8:55-9:20: hundreds of dark 2-4 px specks and twigs on the grass and beach. At 300 m a palm is its
trunk (dark) and a few thin fronds, which alias away. It reads as dirt on the screen, the worst cheap tell left.
- Beyond 150 m, draw a palm as **one camera-facing impostor**: a 10 m star of fronds, colour `3E8E3A` with tips
  `7BB043`, a trunk at most 1 px, alpha-tested with mips (alpha-to-coverage style: keep mip alpha ≥ 0.5 coverage).
- Beyond 600 m, don't draw palms at all; let the terrain's vertex colour carry them (a darker `2A6B2C` patch under each
  cluster, baked into the terrain colours).
- Cluster them: 70 % of palms in clusters of 5-12 on the beaches and around the village, not evenly sprinkled
  over the hillsides.

### 3. [render][content] The silver zeppelin renders black
6:10, 6:45 and 8:15: the hull is dark charcoal `2A2C30`-ish when backlit, so the boss reads as a black blimp.
- Hull material: albedo `B4BAC0`, ambient term from the sky (hemisphere: sky `9DB8D8` from above, sea `3F6F8F` from
  below) at ≥ 0.55, sun diffuse on top of that. The darkest side must never go below `6A7078`.
- Specular 0.6 with a broad highlight (exponent 12) along the top. Silver must glint as it turns.
- Keep the red/black band and the skull roundel. They are what make it read as "pirate".

### 4. [game] The boss fight sags: 3:14, of which 75 s is empty
Engines go at mission 5:44, 6:23 and 6:40. The player dies at 6:43 and respawns at 6:46. The last engine goes at
**8:00**. From 7:00 to 7:45 (video) the plane flies over the volcano with the zeppelin 1,100-2,000 m away and nothing
happening. All 6 escorts have launched by 6:02, so after that the boss is a balloon.
- Respawn during the boss: 700-900 m from the zeppelin, heading at it, at its altitude.
- When the 3rd engine dies: "She's turning for the tower!" The zeppelin speeds up 15 → 22 m/s. The **last
  engine glows/flashes** (emissive `FF6A1A` pulsing at 2 Hz), and 2 aces launch together.
- Target boss length for a decent player: **2:00-2:30**, with no 20 s stretch without a target inside 600 m.

### 5. [audio] The cut at the results screen
517.5 s: everything stops in one frame. The screen change must crossfade: 0.6 s out for the scene's sound, the fanfare
plays out its release (or is faded by 1.0 s), and the results music fades in over 0.8 s. Never cut voices at a
screen change. Make that a rule in the mixer (all non-UI voices release over ≥ 40 ms on stop).

### 6. [game][content] Stunt score spam and stunt cost
39 stunts in one mission, mostly barrel rolls 2-3 s apart (mission 2:09-3:26, a "+100" popup every few seconds).
For the autopilot that is a test artefact. For a human it is a score farm, and it cheapens the stunt system.
- Brief §11: **stunts must cost something**. Each stunt uses 25 % of the BOOST meter; no stunt below 25 %.
- Points: +100 only if an enemy shot at you in the last 2 s or if you are within 300 m of an enemy. Otherwise +10.
  Same stunt again within 5 s: no points.
- Rank the autopilot down too: it should roll only when shot at.

### 7. [game][content] Zeppelin splash is too small
8:30: the cage hits the sea nose-first and makes ~6 small white puffs (≈15 px). The climax ends with a fizzle.
- Impact: a **60-80 m white column** (30-40 billboards `F4F7F2`, rising 2 s, falling 3 s), a ring of spray 120 m
  across, a final orange blast inside the column, and steam `D8DCDF` drifting for 6 s.
- Hold the long shot 1 s past the column's peak before "VICTORY!".

### 8. [game] Smoke from breakaway pieces is still spider legs
8:15-8:20: the pods' trails are long 1-2 px grey lines (same issue as review 2 #7, now on the zeppelin pieces). Use the
plane trail emitter: billboards 3 → 10 m, alpha 0.5 → 0, every 0.04 s, a black core with fire for the first 1.5 s.

### 9. [game] Wave 1's dead 40 s
Fighter 1 dies at mission 0:09, fighter 2 at 0:52. From 0:30 to 1:00 (video) the plane flies over the same green
hill with "DOWN THE RAIDERS 1/4" and no enemy in view. The second, third and fourth raiders must turn onto the
player within 10 s of the first kill and engage inside 600 m by mission 0:20. The first 60 s of the game must be the
busiest, not the emptiest.

### 10. [render] Volcano ash and the two slow frames
- The ash slope (7:25, 7:35) shows the detail texture's tiling as a blotch grid. On ash, use a second detail scale
  (tile 64 m, ±8 %) and lower the 16 m tile's contrast to ±10 %. Add 6-8 dark lava-rock streaks (`3B3330`) running
  downhill from the rim.
- Two 31-33 ms frames at mission 2:19 and 8:10, both with gpu wait ~12 ms (fill rate: big smoke billboards close to
  the camera). Cap the screen-space size of a smoke billboard at 35 % of the screen height, and fade billboards
  within 6 m of the camera.

### 11. [game] DAMAGE bar semantics
The bar is full green when the plane is **undamaged** (0:20) and empties as damage grows. Read literally, "DAMAGE"
with a full bar means "fully damaged". The user asked for clarity, so keep the word, but make the bar mean what it
says. Either:
- Label it **ARMOR** (full = good), or
- Keep **DAMAGE** with a bar that *fills* from empty, green → amber → red, as damage grows.

I'd choose the second, since the user picked the word. Ask the user before changing it.

### 12. [game] Wave 3: show the raid
"Defend the harbour" works as a fight, but I never see the harbour being hurt. Make the raiders strafe the wharf:
2-3 fires on boats/sheds while the wave is alive (fire billboards + smoke columns 30 m), extinguished (smoke only)
after the wave. The player must see what he is defending.

## Not reviewed (send a still next round)
The briefing screen with the controller diagram, the pause menu's CONTROLS page, and the "PRESS A TO FLY" prompt
with the green A glyph. Those were the user's own complaints, so I want to grade them directly.

## Happy as a clam?
**No, not yet.** Between this and that:
1. The harbour framed in the intro (#1). Third round flagged; no more excuses.
2. Palms that read as palms at 300 m (#2).
3. A silver zeppelin that stays silver (#3).
4. A boss fight with no empty minute (#4), ending in a splash worthy of it (#7).
5. No hard audio cuts at screen changes (#5).

After those five, I'd be happy. Then the road from 8.5 to 10 is content density (village life, more set pieces in
the canyon and at the tower) and human-tuned stick feel, which only the user's hands can judge.
