// Storm - Spike 03: can the simulation be told what to do?
//
// The plan's central architectural bet is "directed simulation": a coarse fluid
// solver supplies billowing and turbulence, but never decides what happens. An
// analytic storm skeleton injects buoyancy where the story wants the tower, an
// ambient stability profile spreads the anvil at the right altitude, and a
// shear profile tilts the updraft.
//
// If that does not hold - if the solver ignores the forcing, or produces a
// storm only sometimes - then Phase 03 has no foundation and the whole plan
// needs rethinking. A screensaver that *sometimes* makes a storm has failed.
//
// This is a 2D vertical slice on a staggered (MAC) grid, on the CPU. Two
// dimensions is enough to answer the question and an order of magnitude easier
// to debug, and keeping it off the GPU means a plumbing bug cannot masquerade
// as a physics result.
//
// Usage:  spike03.exe [--all] [--demo] [--film out.bmp] [--quick]

#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ----------------------------------------------------------------- constants

static const float kGravity      = 9.81f;
static const float kTheta0       = 300.0f;    // reference potential temperature, K
static const float kLatentOverCp = 2488.0f;   // K per unit condensed mixing ratio

// ------------------------------------------------------------------- config

struct Config
{
    // Grid and domain
    // 30 km wide so a mature anvil has room to spread without wrapping around
    // the periodic seam and reading as "full domain width".
    int   nx = 384;
    int   ny = 192;
    float h  = 78.0f;             // cell size, metres (30 km x 15 km domain)
    float dt = 2.0f;              // seconds
    // 30 minutes. Long enough for the full cumulus -> congestus -> anvil arc,
    // short enough that the anvil has not yet spread to the domain edge, where
    // the width measurement would saturate and stop discriminating.
    float duration = 1800.0f;     // seconds of storm time

    // Environment - these are the knobs the plan wants to art-direct with
    float tropopause   = 10000.0f;  // equilibrium level: where the anvil should form
    float lapseTropo   = 0.0040f;   // dTheta/dz below it, K/m (conditionally unstable)
    float lapseStrato  = 0.0180f;   // dTheta/dz above it, K/m (the cap)
    float shear        = 0.0f;      // dU/dz, per second
    // Moisture is specified as relative humidity against the saturation
    // profile, never as an independent vapour profile. Given independently,
    // any vapour scale height larger than the saturation scale height makes
    // relative humidity climb with altitude until the environment is
    // supersaturated on its own - the domain is then full of cloud before the
    // simulation takes a single step.
    float surfaceRH     = 0.75f;
    float upperRH       = 0.30f;
    float rhTransition  = 8000.0f;  // height over which RH falls to upperRH
    float satSurface    = 0.0210f;  // saturation mixing ratio at the surface
    float satScale      = 2200.0f;  // e-folding height of saturation, m

    // Forcing - the "storm skeleton"
    float forceX        = 15000.0f; // where the tower is told to go, metres
    float forceY        = 600.0f;
    float forceRadius   = 1600.0f;
    float forceHeat     = 0.0030f;  // K per second at the centre
    float forceMoisture = 3.0e-6f;  // kg/kg per second
    float forceDuration = 1500.0f;  // forcing ramps off after this
    float triggerBubble = 1.5f;     // initial warm bubble amplitude, K

    // Numerics
    int   jacobi = 30;
    // Off by default. Vorticity confinement is not conservative - it injects
    // energy without bound - and at this cell size the force multiplier eps*h
    // is large enough to convect a statically stable atmosphere out of nothing.
    // See experiment G. The billowing it was meant to supply comes from
    // procedural detail noise at render time instead (Spike 02).
    float vorticityConfinement = 0.0f;
    float spongeTop = 2500.0f;      // depth of the absorbing layer below the lid
    uint32_t seed = 1u;
};

// ------------------------------------------------------------------ metrics

struct Metrics
{
    float cloudBase = 0.0f;
    float cloudTop  = 0.0f;
    float lowCentroidX = 0.0f;      // centroid of cloud in the lowest 2 km of cloud
    float highCentroidX = 0.0f;     // centroid in the top 2 km
    float tilt = 0.0f;              // high - low, metres
    float anvilWidth = 0.0f;        // horizontal extent of cloud above the tropopause
    float anvilAspect = 0.0f;       // anvil width / tower width
    float towerWidth = 0.0f;
    float maxUpdraft = 0.0f;
    float maxSpeed = 0.0f;
    float condensate = 0.0f;
    float thetaNoise = 0.0f;        // RMS theta departure well away from the storm
    bool  blewUp = false;
};

// ------------------------------------------------------------------- solver

struct Sim
{
    Config c;
    int nx, ny;
    float h, dt;

    // Staggered layout, periodic in x:
    //   u[i,j] sits on the left face of cell i, at (i*h, (j+0.5)*h)
    //   v[i,j] sits on the bottom face of cell j, at ((i+0.5)*h, j*h)
    //   scalars sit at cell centres ((i+0.5)*h, (j+0.5)*h)
    std::vector<float> u, v, uNew, vNew;
    std::vector<float> th, qv, qc;              // theta perturbation, vapour, cloud water
    std::vector<float> thNew, qvNew, qcNew;
    std::vector<float> phi, phiNew, div, curl, fcx, fcy;

    float time = 0.0f;
    uint32_t rng;

    int  C(int i, int j) const { return j * nx + i; }
    int  U(int i, int j) const { return j * nx + i; }              // nx x ny, periodic in i
    int  V(int i, int j) const { return j * nx + i; }              // nx x (ny+1)
    int  wrap(int i) const { return (i % nx + nx) % nx; }

    float randUnit()
    {
        rng = rng * 1664525u + 1013904223u;
        return (float)((rng >> 8) & 0xFFFFFF) / 16777216.0f;
    }

    // --- environment ---
    float thetaEnv(float y) const
    {
        if (y <= c.tropopause) return c.lapseTropo * y;
        return c.lapseTropo * c.tropopause + c.lapseStrato * (y - c.tropopause);
    }
    float satVapour(float y)  const { return c.satSurface * std::exp(-y / c.satScale); }
    float relHumidity(float y) const
    {
        float t = std::min(1.0f, std::max(0.0f, y / c.rhTransition));
        return c.surfaceRH + (c.upperRH - c.surfaceRH) * t;
    }
    float vapourEnv(float y) const { return relHumidity(y) * satVapour(y); }
    float windEnv(float y)    const { return c.shear * (y - 3000.0f); }

    void init()
    {
        nx = c.nx; ny = c.ny; h = c.h; dt = c.dt;
        rng = c.seed * 2654435761u + 12345u;

        u.assign((size_t)nx * ny, 0.0f);
        v.assign((size_t)nx * (ny + 1), 0.0f);
        uNew = u; vNew = v;
        th.assign((size_t)nx * ny, 0.0f);
        qv.assign((size_t)nx * ny, 0.0f);
        qc.assign((size_t)nx * ny, 0.0f);
        thNew = th; qvNew = qv; qcNew = qc;
        phi.assign((size_t)nx * ny, 0.0f);
        phiNew = phi;
        div.assign((size_t)nx * ny, 0.0f);
        curl.assign((size_t)nx * ny, 0.0f);
        fcx.assign((size_t)nx * ny, 0.0f);
        fcy.assign((size_t)nx * ny, 0.0f);

        for (int j = 0; j < ny; ++j)
        {
            float y = (j + 0.5f) * h;
            for (int i = 0; i < nx; ++i)
            {
                qv[C(i, j)] = vapourEnv(y);
                u[U(i, j)]  = windEnv(y);

                // th holds TOTAL potential temperature (as a departure from the
                // 300 K reference), not a perturbation from the environment.
                // Advection has to carry the total: a parcel lifted into warmer
                // surroundings must become negatively buoyant on its own, which
                // only happens if it carries its absolute theta with it.
                float dx = (i + 0.5f) * h - c.forceX;
                float dy = y - 1500.0f;
                float r2 = (dx * dx + dy * dy) / (2000.0f * 2000.0f);
                th[C(i, j)] = thetaEnv(y)
                            + c.triggerBubble * std::exp(-r2)
                            + (randUnit() - 0.5f) * 0.05f;
            }
        }
        for (int j = 0; j <= ny; ++j)
            for (int i = 0; i < nx; ++i)
                v[V(i, j)] = 0.0f;
    }

    // --- bilinear samplers, periodic in x and clamped in y ---
    float sampleU(float x, float y) const
    {
        float gi = x / h, gj = y / h - 0.5f;
        int i0 = (int)std::floor(gi), j0 = (int)std::floor(gj);
        float fx = gi - i0, fy = gj - j0;
        j0 = std::max(0, std::min(ny - 2, j0));
        fy = std::max(0.0f, std::min(1.0f, fy));
        int i1 = wrap(i0 + 1); i0 = wrap(i0);
        float a = u[U(i0, j0)] * (1 - fx) + u[U(i1, j0)] * fx;
        float b = u[U(i0, j0 + 1)] * (1 - fx) + u[U(i1, j0 + 1)] * fx;
        return a * (1 - fy) + b * fy;
    }

    float sampleV(float x, float y) const
    {
        float gi = x / h - 0.5f, gj = y / h;
        int i0 = (int)std::floor(gi), j0 = (int)std::floor(gj);
        float fx = gi - i0, fy = gj - j0;
        j0 = std::max(0, std::min(ny - 1, j0));
        fy = std::max(0.0f, std::min(1.0f, fy));
        int i1 = wrap(i0 + 1); i0 = wrap(i0);
        float a = v[V(i0, j0)] * (1 - fx) + v[V(i1, j0)] * fx;
        float b = v[V(i0, j0 + 1)] * (1 - fx) + v[V(i1, j0 + 1)] * fx;
        return a * (1 - fy) + b * fy;
    }

    float sampleC(const std::vector<float>& f, float x, float y) const
    {
        float gi = x / h - 0.5f, gj = y / h - 0.5f;
        int i0 = (int)std::floor(gi), j0 = (int)std::floor(gj);
        float fx = gi - i0, fy = gj - j0;
        j0 = std::max(0, std::min(ny - 2, j0));
        fy = std::max(0.0f, std::min(1.0f, fy));
        int i1 = wrap(i0 + 1); i0 = wrap(i0);
        float a = f[C(i0, j0)] * (1 - fx) + f[C(i1, j0)] * fx;
        float b = f[C(i0, j0 + 1)] * (1 - fx) + f[C(i1, j0 + 1)] * fx;
        return a * (1 - fy) + b * fy;
    }

    void advect()
    {
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                float x = i * h, y = (j + 0.5f) * h;
                float vu = u[U(i, j)], vv = sampleV(x, y);
                uNew[U(i, j)] = sampleU(x - vu * dt, y - vv * dt);
            }

        for (int j = 1; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                float x = (i + 0.5f) * h, y = j * h;
                float vu = sampleU(x, y), vv = v[V(i, j)];
                vNew[V(i, j)] = sampleV(x - vu * dt, y - vv * dt);
            }
        for (int i = 0; i < nx; ++i) { vNew[V(i, 0)] = 0.0f; vNew[V(i, ny)] = 0.0f; }

        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                float x = (i + 0.5f) * h, y = (j + 0.5f) * h;
                float bx = x - sampleU(x, y) * dt, by = y - sampleV(x, y) * dt;
                thNew[C(i, j)] = sampleC(th, bx, by);
                qvNew[C(i, j)] = sampleC(qv, bx, by);
                qcNew[C(i, j)] = sampleC(qc, bx, by);
            }

        u.swap(uNew); v.swap(vNew);
        th.swap(thNew); qv.swap(qvNew); qc.swap(qcNew);
    }

    // Saturation adjustment: condense whatever exceeds saturation, release the
    // latent heat into the potential temperature, and evaporate cloud water
    // back when a parcel becomes subsaturated. This is the feedback that makes
    // a cumulus accelerate rather than just coast upward.
    void microphysics()
    {
        for (int j = 0; j < ny; ++j)
        {
            float y = (j + 0.5f) * h;
            float qs = satVapour(y);
            for (int i = 0; i < nx; ++i)
            {
                int k = C(i, j);
                float excess = qv[k] - qs;
                if (excess > 0.0f)
                {
                    qv[k] -= excess;
                    qc[k] += excess;
                    th[k] += kLatentOverCp * excess;
                }
                else if (qc[k] > 0.0f)
                {
                    float dq = std::min(qc[k], -excess);
                    qv[k] += dq;
                    qc[k] -= dq;
                    th[k] -= kLatentOverCp * dq;
                }
                // Precipitation fallout: without it condensate accumulates and
                // the downdraft never develops.
                float fall = qc[k] * 0.00035f * dt;
                qc[k] -= fall;
            }
        }
    }

    void forces()
    {
        // Directed forcing: a sustained warm, moist source at the place the
        // tower is supposed to be.
        float ramp = (time < c.forceDuration)
                   ? 1.0f
                   : std::max(0.0f, 1.0f - (time - c.forceDuration) / 600.0f);
        if (ramp > 0.0f)
        {
            for (int j = 0; j < ny; ++j)
            {
                float y = (j + 0.5f) * h;
                for (int i = 0; i < nx; ++i)
                {
                    float dx = (i + 0.5f) * h - c.forceX;
                    float dy = y - c.forceY;
                    float r2 = (dx * dx + dy * dy) / (c.forceRadius * c.forceRadius);
                    if (r2 > 9.0f) continue;
                    float w = std::exp(-r2) * ramp * dt;
                    th[C(i, j)] += c.forceHeat * w;
                    qv[C(i, j)] += c.forceMoisture * w;
                }
            }
        }

        // Buoyancy on the v faces. Virtual-temperature effect from vapour and
        // condensate loading from cloud water, which is what lets downdrafts
        // form under the anvil.
        for (int j = 1; j < ny; ++j)
        {
            float yb = (j - 0.5f) * h, yt = (j + 0.5f) * h;
            for (int i = 0; i < nx; ++i)
            {
                int kb = C(i, j - 1), kt = C(i, j);
                float bBelow = kGravity * ((th[kb] - thetaEnv(yb)) / kTheta0
                                         + 0.61f * (qv[kb] - vapourEnv(yb)) - qc[kb]);
                float bAbove = kGravity * ((th[kt] - thetaEnv(yt)) / kTheta0
                                         + 0.61f * (qv[kt] - vapourEnv(yt)) - qc[kt]);
                v[V(i, j)] += 0.5f * (bBelow + bAbove) * dt;
            }
        }

        vorticity();
        sponge();
    }

    // Vorticity confinement puts back some of the small-scale curl that
    // semi-Lagrangian advection smears away. This is what supplies the
    // billowing the plan wants the solver for.
    void vorticity()
    {
        if (c.vorticityConfinement <= 0.0f) return;

        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                float dvdx = (v[V(wrap(i + 1), j)] + v[V(wrap(i + 1), j + 1)]
                            - v[V(wrap(i - 1), j)] - v[V(wrap(i - 1), j + 1)]) * 0.25f / h;
                int jm = std::max(j - 1, 0), jp = std::min(j + 1, ny - 1);
                float dudy = (u[U(i, jp)] + u[U(wrap(i + 1), jp)]
                            - u[U(i, jm)] - u[U(wrap(i + 1), jm)]) * 0.25f / ((jp - jm) * h * 0.5f);
                curl[C(i, j)] = dvdx - dudy;
            }

        std::fill(fcx.begin(), fcx.end(), 0.0f);
        std::fill(fcy.begin(), fcy.end(), 0.0f);
        for (int j = 1; j < ny - 1; ++j)
            for (int i = 0; i < nx; ++i)
            {
                float gx = (std::fabs(curl[C(wrap(i + 1), j)]) - std::fabs(curl[C(wrap(i - 1), j)])) / (2 * h);
                float gy = (std::fabs(curl[C(i, j + 1)]) - std::fabs(curl[C(i, j - 1)])) / (2 * h);
                float len = std::sqrt(gx * gx + gy * gy) + 1e-12f;
                float nxx = gx / len, nyy = gy / len;
                float w = curl[C(i, j)];
                // f = eps * h * (N x w)
                fcx[C(i, j)] =  nyy * w * c.vorticityConfinement * h;
                fcy[C(i, j)] = -nxx * w * c.vorticityConfinement * h;
            }

        // The force is computed at cell centres but the velocities it acts on
        // live on faces. Applying it directly offsets it by half a cell in both
        // directions, which does not average out - it biases the flow one way
        // and the storm drifts steadily off the column it was told to occupy.
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                u[U(i, j)] += 0.5f * (fcx[C(wrap(i - 1), j)] + fcx[C(i, j)]) * dt;

        for (int j = 1; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                v[V(i, j)] += 0.5f * (fcy[C(i, j - 1)] + fcy[C(i, j)]) * dt;
    }

    // Absorbing layer below the rigid lid, so gravity waves do not reflect off
    // the top and rattle back down through the anvil.
    void sponge()
    {
        float yTopStart = ny * h - c.spongeTop;
        for (int j = 0; j < ny; ++j)
        {
            float y = (j + 0.5f) * h;
            if (y < yTopStart) continue;
            float s = (y - yTopStart) / c.spongeTop;
            float k = std::max(0.0f, 1.0f - s * s * 3.0f * dt);
            for (int i = 0; i < nx; ++i)
            {
                u[U(i, j)] = windEnv(y) + (u[U(i, j)] - windEnv(y)) * k;
                v[V(i, j)] *= k;
                th[C(i, j)] = thetaEnv(y) + (th[C(i, j)] - thetaEnv(y)) * k;
            }
        }
    }

    void project()
    {
        float meanDiv = 0.0f;
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                float d = (u[U(wrap(i + 1), j)] - u[U(i, j)]) / h
                        + (v[V(i, j + 1)] - v[V(i, j)]) / h;
                div[C(i, j)] = d;
                meanDiv += d;
            }
        // Periodic in x with Neumann top and bottom makes the system singular;
        // removing the mean keeps Jacobi from drifting.
        meanDiv /= (float)(nx * ny);
        for (auto& d : div) d -= meanDiv;

        std::fill(phi.begin(), phi.end(), 0.0f);
        for (int it = 0; it < c.jacobi; ++it)
        {
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    float l = phi[C(wrap(i - 1), j)];
                    float r = phi[C(wrap(i + 1), j)];
                    float d = (j > 0)      ? phi[C(i, j - 1)] : phi[C(i, j)];
                    float t = (j < ny - 1) ? phi[C(i, j + 1)] : phi[C(i, j)];
                    phiNew[C(i, j)] = (l + r + d + t - h * h * div[C(i, j)]) * 0.25f;
                }
            phi.swap(phiNew);
        }

        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                u[U(i, j)] -= (phi[C(i, j)] - phi[C(wrap(i - 1), j)]) / h;

        for (int j = 1; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                v[V(i, j)] -= (phi[C(i, j)] - phi[C(i, j - 1)]) / h;

        for (int i = 0; i < nx; ++i) { v[V(i, 0)] = 0.0f; v[V(i, ny)] = 0.0f; }
    }

    void step()
    {
        advect();
        forces();
        microphysics();
        project();
        time += dt;
    }

    Metrics measure() const
    {
        Metrics m;
        const float kCloud = 1.0e-5f;

        float minY = 1e9f, maxY = -1e9f;
        double totalQc = 0.0;
        for (int j = 0; j < ny; ++j)
        {
            float y = (j + 0.5f) * h;
            for (int i = 0; i < nx; ++i)
            {
                float q = qc[C(i, j)];
                totalQc += q;
                if (q > kCloud) { minY = std::min(minY, y); maxY = std::max(maxY, y); }
            }
        }
        if (maxY < 0.0f) return m;    // no cloud at all

        m.cloudBase = minY;
        m.cloudTop  = maxY;
        m.condensate = (float)totalQc;

        // Centroids are computed about the forcing column and unwrapped across
        // the periodic seam, so a storm sitting near the domain edge does not
        // report a centroid averaged to the middle of the domain.
        auto centroidBand = [&](float y0, float y1)
        {
            double wsum = 0.0, xsum = 0.0;
            for (int j = 0; j < ny; ++j)
            {
                float y = (j + 0.5f) * h;
                if (y < y0 || y > y1) continue;
                for (int i = 0; i < nx; ++i)
                {
                    float q = qc[C(i, j)];
                    if (q <= kCloud) continue;
                    float x = (i + 0.5f) * h;
                    float dx = x - c.forceX;
                    float domain = nx * h;
                    if (dx >  domain * 0.5f) dx -= domain;
                    if (dx < -domain * 0.5f) dx += domain;
                    xsum += dx * q;
                    wsum += q;
                }
            }
            return (wsum > 0.0) ? (float)(xsum / wsum) : 0.0f;
        };

        m.lowCentroidX  = centroidBand(minY, minY + 2000.0f);
        m.highCentroidX = centroidBand(std::max(minY, maxY - 2000.0f), maxY);
        m.tilt = m.highCentroidX - m.lowCentroidX;

        auto widthBand = [&](float y0, float y1)
        {
            int count = 0;
            std::vector<int> cols(nx, 0);
            for (int j = 0; j < ny; ++j)
            {
                float y = (j + 0.5f) * h;
                if (y < y0 || y > y1) continue;
                for (int i = 0; i < nx; ++i)
                    if (qc[C(i, j)] > kCloud) cols[i] = 1;
            }
            for (int i = 0; i < nx; ++i) count += cols[i];
            return count * h;
        };

        m.anvilWidth = widthBand(c.tropopause - 1000.0f, maxY);
        m.towerWidth = widthBand(minY, minY + 3000.0f);
        m.anvilAspect = (m.towerWidth > 0.0f) ? m.anvilWidth / m.towerWidth : 0.0f;

        // How much grid-scale noise is sitting in the temperature field far
        // from the storm. Gravity waves radiating off the updraft are real and
        // expected; a large value here instead means numerical noise that could
        // seed convection where nothing was asked for.
        {
            double sum = 0.0; int n = 0;
            int farLo = wrap((int)(c.forceX / h) + nx / 2 - nx / 8);
            for (int k = 0; k < nx / 4; ++k)
            {
                int i = wrap(farLo + k);
                for (int j = 0; j < ny; ++j)
                {
                    float y = (j + 0.5f) * h;
                    float d = th[C(i, j)] - thetaEnv(y);
                    sum += (double)d * d;
                    ++n;
                }
            }
            m.thetaNoise = (n > 0) ? (float)std::sqrt(sum / n) : 0.0f;
        }

        for (int j = 0; j <= ny; ++j)
            for (int i = 0; i < nx; ++i)
                m.maxUpdraft = std::max(m.maxUpdraft, v[V(i, j)]);

        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                float su = u[U(i, j)], sv = v[V(i, j)];
                float sp = std::sqrt(su * su + sv * sv);
                m.maxSpeed = std::max(m.maxSpeed, sp);
                if (!std::isfinite(sp) || sp > 300.0f) m.blewUp = true;
            }
        return m;
    }
};

// ------------------------------------------------------------ visualisation

struct Image
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;   // top-down, 3 bytes per pixel

    void allocate(int width, int height)
    {
        w = width; h = height;
        rgb.assign((size_t)w * h * 3, 0);
    }
    void put(int x, int y, float r, float g, float b)
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        size_t k = ((size_t)y * w + x) * 3;
        rgb[k + 0] = (uint8_t)(std::max(0.0f, std::min(1.0f, r)) * 255.0f);
        rgb[k + 1] = (uint8_t)(std::max(0.0f, std::min(1.0f, g)) * 255.0f);
        rgb[k + 2] = (uint8_t)(std::max(0.0f, std::min(1.0f, b)) * 255.0f);
    }
};

static void writeBmp(const Image& img, const char* path)
{
#pragma pack(push, 1)
    struct FileHeader { uint16_t type; uint32_t size; uint16_t r1, r2; uint32_t offset; };
    struct InfoHeader { uint32_t size; int32_t w, h; uint16_t planes, bits;
                        uint32_t compression, imageSize; int32_t xppm, yppm;
                        uint32_t used, important; };
#pragma pack(pop)

    int rowPadded = ((img.w * 3) + 3) & ~3;
    std::vector<uint8_t> out((size_t)rowPadded * img.h, 0);
    for (int y = 0; y < img.h; ++y)
    {
        const uint8_t* src = &img.rgb[(size_t)y * img.w * 3];
        uint8_t* dst = &out[(size_t)(img.h - 1 - y) * rowPadded];   // BMP is bottom-up
        for (int x = 0; x < img.w; ++x)
        {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
    }
    uint32_t dataBytes = (uint32_t)out.size();
    FileHeader fh = { 0x4D42, 54 + dataBytes, 0, 0, 54 };
    InfoHeader ih = { 40, img.w, img.h, 1, 24, 0, dataBytes, 2835, 2835, 0, 0 };

    FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "could not write %s\n", path); return; }
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
}

// Draws one panel of the filmstrip: cloud water in white, the buoyant thermal
// tinted warm underneath it, and a dashed line at the prescribed tropopause so
// the anvil can be read against the height it was told to stop at.
static void drawPanel(Image& img, int ox, const Sim& s)
{
    for (int j = 0; j < s.ny; ++j)
    {
        int py = s.ny - 1 - j;
        float y = (j + 0.5f) * s.h;
        for (int i = 0; i < s.nx; ++i)
        {
            float sky = 0.10f + 0.28f * (1.0f - y / (s.ny * s.h));
            float r = sky * 0.55f, g = sky * 0.75f, b = sky * 1.15f;

            float buoy = (s.th[s.C(i, j)] - s.thetaEnv(y)) / 14.0f;
            if (buoy > 0.0f) { r += buoy * 0.50f; g += buoy * 0.18f; }
            else             { b += -buoy * 0.28f; }

            float q = s.qc[s.C(i, j)];
            if (q > 1.0e-5f)
            {
                // Thin anvil cloud carries two orders of magnitude less
                // condensate than the tower core; scaling alpha to the core
                // makes the anvil invisible even though it is measured.
                float a = std::min(1.0f, std::sqrt(q / 0.0012f));
                float shade = 0.60f + 0.40f * std::min(1.0f, q / 0.0010f);
                r = r * (1 - a) + shade * a;
                g = g * (1 - a) + shade * a;
                b = b * (1 - a) + (shade * 1.02f) * a;
            }
            img.put(ox + i, py, r, g, b);
        }
    }

    int tropoRow = s.ny - 1 - (int)(s.c.tropopause / s.h);
    for (int i = 0; i < s.nx; i += 8)
        for (int k = 0; k < 4 && i + k < s.nx; ++k)
            img.put(ox + i + k, tropoRow, 0.95f, 0.75f, 0.25f);

    int fx = (int)(s.c.forceX / s.h);
    for (int k = 0; k < 5; ++k) img.put(ox + fx, s.ny - 1 - k, 0.2f, 1.0f, 0.4f);
}

// ------------------------------------------------------------------ driver

static Metrics run(const Config& cfg, const char* filmPath = nullptr,
                   std::vector<Metrics>* history = nullptr)
{
    Sim s; s.c = cfg; s.init();

    const float snapTimes[] = { 300.0f, 600.0f, 900.0f, 1400.0f, 2000.0f, 2700.0f };
    const int   snapCount = 6;
    Image film;
    int nextSnap = 0;
    if (filmPath)
    {
        film.allocate(cfg.nx * snapCount + (snapCount - 1) * 4, cfg.ny);
        std::fill(film.rgb.begin(), film.rgb.end(), (uint8_t)26);
    }

    int steps = (int)(cfg.duration / cfg.dt);
    for (int n = 0; n < steps; ++n)
    {
        s.step();

        if (filmPath && nextSnap < snapCount && s.time >= snapTimes[nextSnap])
        {
            drawPanel(film, nextSnap * (cfg.nx + 4), s);
            ++nextSnap;
        }
        if (history && (n % 25) == 0) history->push_back(s.measure());
    }

    if (filmPath)
    {
        while (nextSnap < snapCount) { drawPanel(film, nextSnap * (cfg.nx + 4), s); ++nextSnap; }
        writeBmp(film, filmPath);
    }
    return s.measure();
}

static void printHeader(const char* title)
{
    std::printf("\n%s\n", title);
    std::printf("%s\n", std::string(std::strlen(title), '-').c_str());
}

// ------------------------------------------------------------------- tests

static void experimentPlacement(const Config& base)
{
    printHeader("A. Does the tower go where it is told?");
    std::printf("  %10s %14s %12s %10s\n", "asked for", "low centroid", "error", "top");
    const float xs[] = { 5000.0f, 10000.0f, 15000.0f };
    for (float x : xs)
    {
        Config c = base; c.forceX = x;
        Metrics m = run(c);
        std::printf("  %8.0f m %12.0f m %10.0f m %8.0f m\n",
                    x, x + m.lowCentroidX, m.lowCentroidX, m.cloudTop);
    }
}

static void experimentEquilibriumLevel(const Config& base)
{
    printHeader("B. Does the stability layer set the cloud top?");
    std::printf("  %12s %12s %12s %12s\n", "tropopause", "cloud top", "overshoot", "anvil width");
    const float tops[] = { 7000.0f, 9000.0f, 11000.0f, 12500.0f };
    for (float t : tops)
    {
        Config c = base; c.tropopause = t;
        Metrics m = run(c);
        std::printf("  %10.0f m %10.0f m %10.0f m %10.0f m\n",
                    t, m.cloudTop, m.cloudTop - t, m.anvilWidth);
    }
}

static void experimentShear(const Config& base)
{
    printHeader("C. Does shear tilt the updraft?");
    std::printf("  %14s %12s %12s %12s\n", "shear", "tilt", "cloud top", "anvil width");
    const float shears[] = { 0.0f, 0.0010f, 0.0020f, 0.0035f };
    for (float sh : shears)
    {
        Config c = base; c.shear = sh;
        Metrics m = run(c);
        std::printf("  %8.1f m/s/km %10.0f m %10.0f m %10.0f m\n",
                    sh * 1000.0f, m.tilt, m.cloudTop, m.anvilWidth);
    }
}

static void experimentAnvil(const Config& base)
{
    printHeader("D. Does the anvil spread once the top is capped?");
    std::printf("  %10s %12s %12s %14s %12s\n", "time", "cloud top", "anvil width", "tower width", "aspect");
    Config c = base;
    std::vector<Metrics> history;
    run(c, nullptr, &history);
    const int marks[] = { 6, 12, 18, 24, 30, 42, 53 };
    for (int idx : marks)
    {
        if (idx >= (int)history.size()) continue;
        const Metrics& m = history[idx];
        std::printf("  %8.0f s %10.0f m %10.0f m %12.0f m %10.2f\n",
                    idx * 25 * c.dt, m.cloudTop, m.anvilWidth, m.towerWidth, m.anvilAspect);
    }
}

static void experimentRepeatability(const Config& base)
{
    printHeader("E. Is it the same storm every time?");
    std::printf("  %6s %12s %12s %14s %12s\n", "seed", "cloud top", "anvil width", "condensate", "max updraft");

    std::vector<float> tops, widths;
    for (uint32_t seed = 1; seed <= 6; ++seed)
    {
        Config c = base; c.seed = seed;
        Metrics m = run(c);
        tops.push_back(m.cloudTop);
        widths.push_back(m.anvilWidth);
        std::printf("  %6u %10.0f m %10.0f m %14.2f %10.1f m/s\n",
                    seed, m.cloudTop, m.anvilWidth, m.condensate, m.maxUpdraft);
    }

    auto stats = [](const std::vector<float>& v, float& mean, float& spread)
    {
        mean = 0.0f;
        for (float x : v) mean += x;
        mean /= (float)v.size();
        float var = 0.0f;
        for (float x : v) var += (x - mean) * (x - mean);
        spread = std::sqrt(var / (float)v.size());
    };
    float mt, st, mw, sw;
    stats(tops, mt, st);
    stats(widths, mw, sw);
    std::printf("\n  cloud top   mean %.0f m, sd %.0f m  (%.1f%%)\n", mt, st, 100.0f * st / mt);
    std::printf("  anvil width mean %.0f m, sd %.0f m  (%.1f%%)\n", mw, sw, 100.0f * sw / mw);
}

static void experimentStability(const Config& base)
{
    printHeader("F. Does it stay stable over a full arc?");
    Config c = base;
    std::vector<Metrics> history;
    Metrics final = run(c, nullptr, &history);

    float worstSpeed = 0.0f;
    bool  blewUp = false;
    for (const Metrics& m : history) { worstSpeed = std::max(worstSpeed, m.maxSpeed); blewUp |= m.blewUp; }

    float cfl = worstSpeed * c.dt / c.h;
    std::printf("  duration          %.0f s of storm time (%d steps)\n", c.duration, (int)(c.duration / c.dt));
    std::printf("  peak speed        %.1f m/s\n", worstSpeed);
    std::printf("  peak updraft      %.1f m/s\n", final.maxUpdraft);
    std::printf("  worst CFL         %.2f  (semi-Lagrangian, so >1 is legal)\n", cfl);
    std::printf("  theta noise       %.2f K RMS far from the storm\n", final.thetaNoise);
    std::printf("  diverged          %s\n", blewUp ? "YES" : "no");
}

// Two results from the suite above need explaining before they can be trusted:
// a placement error that is the same size at every forcing position, and 1.6 K
// of temperature noise measured far from the storm. Both have decisive tests.
static void experimentDiagnostics(const Config& base)
{
    printHeader("G. Where do the bias and the noise come from?");

    // If a quiescent atmosphere stays quiet, the noise in a storm run is
    // radiated by the storm - gravity waves off the updraft, which are real.
    // If it does not, the solver is manufacturing it.
    {
        std::printf("  quiescent atmosphere, no forcing at all\n");
        std::printf("    %-22s %12s %12s %12s\n", "confinement", "theta noise", "peak speed", "condensate");
        for (float eps : { 0.0f, 0.02f, 0.06f, 0.12f })
        {
            Config c = base;
            c.forceHeat = 0.0f; c.forceMoisture = 0.0f; c.triggerBubble = 0.0f;
            c.vorticityConfinement = eps;
            Metrics m = run(c);
            std::printf("    eps %.2f               %8.3f K %10.2f m/s %12.3f\n",
                        eps, m.thetaNoise, m.maxSpeed, m.condensate);
        }
        std::printf("\n");
    }

    // Vorticity confinement applies a cell-centred force to face-centred
    // velocities, so it is the obvious suspect for a systematic drift.
    {
        std::printf("  placement error against vorticity confinement\n");
        for (float eps : { 0.0f, 0.02f, 0.06f, 0.12f })
        {
            Config c = base; c.vorticityConfinement = eps;
            Metrics m = run(c);
            std::printf("    eps %.2f         %+8.0f m\n", eps, m.lowCentroidX);
        }
        std::printf("\n");
    }

    // A fixed offset is calibratable; one that grows with time is a drift and
    // would wander over a ten-minute arc.
    {
        std::printf("  placement error against run length\n");
        for (float d : { 600.0f, 1200.0f, 1800.0f })
        {
            Config c = base; c.duration = d;
            Metrics m = run(c);
            std::printf("    %6.0f s         %+8.0f m\n", d, m.lowCentroidX);
        }
        std::printf("\n");
    }
}

int main(int argc, char** argv)
{
    Config base;
    bool doAll = true, demo = false;
    std::string film;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if      (a == "--demo")  { demo = true; doAll = false; }
        else if (a == "--all")   doAll = true;
        else if (a == "--quick") base.duration = 1800.0f;
        else if (a == "--film" && i + 1 < argc) { film = argv[++i]; doAll = false; }
        else { std::printf("Unknown argument: %s\n", a.c_str()); return 1; }
    }

    std::printf("\nStorm / Spike 03 - can the simulation be directed?\n");
    std::printf("=================================================\n");
    std::printf("2D vertical slice, %dx%d cells at %.0f m, dt %.1f s, %.0f s of storm time.\n",
                base.nx, base.ny, base.h, base.dt, base.duration);

    if (!film.empty())
    {
        Config c = base; c.shear = 0.0010f; c.duration = 2100.0f;
        Metrics m = run(c, film.c_str());
        std::printf("\nWrote %s\n", film.c_str());
        std::printf("cloud %0.f-%.0f m, anvil %.0f m, tilt %.0f m, peak updraft %.1f m/s\n",
                    m.cloudBase, m.cloudTop, m.anvilWidth, m.tilt, m.maxUpdraft);
        return 0;
    }

    if (demo)
    {
        std::vector<Metrics> history;
        run(base, nullptr, &history);
        printHeader("Evolution");
        std::printf("  %8s %12s %12s %12s %12s %12s\n",
                    "time", "base", "top", "anvil", "updraft", "condensate");
        for (size_t i = 0; i < history.size(); i += 5)
        {
            const Metrics& m = history[i];
            std::printf("  %6.0f s %10.0f m %10.0f m %10.0f m %8.1f m/s %12.2f\n",
                        i * 25 * base.dt, m.cloudBase, m.cloudTop, m.anvilWidth,
                        m.maxUpdraft, m.condensate);
        }
        return 0;
    }

    if (doAll)
    {
        experimentPlacement(base);
        experimentEquilibriumLevel(base);
        experimentShear(base);
        experimentAnvil(base);
        experimentRepeatability(base);
        experimentStability(base);
        experimentDiagnostics(base);
        std::printf("\n");
    }
    return 0;
}
