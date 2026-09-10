# The meteorology of Storm

*What the simulation is actually modelling, why the storm goes through the
stages it does, and where the model stops being physics and starts being
stagecraft.*

This document is for someone who wants to know what they are looking at. It
assumes no meteorology and no graphics programming; every term of art is
explained the first time it is used. The numbers quoted are the ones the
program actually runs on — they live in `Sounding` and `StormArc` in
`src/simulation.h`, and the measurements come from the `/arc` harness described
in the README.

Three parts:

1. **[The physics that is really there](#part-1--the-physics-that-is-really-there)**
   — the atmosphere the solver integrates, and why a storm happens in it at all.
2. **[The variables, and what each one does](#part-2--the-variables-and-what-each-one-does)**
   — the sounding vector as a set of dials, and which stage of the storm each
   one controls.
3. **[Where it departs from reality](#part-3--where-it-departs-from-reality)**
   — the compressions, the shortcuts, and the parts that are simply drawn
   rather than simulated.

---

## Part 1 — the physics that is really there

### The one idea everything rests on: buoyancy

A thunderstorm is a buoyancy engine. Warm air is less dense than cold air at
the same pressure, so a warm parcel in cooler surroundings rises, and a cool
parcel in warmer surroundings sinks. Everything in the storm — the tower, the
anvil, the rain shaft, the cold air spreading out at the ground — is that one
statement applied over and over.

The complication is that air cools as it rises, simply because it expands into
lower pressure, without losing any heat to its surroundings. Dry air cools at
almost exactly **9.8 K per kilometre** of ascent. So "is this parcel warmer
than its surroundings?" is not a question you can answer by comparing raw
temperatures at different heights — the parcel's temperature is falling as it
climbs whatever else happens.

Meteorology solves this with **potential temperature**, usually written θ
("theta"): the temperature a parcel *would* have if you brought it down to
sea-level pressure. Expansion cooling leaves θ unchanged, so a rising parcel
carries its θ with it like a label. Comparing a parcel's θ to the θ of the air
at its current height answers the buoyancy question directly, and that is
exactly what the solver does. From `sim.hlsl`:

```hlsl
float bAbove = kGravity * ((above.r - thetaEnv(yAbove)) / kTheta0
                         + 0.61 * (above.g - vapourEnv(yAbove)) - above.b - above.a);
```

Read that as: *acceleration = gravity × (how much warmer this parcel is than
its surroundings, as a fraction, plus a small correction for how much moister
it is, minus the weight of the cloud water and the rain it is carrying)*. The
`0.61` term is real physics — water vapour is lighter than dry air, so humid
air is buoyant on its own account. The two subtractions are **condensate
loading**: cloud droplets and raindrops are dead weight the parcel has to lift,
and they are half of why a downdraft exists. `kTheta0 = 300.0` is the reference
temperature, roughly 27 °C.

### The atmosphere as a sounding

A **sounding** is what a weather balloon measures: temperature and humidity as
a function of height, plus the wind. It is the single most important object in
severe-weather forecasting, because it tells you what a storm would do if one
started. Storm's entire atmosphere is one such profile, in the `Sounding`
struct, and it is deliberately the *only* place the atmosphere is described —
no physics routine anywhere carries a tuned constant of its own.

The profile has three layers.

**The mixed layer** (ground to `mixedTop = 900 m`). Through the day the sun
heats the ground, thermals churn the lowest kilometre or so, and it ends up
stirred to near-uniform potential temperature and near-uniform humidity. The
code sets `lapseMixed = 0.0005` K/m — half a kelvin of θ per kilometre, which
is very nearly neutral. This is the layer the storm breathes from, and its
uniformity is why a cumulus field has **one flat base** rather than a base per
cloud: every parcel in the layer carries the same amount of vapour, so every
parcel condenses at the same height.

**The free troposphere** (900 m to `equilibrium = 9200 m`), at
`lapseTropo = 0.0034` K/m — 3.4 K per kilometre of potential temperature. That
corresponds to an actual temperature falling at about 6.4 K/km, close to the
textbook average of 6.5. This layer is **conditionally unstable**, the single
most important phrase in the document, and it is unpacked below.

**The stratosphere** (above 9200 m), at `lapseStrato = 0.0230` K/m. A steep
increase in potential temperature means strongly *stable* air: anything pushed
up into it becomes colder than its surroundings immediately and is pushed back
down. This is the ceiling. In the real atmosphere it is the tropopause, and it
is why thunderstorm anvils are flat.

### Latent heat: the engine

Air can hold only so much water vapour, and the limit falls sharply with
height, because it falls with temperature. The code models the limit as a
simple exponential:

```hlsl
float satVapour(float y) { return gSatSurface * exp(-y / gSatScale); }
```

with `satSurface = 0.0210` and `satScale = 2200` m. Moisture is measured as a
**mixing ratio** — kilograms of water per kilogram of air — so 0.0210 is 21
grams per kilogram, roughly what saturated air holds at 25 °C at sea level.
With `surfaceRH = 0.65`, the mixed layer carries 13.7 g/kg, which corresponds
to a dew point near 19 °C: a properly humid summer day, and squarely in the
range that supports severe storms.

Lift such a parcel and, at the height where its vapour content exactly meets
the falling saturation limit, water starts to condense. That height is the
**lifting condensation level**, and it is the cloud base. It comes out of the
two numbers above as `-satScale × ln(surfaceRH)` = **948 m**, and the `/arc`
harness measures the actual cloud base at 1035 m — one grid cell off. That
agreement is not luck; it is the point of specifying humidity as a relative
humidity against the saturation curve rather than as an independent vapour
profile.

Condensation is where the energy comes from. Turning vapour into liquid
releases the **latent heat** that was absorbed when the water evaporated off
the ground days earlier, and it goes straight into warming the parcel:

```hlsl
static const float kLatentOverCp = 2488.0;   // K per unit condensed mixing ratio
```

That constant is the latent heat of vaporisation divided by the specific heat
of air — about 2.5 million joules per kilogram over about 1004 joules per
kilogram per kelvin. Condensing all 13.7 g/kg of the mixed layer's moisture
would warm a parcel by 34 K.

So: a parcel that is lifted a few hundred metres cools, condenses, warms
itself, becomes buoyant, rises faster, condenses more, and runs away. This is
the feedback loop that makes a cumulus *accelerate* rather than coast, and the
solver implements it in five lines of `CSMicrophysics`.

### Conditional instability, and why the sky is not always full of storms

"Conditionally unstable" means **stable to dry lifting, unstable to moist
lifting**. A parcel lifted a hundred metres and *not* condensing becomes colder
than its surroundings and sinks back. The same parcel lifted past its
condensation level starts releasing latent heat and never comes back. The
condition is whether it reaches the cloud base.

This is why a hot humid afternoon can be completely cloudless right up until it
isn't. The energy is there the whole time — forecasters call the stored energy
**CAPE**, Convective Available Potential Energy, in joules per kilogram — but
something has to lift a parcel far enough to unlock it. The textbook estimate
is that the fastest updraft a sounding can produce is `w = √(2 · CAPE)`; Storm
measures peak updrafts of 50–64 m/s, implying around 1,500 J/kg actually
realised. Real storms realise roughly half of what parcel theory promises, so
the underlying environment is a strong severe-weather sounding — not an extreme
one, but a real one.

### The cap: what turns an afternoon into a storm

The thing that usually stops parcels reaching their condensation level is a
**capping inversion**, or "the cap": a shallow layer, typically one to two
kilometres up, that is warmer than it ought to be. Often it is the leftover
mixed layer from a hot, dry region upwind. A thermal rising into it finds air
warmer than itself, stops dead, and spreads out. The result is a fair-weather
sky — flat-bottomed cumulus that never get anywhere.

Through the afternoon, continued surface heating erodes the cap from below.
When it finally gives way, everything that was being held down goes up at once.
This is the single most important piece of timing in severe-weather
forecasting, and it is what the whole of `StormArc` exists to reproduce.

In the code the cap is a step of `capStrength = 5.5` K spread over
`capDepth = 500` m, added to the environmental profile wherever the arc has put
it:

```hlsl
float capInversion(float y, float capHeight, float strength)
{
    return strength * smoothstep(capHeight, capHeight + gCapDepth, y);
}
```

The design note in `simulation.h` records the discovery that **what separates
the acts is the cap, not the forcing**. In a conditionally unstable atmosphere
a parcel that reaches its condensation level is committed — latent heat carries
it to its neutral level whatever the heat source did — so weakening the heat
source gives you a thin cumulonimbus, not a cumulus. Only a lid gives you a
cumulus.

### The top: equilibrium level, overshoot, and the anvil

A rising parcel keeps accelerating as long as it is warmer than its
surroundings. Eventually the environment's potential temperature catches up
with the parcel's, and above that height the parcel is *decelerating* — but it
is still moving at 50 m/s, so it keeps going. That crossing point is the
**equilibrium level** (also "level of neutral buoyancy"), and the distance the
updraft coasts past it is the **overshooting top**, the lumpy dome you see on
satellite imagery above an otherwise flat anvil.

Having overshot, the air falls back to the equilibrium level and has nowhere to
go but sideways. That is the **anvil** — the flat, fibrous, downwind-streaming
sheet that gives a cumulonimbus its shape and its name.

Storm places the equilibrium level at 9200 m and measures about a kilometre of
overshoot. Getting this right took a calibration sweep (the table is in the
README), and the useful half of the result was the failure mode: put the cap
*above* the parcel's own neutral level and it caps nothing, the storm tops out
on its own, and **there is no anvil, because there is nothing for the outflow
to spread under.**

### Ice: without it there is no anvil

Air at 9 km is dry — `upperRH = 0.30` — and cloud droplets pushed into it
evaporate almost immediately. Under the solver's instantaneous saturation
adjustment, "almost immediately" means within a single step. An early version
of the storm measured an 11 km tower and rendered as a bare column, because
everything it detrained was gone before it had spread a hundred metres.

Real anvils are made of ice crystals, and ice behaves differently in the two
ways that matter: it **sublimates far more slowly** than liquid water
evaporates, and it **falls far more slowly** than raindrops. Those two rates
are the anvil. The code models this with a height-dependent glaciated fraction:

```hlsl
float iceFraction(float y) { return saturate((y - gGlaciation) / 2000.0); }
```

and then scales evaporation and fallout by it: `iceEvaporation = 0.004` (ice
sublimates at 0.4% of the liquid rate) against `iceFallout = 0.00002` per
second where water gets `0.00035`.

The height matters enormously, and the code distinguishes two heights that are
easy to conflate:

- **`freezingLevel = 4000` m** — the 0 °C line. Below it, cloud droplets are
  liquid and can collide into raindrops. Above it they cannot, and rain
  formation is switched off. Without this distinction the storm converted its
  whole depth to precipitation: a 9 km column of rain with a thin updraft up
  one side.
- **`glaciationLevel = 8000` m** — where the cloud is actually *glaciated*,
  meaning fully converted to ice. In deep convection this happens near −38 °C,
  not at 0 °C, because cloud droplets supercool readily. Set the transition at
  the freezing level instead and everything detrained above 4 km becomes
  permanent, and the storm grows a pancake at 5–6 km rather than an anvil at 9
  — measured as 21,700 cloudy cells at 5.6 km against 2,700 at the top.

That one distinction decides the storm's entire silhouette.

### Rain, and where the downdraft comes from

Cloud droplets are about 20 microns across and fall at centimetres per second —
effectively suspended. Raindrops are a millimetre or more and fall at metres
per second. Getting from one to the other is **autoconversion** (droplets
colliding and merging until some are large enough to be called rain) followed
by **accretion** (falling raindrops sweeping up the droplets in their path,
which is the runaway that makes a shaft dense once it has started). Storm
implements exactly that pair:

```hlsl
float rainMade = (max(s.b - gAutoThreshold, 0.0) * gAutoRate
               + s.b * s.a * gAccretionRate) * warm * gSimDt;
```

with `autoThreshold = 0.0012`, `autoRate = 0.0025` per second and
`accretionRate = 0.5` per second per unit rain. Rain then falls at
`rainFallSpeed = 8.0` m/s relative to the air, a realistic mass-weighted
terminal velocity for moderate rain.

The important part comes next. Rain falling out of the cloud base falls through
**unsaturated air**, and it evaporates into it. Evaporation takes latent heat
*back out* — the reverse of the process that powered the updraft — and the air
it falls through is chilled:

```hlsl
float evaporated = min(s.a, gRainEvaporation * deficit * s.a * gSimDt);
s.a -= evaporated;
s.g += evaporated;
s.r -= kLatentOverCp * evaporated;      // the air cools
```

Chilled air is negatively buoyant, so it descends, dragging more rain with it.
**This is the downdraft.** It is not an aesthetic addition; it is the mechanism
by which a storm spreads a pool of cold air under its own base, and eventually
the mechanism by which an ordinary storm kills itself — the cold pool cuts off
the warm inflow the updraft was feeding on. The `/arc` measurements show
exactly that ending: condensate peaks at 1200 s and the storm is finished by
2800 s. **The storm dies of its own rain.**

### Shear: why a storm survives more than twenty minutes

If the wind is the same at all heights, the updraft and its rain occupy the
same column. The rain falls back down through the updraft, its evaporative
cooling undercuts the inflow, and the storm is over in half an hour. This is an
ordinary single-cell thunderstorm, and it is what most storms are.

**Wind shear** — wind that changes with height — separates the two. The updraft
leans downstream, the rain falls out of the tilted tower *beside* the inflow
rather than into it, and the storm can go on indefinitely. Storm's profile is:

```cpp
float shear[2] = { 4.0f, 0.0f };   // m/s per km, through shearTop = 6000 m
```

which is 24 m/s of wind change across the lowest 6 km. In forecasting terms
that is 0–6 km bulk shear of about 47 knots; the conventional threshold for
supercells is around 20 m/s (40 kt), so this is a genuine supercell environment
rather than a number chosen to look good.

The solver runs in the **storm-relative frame**: the box travels with the
storm, and what the equations see is the environmental wind *minus* the storm's
own motion. This is standard practice in storm research — forecasters compute
storm-relative helicity in precisely this frame — and here it has the practical
benefit that the heat source keeps feeding one column while the upper levels
stream past it.

### The mesocyclone, and what makes a supercell

A **supercell** is a thunderstorm with a persistently rotating updraft, and
that rotating updraft is called a **mesocyclone**. It is what separates the
storms that produce strong tornadoes from the storms that do not.

In the real atmosphere it arises like this: vertical wind shear is,
mechanically, horizontal vorticity — imagine a paddle-wheel lying on its side,
spun by faster wind above and slower wind below. The updraft tilts that
horizontal spin into the vertical, and then *stretches* it, and a spinning
column of air conserves angular momentum as it is stretched, exactly like a
figure skater pulling their arms in. The result is a mesocyclone typically
2–10 km across, rotating cyclonically — counter-clockwise in the Northern
Hemisphere.

**Storm does not do this.** Spike 04 measured that at this grid resolution the
tilting-and-stretching mechanism does not produce a usable mesocyclone, and
waiting for one is waiting for nothing. So the rotation is *imposed*: a target
swirl about the storm's axis, and the flow is nudged toward it.

What matters is that this is not merely cosmetic. A rotating updraft is
dynamically different from a non-rotating one — the low pressure at the centre
of a vortex pulls air up into it, which is why a supercell's updraft can be
sustained by shear that would tear an ordinary storm apart. The project
measured this directly:

| 0–6 km shear | 2.0 | 3.0 | 4.0 | 5.0 m/s/km |
|---|---|---|---|---|
| **cloud top, no rotation** | 9765 | 8775 | 8235 | 6885 m |
| **cloud top, rotating** | 11475 | 11385 | 11205 | 10665 m |

Unrotated, the storm loses 30% of its depth across that range. Rotating, 7%.
That is a real supercell behaviour reproduced by an artificial mechanism, and
it is the entire argument for imposing the rotation.

The swirl is a **Rankine vortex** — the standard idealisation of a real vortex:
rotating as a solid body inside a core radius, and falling off as 1/r outside
it. `rotationSpeed = 24.0` m/s peak tangential at `rotationRadius = 1800` m
gives a core vorticity of about 0.027 s⁻¹, against a conventional mesocyclone
threshold of 0.01 s⁻¹ — a strong mesocyclone, and within the observed range.

The axis leans downstream (`rotationTilt = 0.30`, i.e. 300 m of lean per
kilometre of height) because the tower it sits in leans, and it is confined
vertically to the storm layer (`rotationBase = 900` m to `rotationTop = 8000`
m) because a real mesocyclone occupies neither the boundary layer beneath the
storm nor the anvil above it.

One detail is genuinely physical rather than convenient: only the *tangential*
component of the flow is steered. Relaxing the whole horizontal velocity toward
"environment plus swirl" would erase the storm's inflow and outflow through the
very region where it does its breathing.

### The tornado, the wall cloud, and the clear slot

Below the mesocyclone, a real supercell develops the set of features that make
up the classic storm-chaser photograph:

- The **wall cloud**: a lowered collar of cloud hanging beneath the main base,
  directly under the mesocyclone. It forms because rain-cooled, moistened air
  is drawn back into the updraft and condenses at a lower height than the
  surrounding inflow. It is the thing a tornado comes out of, and a supercell
  updraft base is emphatically not flat.
- The **rear-flank downdraft (RFD)** and its **clear slot**: a downdraft that
  wraps around the back of the mesocyclone, cutting a rain-free notch into the
  precipitation. This is what lets you *see* a tornado at all — without it the
  funnel is embedded in opaque rain.
- The **vault**: a rain-free region directly under the updraft, where air is
  rising too fast for precipitation to form or fall.
- The **condensation funnel**: not the tornado itself but the visible part of
  it. Pressure inside a tornado is low enough that the air expands and cools
  past its condensation point, so the vortex draws its own cloud downward from
  the base. It descends over a minute or two, holds, and then **ropes out** —
  thinning and tilting into a contorted rope as the vortex loses its supply.
- The **debris cloud**: dust and material lifted off the ground where the
  vortex touches down, often visible before the funnel has fully descended.

**None of these are simulated.** All five are authored geometry, evaluated
analytically inside the renderer. Part 3 explains why, and what that means.

### All of it, in one picture

![A vertical slice through the mature storm along the shear: the mixed layer and cloud base, the freezing and glaciation levels, the equilibrium level with the anvil spreading beneath the stratosphere and an overshooting top above it, the tilted tower and its mesocyclone, the rain shaft and rain-cooled downdraft, and the wall cloud, funnel and debris cloud at the base](storm-structure.svg)

Everything above, on the shipped sounding, at the moment the tornado is on the
ground. The heights are the real ones and so are the widths — which is worth
dwelling on twice. The anvil reaches the edge of the domain, because it is a
20 km box and a real anvil streams a hundred kilometres downwind. And the funnel
is the sliver under the wall cloud: a kilometre tall and four grid cells across,
which is the entire reason Part 3 has a section titled *the tornado and its
entourage are drawn, not simulated*.

---

## Part 2 — the variables, and what each one does

The `Sounding` struct is a set of dials, and each one has a distinct job. The
tables below group them by which part of the storm they control.

### What sets the cloud base

| Variable | Value | Effect |
|---|---|---|
| `surfaceRH` | 0.65 | Relative humidity in the mixed layer. Directly sets the cloud base: `-satScale · ln(RH)` = 948 m. **Raise it and the base drops.** |
| `satSurface` | 0.0210 | Saturation mixing ratio at the ground — how much water the air *could* hold. Sets the total fuel available. |
| `satScale` | 2200 m | How fast the saturation limit falls with height. |
| `mixedTop` | 900 m | Depth of the well-stirred boundary layer. Must sit **just below** the cloud base — if the condensation level falls below it, the environment saturates on its own and the domain grows a flat stratus sheet instead of a storm. |
| `lapseMixed` | 0.0005 K/m | Near-neutral, so a thermal crosses the boundary layer almost for free. |

The base is the most visible single number in the frame, and the constraint
between `surfaceRH` and `mixedTop` is tight by construction: it is what makes
the base *flat*.

### What sets the depth

| Variable | Value | Effect |
|---|---|---|
| `lapseTropo` | 0.0034 K/m | How unstable the free troposphere is. Sets where the parcel's own neutral level lands, and therefore the hard ceiling on storm depth. |
| `equilibrium` | 9200 m | Where the stratosphere begins — where the anvil spreads. |
| `lapseStrato` | 0.0230 K/m | How firm that ceiling is. |
| `upperRH` / `rhTransition` | 0.30 / 6000 m | How dry the upper troposphere is, and over what depth the humidity falls to it. Dry air aloft is what kills detrained cloud — and therefore what makes ice necessary. |

The first two are **not independent**, and the project calls this the single
most useful thing it learned about direction. Where the parcel's warming curve
crosses the environment's is the parcel's own neutral level; an inversion
placed *above* that crossing caps nothing at all. At 4.0 K/km the crossing is
at 9.2 km, so a cap at 10.5 km was never reached and the plume spread at
mid-level instead. Lowering the lapse rate to 3.4 K/km moved the crossing above
the cap, and the cap became what stops the storm — which is what it is for.

### What sets the phases

| Variable | Value | Effect |
|---|---|---|
| `capStrength` | 5.5 K | How much extra warmth the lid holds. |
| `capDepth` | 500 m | How thick the lid is. |
| `arc.capCumulus` | 2600 m | Where the lid sits during act one. |
| `arc.capCongestus` | 6000 m | Where it sits during act two. |
| `arc.rampCumulus` | 0.32 | How hard the boundary layer is heated during act one. |

This group is the storm's dramatic structure, and it works by moving the lid
rather than by moving the heat. See the phase table below.

### What sets the shape

| Variable | Value | Effect |
|---|---|---|
| `shear[0]` | 4.0 m/s per km | How hard the tower leans, and how far downstream the anvil streams. |
| `shearTop` | 6000 m | The depth the shear acts through. |
| `windRelaxation` | 0.0015 /s | How firmly the environment is held against the storm's own circulation. Without it, a periodic box has nothing maintaining the shear and the storm mixes its own environment flat within a few hundred seconds. |
| `rotationSpeed` | 24 m/s | Peak tangential speed of the mesocyclone. |
| `rotationRadius` | 1800 m | The Rankine core radius — how wide the mesocyclone is. |
| `rotationBase` / `rotationTop` | 900 / 8000 m | The storm layer it occupies. |
| `rotationTilt` | 0.30 | How far the rotation axis leans downstream per metre of height. |
| `rotationRate` | 0.05 /s | How hard the flow is held to the target swirl. |

The mesocyclone's *geometry* turns out to matter more than its strength. A core
of 2.6 km faded out to 8.8 km covers most of the domain, and a rotating column
that wide drags a broad layer up underneath it — 1.5 million cloudy cells out
of 5.7 million. A real mesocyclone is two to four kilometres across, and
confining it to that is the whole of what it needs.

### What sets the precipitation and the downdraft

| Variable | Value | Effect |
|---|---|---|
| `freezingLevel` | 4000 m | Rain only forms below it. |
| `glaciationLevel` | 8000 m | Cloud above it is ice: it neither evaporates nor falls out. **This is the anvil.** |
| `iceEvaporation` | 0.004 | Ice sublimates at 0.4% of the liquid rate. |
| `fallout` / `iceFallout` | 0.00035 / 0.00002 /s | Slow loss of cloud water and of ice crystals. |
| `autoThreshold` / `autoRate` | 0.0012 / 0.0025 /s | When droplets start colliding into rain, and how fast. |
| `accretionRate` | 0.5 /s per unit rain | Falling rain collecting droplets — the runaway. |
| `rainFallSpeed` | 8.0 m/s | Terminal velocity, relative to the air. |
| `rainEvaporation` | 0.80 | How fast rain evaporates into unsaturated air. **This is the downdraft.** |

### What supplies the storm in the first place

| Variable | Value | Effect |
|---|---|---|
| `forceHeat` | 0.0090 | Heating rate at the centre of the source. |
| `forceMoisture` | 2.4e-6 | Moistening rate. |
| `forceRadius` | 1200 m | How wide the heated patch is — and therefore how wide the tower is. |
| `forceDepth` | 400 m | Vertical half-depth. |
| `forceHeight` | 400 m | Height of the patch, inside the mixed layer. |
| `forceOffset` | (−5000, 0) m | Where it sits, deliberately upstream of centre so the anvil has the long side of the domain to stream into. |

The source is **wide and shallow rather than a point**, and that is a
meteorological choice: a narrow plume rises as a single mushroom, whereas a
broad heated sheet inside the mixed layer feeds condensation across a whole
layer, which is what gives one flat base under several turrets. Low-frequency
noise is applied across the patch so it is not axisymmetric — a perfectly round
source gives a perfectly round mushroom, and a real cumulus is several turrets
sharing one base because the ground under them is uneven.

Phase 04 made the source **narrower and cooler**, which sounds backwards until
you see the reason: an anvil only reads as an anvil when it is wider than what
feeds it, the domain caps how wide the anvil can get, so the *tower* is what
has to give. This was affordable only because a rotating updraft sustains
itself on far less forcing than an unrotating one.

### Housekeeping the box requires

| Variable | Value | Effect |
|---|---|---|
| `lateralMargin` | 0.16 | Fraction of the half-width over which everything is relaxed back to the environment. The sides are periodic, so without this an anvil reaching one edge reappears at the other. |
| `lateralRate` | 0.045 /s | How absorbent that margin is at the very edge. |
| `envRelaxation` | 0.0 | How hard clear air is pulled back to the sounding. **Zero**, deliberately: the heat source works by accumulating warmth and moisture in clear air, and a relaxation that pulls clear air back to the sounding is subtracting from the source every step. |

### The phases, in order

The arc is a script, and this is the whole of it. Times are in *storm seconds*;
the display runs the storm 20× faster than real weather, so divide by 20 for
what you actually watch.

| Storm time | On screen | What happens | Driven by |
|---|---|---|---|
| 0 – 250 s | 0 – 12 s | **Cumulus.** The lid sits at 2.6 km at full 5.5 K strength; the boundary layer is heated at 32% of full. Thermals reach the condensation level, condense, and stop dead on the inversion. A flat-based fair-weather cumulus a couple of hundred metres deep. | `cap → capCumulus`, `ramp → rampCumulus` |
| 250 – 500 s | 12 – 25 s | **Congestus.** The lid rises toward 6 km and begins to weaken. Towers punch up through where it used to be and reach mid-levels. Rotation starts to be introduced. Cloud top ~3.5 km, updraft ~25 m/s. | `cap → capCongestus`, `strength` falling |
| 500 – 800 s | 25 – 40 s | **Deepening.** The lid rises to the equilibrium level and its strength goes to zero — by 800 s there is no lid at all. Heating ramps to full. The tower runs to the cap: 7.6 km at 800 s, updraft 54 m/s. Ice begins to build the anvil. Lightning switches on. | `cap → equilibrium`, `strength → 0`, `ramp → 1` |
| 800 – 1900 s | 40 – 95 s | **Mature.** Full depth ~9.2 km, sustained updraft ~50 m/s, anvil spreading downstream, rain shaft below, cold air spreading at the ground. Rotation is fully established, and the wall cloud and rear-flank clear slot come in with it. | `rotation → 1` |
| 1295 – 1515 s | 65 – 76 s | **Funnel descends.** The tornado begins 45% of the way through the mature act — a tornado follows the mesocyclone rather than arriving with it — and takes 220 s to reach the ground. The debris cloud appears near the end of the descent. | `funnelDescent` |
| 1515 – 1935 s | 76 – 97 s | **On the ground.** Full intensity. | `funnelIntensity` |
| 1935 – 2195 s | 97 – 110 s | **Roping out.** The funnel thins and tilts. Descent and intensity are separate curves precisely because a tornado dies by thinning, not by rising back into the cloud. | `funnelIntensity` falling |
| 1900 – 2600 s | 95 – 130 s | **Decay.** Heating ramps to zero, rotation fades, the storm consumes the instability it grew in and is undercut by its own cold pool. Cloud top falls to 6 km, condensate collapses. | `ramp → 0`, `rotation → 0` |
| 2600 – 2920 s | 130 – 146 s | **Empty sky**, then a cut to a new seed. | `finished()` |

### Where the variety comes from

Each run derives a different atmosphere from the run's seed, and the way it
does so is itself a finding. Spike 03 measured cloud top varying by **0.4%**
across four different noise seeds — every seed made the same storm — while the
sounding controlled it monotonically. So `DeriveStorm` perturbs the
*atmosphere*, not the noise:

```cpp
sounding.surfaceRH   += SeedSpread(seed, 1, 0.030f);   // where the base sits
sounding.lapseTropo  += SeedSpread(seed, 2, 0.00016f); // how deep it gets
sounding.equilibrium += SeedSpread(seed, 3, 700.0f);   // and where the anvil is
const float vigour = SeedValue(seed, 4);
sounding.shear[0]      = 3.3f + vigour * 1.4f;         // how hard it leans
sounding.rotationSpeed = 19.0f + vigour * 9.0f;        // and how hard it spins
```

That last pair is kept deliberately in step, because Phase 04 measured that
shear without rotation tears the storm apart: a seed asking for more of one has
to ask for more of the other. Meteorologically this is right — shear and
storm-relative helicity go together in real severe-weather environments.

The result is a genuinely different day's weather each run, not a different
random number: a different cloud base, a different depth, a different lean, a
different tornado lifetime. The ranges are modest on purpose — each one alone
is recognisably the same kind of storm; together they are a sky that does not
repeat.

---

## Part 3 — where it departs from reality

Everything above is real physics, implemented honestly. This part is the other
half of the ledger. Some of these are approximations every cloud model makes;
some are compressions made to fit a storm into a screensaver; and some are
things that are simply *drawn*.

### Time is compressed roughly a hundredfold

The solver takes 1 second of storm time per step at 20 steps per second, so
**the storm runs 20× faster than real weather**. Beyond that, the scripted arc
gives the storm a total life of 2,600 storm seconds — 43 minutes — where a real
supercell lives one to four hours. Between the two, what you watch in 130
seconds stands for something like three hours of real weather.

The consequence is that everything is out of proportion in the same direction:
the cap erodes in 13 minutes of storm time rather than over an afternoon; a
tornado that would be on the ground for 20 minutes is on the ground for seven;
the anvil spreads at a rate that would be leisurely in reality and looks brisk
here. The one thing given its *own* clock is the tornado's swirl — spun at
storm rate it would be a blur, so it turns on the display clock at roughly one
revolution a second.

### The grid is 90 metres, and a tornado is not

The domain is 224 × 160 × 160 cells at 90 m: 20.2 km along the shear, 14.4 km
deep, 14.4 km across. That is a respectable resolution for a storm-scale model,
and it resolves the tower, the anvil, the rain shaft and the mesocyclone
perfectly well.

It does not resolve a tornado. At 90 m cells a 400 m funnel is four cells
across, and the vortex dynamics that actually make a tornado — the corner flow,
the near-surface inflow layer, subvortices — happen at tens of metres or less.
Research simulations that resolve tornadogenesis run at 10–30 m and cost days
of supercomputer time.

It also means the smallest cloud features the solver knows about are 90 m wide,
whereas the cauliflower texture the eye reads as "cumulus" is far finer. That
gap is filled at render time by noise erosion — the solver supplies the
low-frequency shape and the renderer carves detail into its boundary. **What
you see is not the condensate field.** It is the condensate field with sub-grid
detail invented on top of it, with the erosion backed off inside ice (because
an anvil is a smooth fibrous sheet with no cauliflower on it) and in rain
(because falling rain is streaked along its own direction rather than billowed).

### The storm is started by a heated patch that does not exist

A real storm is initiated by the boundary layer converging somewhere — a front,
a dryline, a sea breeze, terrain, or the gust front of a previous storm — and
lifting parcels past their condensation level.

Storm has none of that. It has a warm, moist source sitting at 400 m, 1200 m
wide, running for the whole life of the storm at a strength the arc dictates.
It is a stand-in for a boundary layer with a place in it where the air is going
up, and the design note is blunt about what it is for: *the simulation supplies
motion and texture; it never decides what happens.*

The `/forced` development mode makes the artificiality visible. Held at a
constant level, the same heat source settles into a steady dry thermal —
5.6 m/s, unvarying, for four thousand seconds, never once condensing — because
a running plume ventilates the heating zone faster than it can accumulate
anything. The original storm only got going because it started from still air
with a 2 K warm bubble handed to it in the initial condition. Real convection
does not need to be handed anything.

### The cap rises; real caps do not

This is the largest single piece of stagecraft in the model, and it is the one
that produces the three acts.

In reality a capping inversion sits at one to two kilometres and **erodes** —
it is warmed away from below by surface heating and lifted by large-scale
ascent, and when it fails, storms go up. It does not travel upward. Storm's lid
starts at 2.6 km and rises to 9.2 km over 800 storm seconds while
simultaneously weakening to nothing, and it does so on a fixed schedule with no
feedback from the storm at all.

The reason it works is instructive. It was tried the honest way first — hold
the lid still and vary the heat source — and that does not produce the arc,
because in a conditionally unstable atmosphere a parcel that reaches its
condensation level is committed. Weaker heating gives a thinner cumulonimbus,
not a cumulus. Only a lid gives you a cumulus, so the lid is what has to move.

It is worth noting how carefully the *implementation* respects the physics even
though the schedule does not. Moving the lid changes every cell's environmental
potential temperature, and an early version that moved the whole stratosphere
down changed the profile by seventy kelvin at a stroke — the entire layer above
the old lid became buoyant at once and the domain filled with half a million
cells of spurious cloud. The shipped version shifts every cell's θ by exactly
the change in the environmental profile, so each parcel's *departure* from its
surroundings — which is all buoyancy depends on — comes through untouched.

### The mesocyclone is imposed, not grown

Covered in Part 1, but it belongs on this list. A real mesocyclone is produced
by the updraft tilting the environment's horizontal vorticity into the vertical
and then stretching it. Storm imposes a target Rankine vortex and nudges the
tangential flow toward it, at 5% per second.

Two honest consequences:

- The storm's rotation is **not a response to its own environment**. Change the
  shear and the mesocyclone does not change with it; the seed changes both
  together by hand. In a real storm, and in a resolving model, one causes the
  other.
- The *asymmetric structure* a real mesocyclone produces does not appear. Spike
  04 measured this directly: imposed swirl gives a rotating updraft and none of
  the hook echo, the displaced precipitation shaft, or the rear-flank downdraft
  that go with a real one.

What *is* reproduced honestly is the consequence: a rotating updraft survives
shear that would destroy a non-rotating one, and the measured numbers for that
are in Part 1.

### The tornado and its entourage are drawn, not simulated

The funnel, the wall cloud, the debris cloud, the rear-flank clear slot and the
vault are all **authored geometry** evaluated analytically in the renderer.
They read the storm's state — the funnel hangs from the cloud base, on the
mesocyclone axis, and leans with it — but nothing in the fluid solver produces
them and nothing in the fluid solver knows they are there.

Concretely, the funnel is a radius profile about the mesocyclone axis
(`tornadoRadius = 215` m, flaring upward as `0.22 + 0.78·h^1.35`), banded by
noise that rotates with height, with a wandering axis and a debris cloud where
it meets the ground:

```hlsl
float radius = gTornadoRadius * (0.22 + 0.78 * pow(h, 1.35));
radius *= 0.74 + 0.52 * n;
density = 1.0 - smoothstep(radius * 0.72, radius, r);
```

The clear slot is likewise a wedge cut back into the precipitation from one
azimuth, curving with radius the way a rear-flank downdraft wraps. The
`clearSlot` comment states the position exactly: *the storm makes the rain;
this decides where it is not.*

Two things make this more defensible than it might sound. First, it is not
composited over the finished image — it goes into `sampleDensity` alongside the
simulated condensate, which means the light volume sees it, so the funnel is
shadowed by the storm above it and casts its own shadow across the ground.
Second, the clear slot is not decorative: without it the funnel hangs inside
opaque precipitation at 18 km and is *not visible at all*, which is exactly
what the first version did.

But it should be stated plainly: **the tornado is a game asset.** It has a life
cycle because a curve says so, not because a vortex is intensifying and then
losing its supply. It ropes out on a timer.

The same is true of the **mammatus** — the pouched underside of the anvil — by
a cheaper trick still: the sample position is displaced up and down by a
low-frequency noise field inside the ice, so a boundary sampled through a
displaced coordinate is a displaced boundary. Real mammatus are sinking lobes
of cloud-laden air destabilised at the anvil base. These are a texture lookup.

And the **lightning** is scheduled, not electrified. The flash rate follows the
arc — the storm is electrified once the tower is deep and stops being so as it
decays — rather than being derived from the mixed-phase region where charge
separation actually happens. It is placed below the glaciation level, which is
the right *place*, for none of the right reasons.

### The fluid equations are simplified in the usual ways

These are standard modelling approximations rather than shortcuts peculiar to
this project, but they are real departures:

- **Incompressible and Boussinesq.** The solver enforces a divergence-free
  velocity field and treats density as constant except in the buoyancy term.
  Over a 14 km column the real air density falls by roughly a factor of four,
  which is why serious cloud models use anelastic or fully compressible
  equations. The practical consequence here is that the upper storm behaves as
  though it were denser than it is.
- **Saturation adjustment is instantaneous.** Any vapour above saturation
  condenses completely within a single step, and any subsaturation evaporates
  cloud water completely. Real condensation has a timescale, and real clouds
  supersaturate slightly.
- **Bulk, single-moment, warm-rain microphysics.** There is one liquid cloud
  category, one rain category, and a *fraction* representing ice — not an ice
  species with its own advection. There is no graupel and **no hail**, which is
  a supercell's signature precipitation, and no droplet or drop size
  distribution. Rain falls at a fixed 8 m/s regardless of how much of it there
  is.
- **The saturation curve is an exponential in height**, not the
  Clausius–Clapeyron relation in temperature. It is a good fit over the range
  that matters, but it means a parcel 5 K warmer than its surroundings
  saturates at the same mixing ratio as one that is not.
- **The pressure solve is 20 Jacobi iterations**, which does not fully
  converge. Residual divergence remains in the field every step.
- **No Coriolis force.** Over 43 minutes at mid-latitudes this is small
  compared with the imposed rotation, but it is genuinely part of how real
  mesocyclones organise.
- **Semi-Lagrangian advection**, which is unconditionally stable — that is what
  lets the step size be chosen for how fast the storm should evolve rather than
  for numerical safety — and which is also notably diffusive. It smooths
  gradients the storm ought to keep sharp.
- **No vorticity confinement**, a common technique for restoring swirl that
  numerical diffusion removes. It was tried and rejected: Spike 03 measured it
  convecting a *statically stable* atmosphere from rest to 29.9 m/s, which is
  not a correction, it is a heat source.

### The box is a box

The domain is periodic on the sides, with a relaxation margin absorbing what
reaches the edge. Several things follow:

- **The anvil cannot spread properly.** A real supercell anvil streams a
  hundred kilometres or more downwind. This one has 20 km of domain and a
  margin that starts absorbing about 5 km from the storm's axis — which is
  roughly where mass continuity puts the anvil edge anyway, but it means the
  anvil you see is the shape of a real anvil cut off at the frame.
- **The cold pool cannot propagate away.** In reality the pool of rain-cooled
  air spreading out under a storm produces a **gust front**, which is one of
  the main ways storms initiate new cells and one half of the balance — between
  cold-pool spreading and low-level shear — that governs how long a storm's
  updraft stays upright. Here the outflow is absorbed at the margin.
- **The environment is held, not evolved.** Nothing in a periodic box maintains
  the wind shear, so the wind is relaxed back to the sounding everywhere except
  inside cloud, at a fixed rate. A real storm modifies its own environment, and
  that feedback is deliberately suppressed — the exemption for cloud is what
  stops the relaxation from erasing the anvil, which *is* the storm's outflow.
- **The ground is flat and inert.** No terrain, no surface heat or moisture
  fluxes, no friction layer. The near-surface inflow a tornado feeds on is
  strongly shaped by all three.
- **The lid is rigid.** Vertical velocity is forced to zero at the top of the
  domain, with a sponge layer beneath it so gravity waves do not reflect back
  down through the cloud top. Real storms radiate those waves into the
  stratosphere.

### And some values are simply set to what looked right

To be complete: a handful of numbers in the render path are constants chosen
for the image rather than derived from anything, and the code labels them as
such. The cloud base the tornado hangs from is hard-coded at 1035 m — the
measured value — rather than read back from the field. The clear slot's azimuth
is 2.60 radians, "toward the camera's left". The funnel turns at roughly one
revolution a second because "faster reads as a special effect; slower does not
read as a tornado at all". The heat source sits 5 km upstream of centre purely
so the anvil has the long side of the domain to stream into, and the code says
so: *it is a framing decision, not a physical one.*

---

## In short

The **thermodynamics is real**: potential temperature, latent heat release,
conditional instability, condensate loading, evaporative cooling driving the
downdraft, and ice being what makes an anvil. Those are not approximated for
effect; they are the reason the storm has the shape it has, and the reason the
cloud base sits within one grid cell of where the sounding says it should.

The **fluid dynamics is real but coarse**: a proper staggered-grid
incompressible solver with pressure projection, at a resolution that resolves a
storm and does not resolve a tornado.

The **direction is not real**. The cap rises on a schedule, the mesocyclone is
imposed, the storm is fed by a heat source that has no cause, and the tornado
and everything around it is drawn.

What makes the result convincing is that the stagecraft is applied at exactly
the places where a physical model would need a hundred times the resolution or
a hundred times the domain, and everything in between is left to the physics.
