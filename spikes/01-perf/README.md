# Spike 01 — What does a raymarch step cost?

Answers the question the feasibility plan left open: **is an 8–15 ms frame at
3440×1440 realistic for a volumetric cloud renderer on this hardware?**

**Verdict: yes, with a wide margin — but not for the reasons the plan assumed.**

---

## Build and run

```
build.bat                      # needs VS 2022 with the C++ desktop workload
build\spike01.exe              # benchmark sweep, then an interactive view
```

| Flag | Effect |
|---|---|
| `--list-adapters` | Enumerate GPUs and exit |
| `--adapter N` | Pick a GPU (default: most VRAM) |
| `--frames N` | Measured frames per config (default 50) |
| `--bench-only` | Skip the interactive view |
| `--interactive-only` | Skip the sweep |
| `--capture out.bmp` | Write one frame to disk before anything else |
| `--csv path` | Where to write results (default `spike01-results.csv`) |
| `--light-study` | Run only the light-march investigation |
| `--capture-mode N` | Which mode `--capture` renders (default 4) |
| `--debug` | Enable the D3D12 debug layer (**invalidates all timings**) |

Shaders are compiled at runtime from `shaders\` and located by walking up from
the executable, so they can be edited and re-run without a rebuild.

## What it measures

A compute shader marches a procedural cloud volume into a UAV. GPU timestamps
bracket **that one dispatch and nothing else** — no present, no blit, no CPU
time. Five density-sampler modes sweep four step counts at two resolutions, so
the delta between adjacent configs isolates one cost each.

| Mode | What it adds |
|---|---|
| 0 | Analytic density — loop and ALU only, zero memory traffic |
| 1 | Base volume — 1 × 3D sample per step |
| 2 | Base + detail — 2 × 3D samples per step (the Nubis sampler) |
| 3 | Mode 2 + cheap/expensive adaptive stepping |
| 4 | Mode 3 + a 6-tap cone march to the sun — the production shader's shape |
| 5 | Mode 3 + a single fetch from a precomputed sun-transmittance volume |

The cloud occupies a **finite box**, not an unbounded layer. That matches what
the project renders (one storm in open sky) and is the only framing where
empty-space skipping has anything to skip.

## Results — RTX 5060 Ti (16 GB), 40 frames per config

Median GPU milliseconds for the raymarch dispatch.

| Mode | 96 steps @ 1720×720 | 96 steps @ 3440×1440 |
|---|---:|---:|
| 0 analytic | 0.45 | 1.75 |
| 1 base only | 0.56 | 1.93 |
| 2 base + detail | 0.71 | 2.27 |
| 3 adaptive | 1.88 | 4.37 |
| 4 adaptive + light march | **4.81** | 12.81 |

Worst case, volume covering every pixel (mode 4, 96 steps): **12.54 ms** at
1720×720, **34.75 ms** at 3440×1440.

RTX 3060 for comparison: 6.69 ms production-shaped, 46.10 ms worst case at
native. Roughly 1.4× the 5060 Ti.

---

## Findings

### 1. The plan's budget holds, with room to spare

The production-shaped config costs **4.81 ms** at half resolution. The plan
estimated 3–8 ms for the render band. At a 30 fps cap that leaves ~28 ms for
everything else, against a simulation the plan budgeted at 2–4 ms.

### 2. Texture fetches are essentially free — the loop is the cost

The marginal cost of one step is **~0.0042–0.0049 ms** at 1720×720 *regardless
of whether that step performs zero, one, or two 3D texture fetches*. The
analytic mode, which touches no memory at all, is the **most** expensive per
step, because its trig costs more than a cache-friendly volume sample.

This inverts the usual assumption. Optimisation effort belongs on step count and
ALU, not on memory traffic — and reading the simulation volume in Phase 02 adds
a third fetch that should cost near nothing.

### 3. The light march is 60% of the frame

Mode 4 costs 4.81 ms against mode 3's 1.88 ms at the same step count. The 6-tap
cone march to the sun is **~2.9 ms, about 61% of the total**.

That is where optimisation should go, and it suggests an architectural answer
the plan already has the pieces for: precompute sun transmittance into a
low-resolution volume at the **20 Hz simulation rate** instead of marching it
per-sample per-frame. Lighting changes slowly; the sun barely moves between
frames.

### 3a. Follow-up: the light march can be made almost free

Finding 3 was investigated on its own (`--light-study`). All figures 1720×720,
96 steps, RTX 5060 Ti.

**Cost is exactly linear in tap count** — ~0.53 ms per tap, flat from 2 to 12
taps. There is no fixed overhead and no divergence cliff; the taps themselves
are the entire cost. So the lighting bill is simply `taps × 0.53 ms`.

| Taps | Total | Lighting | Per tap |
|---:|---:|---:|---:|
| 2 | 3.13 | 1.06 | 0.531 |
| 4 | 4.21 | 2.15 | 0.537 |
| 6 | 5.23 | 3.16 | 0.527 |
| 8 | 6.23 | 4.17 | 0.521 |
| 12 | 7.92 | 5.86 | 0.488 |

**Dropping the detail octaves from the light march saves 26%** (5.20 → 3.84 ms).
Worth taking, but not the answer.

**Precomputing transmittance into a volume is the answer.** A 64×48×64 R16F
volume (~200 m voxels), each voxel marching 24 taps to the sun:

| | ms |
|---|---:|
| Raymarch, single volume fetch | 2.14 |
| Volume build | 0.42 |
| Both, rebuilt every frame | 2.56 |
| Both, rebuilt at 20 Hz against 60 fps | **2.28** |
| 6-tap march, for comparison | 5.20 |

**2.28× faster, and lighting drops from 60% of the raymarch to 9%.** The fetch
itself costs 0.08 ms — the raymarch with lighting (2.14 ms) is barely more
expensive than the raymarch with *no lighting at all* (2.06 ms).

The reason it works is that it decouples lighting resolution from screen
resolution. The 6-tap march evaluates density roughly 37 million times per
frame (pixels × accumulation samples × taps); the volume build evaluates it 4.7
million times (196k voxels × 24 taps) — and buys *better* lighting doing it,
since 24 taps reach further than 6.

**And it costs nothing visually.** Comparing captured frames pixel by pixel:

```
mean abs diff 0.103/255 (0.04%)   max 4/255
channels differing by >2: 0.04%   by >8: 0.000%
```

A maximum error of 4/255 across the whole frame is below 8-bit dithering noise.

Caveats: measured on a static cloud with a static sun. Temporal stability under
a 20 Hz rebuild while the storm evolves is **untested**, and 200 m voxels cannot
shadow a tornado funnel or carry lightning, which is a point light inside the
volume and needs its own mechanism. The build here evaluates density
procedurally; reading a simulation volume instead should be cheaper, so 0.42 ms
is a conservative figure.

### 4. Cheap/expensive adaptive stepping was a pessimisation

**Mode 3 is 2.7× slower than mode 2**, not faster.

The cause is in the implementation: the fine step is half the base step and the
iteration budget is doubled, so inside cloud it takes up to 2× the samples of
fixed stepping, and the coarse phase cannot recover that in a bounded volume
where a ray is mostly *inside* cloud once it enters.

The lesson is not that empty-space skipping does not work — it is that a
cheap/expensive sample toggle is not a substitute for a real acceleration
structure. The plan's occupancy-mip chain (Phase 02) is the right mechanism, and
this result says to **measure it rather than assume it pays**.

With lighting solved by finding 3a, this is now the dominant remaining cost.
Fixed stepping (mode 2, 0.71 ms) plus a precomputed light volume (+0.08 ms fetch,
+0.14 ms amortised build) lands at roughly **0.93 ms** — another 2.4× below the
2.28 ms measured above. **The production shader should start as fixed stepping
plus a light volume**, and adopt an acceleration structure only once one is
measured to beat that.

### 5. Half-resolution rendering is required, not optional

A full-frame storm at native 3440×1440 costs 34.75 ms on the 5060 Ti and
46.10 ms on the 3060 — both over a 33 ms budget before any simulation runs.
At half resolution the same worst case is 12.54 ms, and with a 4×4 Bayer pattern
updating one pixel in sixteen it is under 1 ms.

### 6. The scene had to be fixed four times before any number meant anything

The first sweep looked excellent and was worthless. It was measuring the
underside of an opaque overcast deck where rays terminated within a few samples.
Nothing in the timings revealed this — only capturing the frame and looking at
it did. Two shader bugs surfaced the same way:

- A `saturate()` on the lower bound of the Perlin–Worley remap made it an
  identity mapping, so the Worley erosion was doing **nothing**.
- Aerial perspective was applied to every ray that *entered the bounding box*
  rather than in proportion to accumulated cloud, painting the box onto the sky
  as a visible quadrilateral.

`--capture` exists because of this. Any future perf work on this project should
produce an image alongside the numbers.

---

## Caveats

- Timings cover the raymarch dispatch only — no simulation, no temporal resolve,
  no present, no swap-chain cost.
- Density comes from procedural noise, not a simulation volume. Per finding 2
  the extra fetch should be near-free, but that is an inference, not a
  measurement.
- Frames are fully synchronised, so nothing overlaps. A real renderer would
  recover some of this.
- The standard framing puts the storm across roughly 20% of the frame; the
  worst-case row bounds the other end.
- The image is *structurally* representative, not beautiful. Cloud-base
  hardness, silhouette and detail erosion are Spike 02's problem.
