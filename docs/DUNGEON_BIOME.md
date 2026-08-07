# The dungeon biome — spec

Status: **proposal, not built.** Written 2026-08-07.

Every other biome is a *landscape you watch*. This one is a *story you
follow*, and that difference drives almost every decision below. It is the
first biome where the viewer is meant to ask "what happens next" rather than
"what is it doing".

---

## 1. The one constraint that decides everything

**A character is one pixel.** At 1px/cell on a 140-LED disc, the hero is a
single lit dot. Everything in this spec is downstream of that.

You cannot tell a story with sprites here. You tell it with **colour,
motion, and consequence**:

| Cue | Carries |
|---|---|
| **Hue** | who someone is (fixed, learnable across many viewings) |
| **Motion signature** | what kind of thing they are |
| **Brightness / pulse** | their state — hurt, afraid, powerful |
| **What changes in the world** | that something happened |

Motion is the strongest of these and the most under-used elsewhere in the
project. The UFO in the sky biome already proves the principle: it is
recognisable *because it hovers and darts*, not because of its shape. Apply
the same thinking here.

Proposed motion signatures:

- **Hero** — purposeful. Moves in straight committed runs toward unexplored
  space, pauses at junctions (a visible *decision*), never retraces without
  reason.
- **Villain** — patient. Barely moves. Occupies a deep room and *waits*.
  When it does move it is faster than anything else on screen.
- **Small monster gangs** — jittery, clustered, always 3–6 together, never
  alone. Individually erratic, collectively coherent.
- **Big monster** — slow, heavy, and it does not path around things. It goes
  *through*. Its motion is the only one that changes the map.
- **The captive** — does not move at all until freed. A single stationary
  point of a distinctive hue, deep in the map, is a **question the viewer
  holds** for as long as it takes.

That last one is, I think, the strongest idea available here: a still pixel
that the viewer learns to read as *someone waiting* is enormous narrative
value for almost no rendering cost.

---

## 2. Pacing: it must never finish

The vat runs for days in a gallery. A dungeon that gets completed and then
sits solved is dead screen. Three candidate structures:

1. **Regenerate on completion** — the hero wins, the picture fades, a new
   dungeon carves itself. Clean but makes the ending a *reset*, which
   deflates it.
2. **The dungeon keeps growing** — new wings carve themselves at the edges
   faster than they can be explored. No completion state exists. Endless,
   but nothing ever resolves, so nothing ever *matters*.
3. **Episodes with consequence carried forward** (recommended) — a run ends
   (the hero wins, dies, or flees), the dungeon regenerates, but something
   *persists*: the dead hero's remains lie where they fell in the new
   layout, the villain that won is stronger next time, a rescued captive
   becomes the next run's hero.

**Recommendation: 3.** It gives you both an ending and a reason to keep
watching, and it is the only one where the piece accumulates meaning over a
long exhibition rather than looping. It also matches how the rest of the
vat already thinks — the world has seasons and memory, not levels.

Suggested run length: **8–25 minutes**, varying. Long enough to develop,
short enough that a gallery visitor sees a whole arc.

---

## 3. The generation is part of the show

Do not generate the dungeon and then reveal it. **Carve it live**, visibly,
before anything moves in: rooms bloom, corridors reach out and connect. It
costs nothing extra (the carve is the same algorithm either way) and it buys
a genuine opening act — the world building itself is a thing worth watching,
and it tells the viewer a new run has started without any text.

Method: rooms placed with spacing, then corridors carved along a spanning
tree with a few extra loops added (a pure tree makes exploration
predictable and dead-endy; loops are what make a dungeon feel like a place).
Carve ~1 room/second so the opening runs 30–60s.

Fog of war: **yes, and it is the whole reason this reads as exploration.**
Unexplored cells are dark. The hero reveals as it moves. The viewer learns
the map *at the same rate the hero does* — which is what makes an encounter
land, because you did not see it coming either.

Rooms should have identity, keyed by their content, the same way city roofs
key off height: a treasury, a flooded room, a nest, a shrine, a collapsed
hall. That is what stops a dungeon reading as corridors and boxes.

---

## 4. What already exists (build on this, do not reinvent)

The sim already has most of the substrate:

- `Agent` with `x/y`, `species`, and vitals — `hunger`, `thirst`,
  `fatigue`, `stress`, `health` — plus `Mood` and `Intent`.
- `Intent` already includes `WANDER / FORAGE / DRINK / FLEE / HUNT / REST`.
  A dungeon needs roughly: `EXPLORE / HUNT / FLEE / GUARD / RESCUE`. Two of
  those exist; `FLEE` and `HUNT` are exactly right already.
- Eight species archetypes (`SPEC_PACKHUNTER`, `SPEC_SWARMER`,
  `SPEC_MYSTIC`…) which map onto monster gangs almost directly.
- Predator/prey pressure, panic flags, and the whole mod-matrix plumbing.

**What is genuinely new:** the roles (hero/captive/villain are not species,
they are *parts*), the fog of war, the room semantics, and encounter
resolution.

Verdict: this is maybe 30% new machinery and 70% dressing existing agents in
a new role. That is a much smaller job than it first looks — but see §7.

---

## 5. Encounters

The emergent bit, and the part most likely to disappoint if done naively.
A fight resolved by invisible dice is not drama; the viewer sees two dots
touch and one vanish.

Make encounters **legible and slow**:

1. **Approach** — both parties visible, closing. The viewer gets to
   anticipate. This is most of the value and costs nothing.
2. **Stand-off** — a beat where both hold. Pulse both agents. Perhaps a
   full second. This is the single cheapest way to make a fight feel like
   an event.
3. **Resolution** — brief, bright, local. A flash, a scatter.
4. **Aftermath** — and this is what sells it: the loser *stays on the map*
   as a mark. The world remembers.

Outcomes should not be symmetric. A hero meeting a gang should usually win
but be **hurt** (health drops, movement slows, and the viewer can see it),
so the hero's condition is a running story across the whole run rather than
a coin flip at the end.

---

## 6. Music

This is where the biome earns its place, because it is the first one with
actual *narrative* structure to modulate from. Everything else in the vat
modulates on ambience — how much water, how much city, how windy. Here the
music can have **stakes**.

New mod sources (append at 70+, never insert — saved patches address slots
by index):

| source | meaning |
|---|---|
| `dungeon_depth` | how deep the hero is — drives register down, reverb up |
| `dungeon_known` | fraction of map revealed — a slow build across a whole run |
| `dungeon_threat` | proximity of the nearest hostile to the hero |
| `dungeon_peril` | hero health inverted |
| `dungeon_encounter` | 0→1 spike through approach/stand-off/resolution |
| `dungeon_captive` | is the captive still waiting (an unresolved chord) |

`dungeon_threat` is the good one: a source that rises as something
approaches, before the viewer can see why, is *dramatic irony in a
modulation slot*. The music knows first.

Musical identity: low register, long silences, single struck notes with
long decay. The dungeon should be the **quietest** biome — silence is what
makes the encounter spikes land. Minor and modal; no resolution until a run
ends.

**Critical constraint, learned the hard way:** all of these must be computed
in `updateModPool` from **sim state only**, never renderer state. In the
plugin the UI only runs while an editor is open, so a renderer-derived
source reads zero whenever nobody is looking. See `skyFlyerUp` /
`alienApparition01` for the established pattern.

---

## 7. Honest risks

- **Legibility is the whole gamble.** If hero/villain/gang are not
  instantly distinguishable at one pixel, this becomes coloured noise that
  moves. Everything else is easy by comparison. **Mitigation: prototype the
  motion signatures and the palette first, in the contact-sheet harness,
  before building any dungeon logic at all.** If a still frame plus a few
  seconds of motion cannot answer "which one is the hero", stop.
- **It breaks the vat's grammar.** Every other biome is contemplative and
  has no protagonist. A story biome in the voyage rotation may feel like a
  different artwork spliced in. This might be a feature (a punctuation mark
  in a long drift) or might be jarring — it needs judging on the panel.
- **Scope.** This is comfortably the largest biome: generation + fog +
  roles + encounters + music. Realistically several sessions, not one.
- **Fog of war and the round panel.** Most of the disc being dark most of
  the time may look broken rather than atmospheric on a 140-LED panel with
  OLED ground mode. Needs an early test.

---

## 8. Suggested build order

Each step is watchable on its own, which matters — none of this is
verifiable except by looking.

1. **Palette + motion prototype.** No dungeon. Five dots with the proposed
   hues and motion signatures on a blank field. Answer the legibility
   question before anything else.
2. **Live carve + fog.** The dungeon builds itself and a single explorer
   reveals it. Already watchable, and settles the fog-on-panel risk.
3. **Roles and encounters.** Gangs, villain, captive; approach/stand-off/
   resolution; marks left behind.
4. **Music and mod sources.**
5. **Episodes and persistence** (§2.3).

---

## 9. Open questions for the owner

1. **Fantasy or something stranger?** The vat already has an alien biome
   that is not Earth-with-different-colours. A dungeon could be a stone
   keep, or it could be something the alien world would build. The cast
   names (hero, damsel, villain) suggest classic fantasy — is that the
   intent, or is it shorthand for the *roles*?
2. **Should the viewer ever be able to tell who wins in advance?** Fully
   emergent means sometimes the hero dies in the second room and nothing
   happens for ten minutes. Some authorial weighting (the hero *tends* to
   survive to the third act) may make better viewing. How much of a thumb
   on the scale is acceptable in a generative piece?
3. **Does it join the voyage rotation** (`--drift`), or is it a place you
   deliberately go? A 20-minute story that gets interrupted by a crossing
   at minute 6 is worse than not having it.
