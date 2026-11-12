# Review 6: polish round 5 on hardware (480p)

Material: `captures/hw/game_round6.mp4` (605 s, 480p progressive, autopilot, real Xbox, own audio), serial log
`captures/serial/serial-20261111-121213.log`, master 0456c29. Timestamps are **video time**; video ≈ mission time + 25 s
("VICTORY!" for mission 7:15 is on screen at 7:40).

**Score: 8.5 / 10** (round 5: 8.0). **I am happy as a clam.** The three items I set as the bar are delivered
(below). In 480p the image is crisp: HUD text, the briefing and the zeppelin's girders are all sharper than in any
earlier round. What's left is the road from "a good 2003 Xbox game" to "a great one". None of it is cheap-looking
any more.

## The three conditions from review 5
1. **Splash shot: delivered.** 7:30-7:42: the dive is filmed close (the cage fills a third of the frame against the
   volcano). Impact with a fireball, then a white-grey column ~45 % of the screen height, burning debris on the
   water, and "VICTORY!" ~1 s after the peak. That is a climax.
2. **Wreck under water: delivered.** No cage visible under the sea surface after the impact (7:38-7:44).
3. **Pacing: delivered for wave 1, acceptable for the boss.** Wave 1 is 47 s in two pairs (mission 0:06-0:53),
   right. The boss ran 3:10 (target 2:00-2:30), but **it does not drag**. There's a kill or an engine every 8-15 s,
   12 escorts in pairs, the armour phase with the 2 aces (6:20-6:55 video), and a death + respawn inside it. The only
   slack is the ~30 s from "armour off" to the last engine (mission 6:30 → 7:00). See #3.

## Also fixed (must not regress)
- **Briefing** (0:15): every label in big text, coloured A/B/X/Y, black and white buttons labelled (CLOSE CAM /
  CHASE CAM), and "PRESS (A) TO FLY" is clear. This is now a proper controller screen.
- **Tower cut** (4:26-4:27): the zeppelin slides out from behind the pirate rock with the radio "There she is - the
  Silverback!". The right shot.
- **Guns**: dogfight spectrograms now show broadband 2-6 kHz bursts for each burst of fire (e.g. 3:17-3:24). The
  gun is audible over the engine.
- **Audio**: −16.5 LUFS integrated, peak −4.2 dBFS, **no silence anywhere** after boot (the results crossfade is
  seamless).
- **Frame rate**: 59.9 fps on 52 of 54 stat lines.

## §12 detail checklist

| # | Item | Grade | Evidence |
|---|---|---|---|
| 1 | Edges/aliasing | pass | 480p: clean. |
| 2 | Z-fighting | pass | — |
| 3 | Seams | pass | — |
| 4 | Pop-in | pass | — |
| 5 | LOD/fog | pass | — |
| 6 | Shimmer | pass | — |
| 7 | Font/kerning | pass | — |
| 8 | HUD alignment | minor | "VICTORY!" overprints the radio box ("...IS FREE!VICTORY!", 7:40-7:42). |
| 9 | Easing | pass | — |
| 10 | Sound | pass | No gaps, no cuts. |
| 11 | Camera jitter | pass | — |
| 12 | Frame pacing | minor | 2 single slow frames (33/35 ms). |
| 13 | Sorting | pass | — |
| 14 | Colour/contrast | pass | — |

**13 pass, 2 minor, 0 fail.** First round without a fail.

## Ranked items (8.5 → 10)

### 1. [game] The canyon chase still doesn't happen
The aces arrive at mission 1:27, but for the whole canyon run (video 1:35-2:40) the off-screen arrow reads
**2,100-2,400 m**. They never come into the canyon, so their new canyon fire is never seen, and HULL stays full. This is
the scene the level is built around.
- When the player is in the canyon and an ace is free, the ace gets a **canyon pursuit** waypoint path (the ring
  line, offset 60 m up) and closes to 150-300 m behind the player.
- Fire 2-3 bursts per pass (tracers past the canopy), and **miss on purpose** for the first 2 bursts (aim error +3°).
  The chase must be exciting, not lethal.
- Radio: "Purple tails right behind you, Red!" when the ace enters the canyon.

### 2. [game] The "VICTORY!" banner and radio box collide
At 7:40 the banner overprints the end of the radio line. Put "VICTORY!" at y = 45 % of the screen height (centre),
scale 3x, and hold the radio box until the banner is shown, or drop that radio line (the banner says it).

### 3. [game] The boss's last 30 seconds
After "armour off" (mission 6:30) the autopilot needs 30 s to hit the last engine. A human will be faster, but give
the moment a push:
- On armour off: the zeppelin turns toward the tower at 22 m/s, the radio says "She's running for the tower!", and the
  last engine glows red-hot and smokes (an easy, visible target).
- The tower gets 2 flak guns that open up while she runs (pressure on the player).
- Total boss target stays 2:00-2:30 for a human. Check with the user's own run.

### 4. [content] The tower as a fortress
4:26: from the cut it is a dark rock spike with a small hut on top. Make it a pirate base: 2-3 wooden platforms
on stilts, a hangar mouth in the rock with a lit interior (`FFD98A`), 2 flak guns, a black flag with the skull and
banana, and 2 searchlight beams (additive cones, alpha 0.15) sweeping the sky. The cut lasts 2.5 s; make it count.

### 5. [render] The two slow frames
33 and 35 ms, once each in a 7 minute mission. Find which effect was on screen and cap it. Below most players'
notice; on the list for completeness.

### 6. [content] The splash column's colour
The column is mostly grey smoke with a white core (7:40). Make the water part brighter and whiter (`F4F7F2`,
alpha 0.9 at the core), and keep the grey for the smoke rising from the wreck above it. Water and smoke must read
as two things.

### 7. [game] The escort count
12 escorts (6 pairs) in one boss fight. The 5th and 6th pairs feel like a loop. Cap it at 4 pairs, then the 2 aces
of the armour phase. The fight gets shorter without losing density.

### 8. [audio] A theme for the aces
Each ace (Knuckles, Big Mo, Scar) gets a 2 s brass sting when he arrives, a different motif each, and a taunt.
Cheap, and it gives the villains a face.

### 9. [content] Life in the village
The harbour is the heart of the level now. Add flags that flap (vertex sine), 2-3 boats moving slowly in the bay with
wakes, and gulls (6-10 white "V" billboards circling above the wharf). Things that move make a place feel alive.

### 10. [user] The stick-feel pass
The remaining 0.5 point can only come from the user's hands: turn rates, auto-level, stunt timing, camera lag under
real input. Ask the user for one evening's play with notes, and tune from that, not from the autopilot.

## Happy as a clam?
**Yes.** Happy as a clam. The bar from round 5 is met, the checklist has no fail, and nothing on screen looks cheap
to a 25-year Xbox veteran. What stands between this and a 10 is #1 (the canyon chase), #3/#4 (a finale with a place
to run to), the village's life, and the user's own stick-feel notes.
