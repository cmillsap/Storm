// Storm - Spike 04: does the mesocyclone work in 3D?
//
// Spike 03 established that a directed 2D storm goes exactly where it is told,
// that a stability layer sets the cloud top, and that the anvil spreads once
// capped. It also found the one thing 2D cannot answer: at 3.5 m/s/km of shear
// the 2D storm tore apart, because a vertical slice has no mechanism for the
// rotating updraft that lets a real supercell survive strong shear.
//
// The physics 2D is missing: vertical shear puts horizontal vorticity into the
// environment. An updraft tilts that vorticity into the vertical. If the
// hodograph is straight the tilted vorticity forms a symmetric counter-rotating
// pair and the storm splits with no net rotation. If the hodograph is curved,
// part of the ambient vorticity is *streamwise* - aligned with the
// storm-relative flow - and the updraft acquires net rotation. That is a
// mesocyclone, and it is what keeps the updraft alive and displaced from its
// own precipitation.
//
// This spike asks three things:
//   A. Does a 3D storm survive shear that killed the 2D one?
//   B. Does hodograph curvature produce rotation, controllably?
//   C. Does the updraft separate from its precipitation?
//
// Usage:  spike04.exe [--all] [--demo] [--film out.bmp] [--quick]

#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const float kGravity      = 9.81f;
static const float kTheta0       = 300.0f;
static const float kLatentOverCp = 2488.0f;
static const float kPi           = 3.14159265f;

static inline int wrapi(int i, int n) { return (i % n + n) % n; }

// ------------------------------------------------------------------- config

struct Config
{
    int   nx = 96, ny = 96, nz = 64;
    float h  = 250.0f;              // 24 x 24 x 16 km
    float dt = 3.0f;
    float duration = 2400.0f;

    // Environment
    float tropopause  = 11000.0f;
    float lapseTropo  = 0.0040f;
    float lapseStrato = 0.0180f;
    float surfaceRH   = 0.78f;
    float upperRH     = 0.30f;
    float rhTransition = 8000.0f;
    float satSurface  = 0.0210f;
    float satScale    = 2200.0f;

    // Hodograph. shearMag is the wind change across shearDepth; curvature 0 is
    // a straight hodograph (crosswise vorticity, splitting storm) and 1 is a
    // quarter circle (streamwise vorticity, dominant rotating updraft).
    float shearMag   = 22.0f;       // m/s across the shear layer
    float shearDepth = 6000.0f;
    float curvature  = 1.0f;

    // Forcing
    float forceX = 12000.0f, forceY = 12000.0f, forceZ = 800.0f;
    // Stronger and wider than the 2D spike used. A 3D plume entrains across a
    // whole surface rather than two flanks, so the same forcing that drove a
    // 2D tower to 13 km stalls at 7 km here.
    float forceRadius = 3200.0f;
    float forceHeat = 0.0060f;
    float forceMoisture = 5.0e-6f;
    float forceDuration = 1500.0f;
    float triggerBubble = 3.0f;

    // Directed rotation. The plan art-directs everything else about the storm;
    // the mesocyclone is no different. This relaxes the tangential wind about
    // the forcing column toward a Rankine-like profile peaking at swirlRadius,
    // over a height band, rather than waiting for shear tilting to produce
    // rotation on its own.
    float swirlSpeed  = 12.0f;      // target peak tangential wind, m/s
    float swirlRadius = 3000.0f;
    float swirlBottom = 800.0f;
    float swirlTop    = 7500.0f;
    float swirlRate   = 1.0f / 240.0f;   // relaxation rate, 1/s

    int   jacobi = 20;
    float spongeTop = 3000.0f;
    uint32_t seed = 1u;
};

struct Metrics
{
    float cloudBase = 0, cloudTop = 0;
    float maxUpdraft = 0, lateUpdraft = 0;
    float maxZeta = 0;              // peak mid-level vertical vorticity, 1/s
    float wZetaCorr = 0;            // correlation of w with zeta in the updraft
    float updraftX = 0, updraftY = 0;
    float precipX = 0, precipY = 0;
    float separation = 0;           // updraft centre to precipitation centre
    float maxSpeed = 0;
    float condensate = 0;
    bool  blewUp = false;
};

// ------------------------------------------------------------------- solver

struct Sim
{
    Config c;
    int nx, ny, nz;
    float h, dt;
    float meanU = 0, meanV = 0;

    std::vector<float> u, v, w, uN, vN, wN;
    std::vector<float> th, qv, qc, thN, qvN, qcN;
    std::vector<float> phi, phiN, div;

    float time = 0;
    uint32_t rng;

    inline int CI(int i, int j, int k) const { return (k * ny + j) * nx + i; }
    inline int WI(int i, int j, int k) const { return (k * ny + j) * nx + i; }

    float randUnit()
    {
        rng = rng * 1664525u + 1013904223u;
        return (float)((rng >> 8) & 0xFFFFFF) / 16777216.0f;
    }

    float thetaEnv(float z) const
    {
        if (z <= c.tropopause) return c.lapseTropo * z;
        return c.lapseTropo * c.tropopause + c.lapseStrato * (z - c.tropopause);
    }
    float satVapour(float z) const { return c.satSurface * std::exp(-z / c.satScale); }
    float relHumidity(float z) const
    {
        float t = std::min(1.0f, std::max(0.0f, z / c.rhTransition));
        return c.surfaceRH + (c.upperRH - c.surfaceRH) * t;
    }
    float vapourEnv(float z) const { return relHumidity(z) * satVapour(z); }

    // Hodograph, blended between a straight line and a quarter circle.
    void hodograph(float z, float& U, float& V) const
    {
        float f = std::min(1.0f, std::max(0.0f, z / c.shearDepth));
        float ang = 0.5f * kPi * f;
        float straightU = c.shearMag * f,             straightV = 0.0f;
        float curvedU   = c.shearMag * std::sin(ang), curvedV   = c.shearMag * (1.0f - std::cos(ang));
        U = straightU + (curvedU - straightU) * c.curvature;
        V = straightV + (curvedV - straightV) * c.curvature;
    }
    // Storm-relative: the layer-mean wind is removed so the storm stays put in
    // the periodic domain and placement stays measurable.
    float windU(float z) const { float U, V; hodograph(z, U, V); return U - meanU; }
    float windV(float z) const { float U, V; hodograph(z, U, V); return V - meanV; }

    void init()
    {
        nx = c.nx; ny = c.ny; nz = c.nz; h = c.h; dt = c.dt;
        rng = c.seed * 2654435761u + 12345u;

        size_t nCell = (size_t)nx * ny * nz;
        u.assign(nCell, 0); v.assign(nCell, 0);
        w.assign((size_t)nx * ny * (nz + 1), 0);
        uN = u; vN = v; wN = w;
        th.assign(nCell, 0); qv.assign(nCell, 0); qc.assign(nCell, 0);
        thN = th; qvN = qv; qcN = qc;
        phi.assign(nCell, 0); phiN = phi; div.assign(nCell, 0);

        // Depth-average the hodograph over the shear layer.
        meanU = meanV = 0;
        const int samples = 64;
        for (int s = 0; s < samples; ++s)
        {
            float U, V; hodograph((s + 0.5f) / samples * c.shearDepth, U, V);
            meanU += U; meanV += V;
        }
        meanU /= samples; meanV /= samples;

        for (int k = 0; k < nz; ++k)
        {
            float z = (k + 0.5f) * h;
            float envU = windU(z), envV = windV(z);
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    int idx = CI(i, j, k);
                    qv[idx] = vapourEnv(z);
                    u[idx] = envU;
                    v[idx] = envV;

                    float dx = (i + 0.5f) * h - c.forceX;
                    float dy = (j + 0.5f) * h - c.forceY;
                    float dz = z - 1500.0f;
                    float r2 = (dx * dx + dy * dy + dz * dz) / (2500.0f * 2500.0f);
                    th[idx] = thetaEnv(z) + c.triggerBubble * std::exp(-r2)
                            + (randUnit() - 0.5f) * 0.05f;
                }
        }
    }

    // Trilinear sample. Periodic in x and y, clamped in z.
    float sampleGrid(const std::vector<float>& f, int ax, int ay, int az,
                     float gx, float gy, float gz) const
    {
        int i0 = (int)std::floor(gx), j0 = (int)std::floor(gy), k0 = (int)std::floor(gz);
        float fx = gx - i0, fy = gy - j0, fz = gz - k0;
        if (k0 < 0)            { k0 = 0;      fz = 0.0f; }
        else if (k0 >= az - 1) { k0 = az - 2; fz = 1.0f; }
        int i1 = wrapi(i0 + 1, ax), j1 = wrapi(j0 + 1, ay);
        i0 = wrapi(i0, ax); j0 = wrapi(j0, ay);
        int k1 = k0 + 1;
        auto at = [&](int i, int j, int k) { return f[(size_t)(k * ay + j) * ax + i]; };
        float a00 = at(i0, j0, k0) * (1 - fx) + at(i1, j0, k0) * fx;
        float a10 = at(i0, j1, k0) * (1 - fx) + at(i1, j1, k0) * fx;
        float a01 = at(i0, j0, k1) * (1 - fx) + at(i1, j0, k1) * fx;
        float a11 = at(i0, j1, k1) * (1 - fx) + at(i1, j1, k1) * fx;
        float b0 = a00 * (1 - fy) + a10 * fy;
        float b1 = a01 * (1 - fy) + a11 * fy;
        return b0 * (1 - fz) + b1 * fz;
    }

    float sU(float x, float y, float z) const { return sampleGrid(u, nx, ny, nz, x / h,        y / h - 0.5f, z / h - 0.5f); }
    float sV(float x, float y, float z) const { return sampleGrid(v, nx, ny, nz, x / h - 0.5f, y / h,        z / h - 0.5f); }
    float sW(float x, float y, float z) const { return sampleGrid(w, nx, ny, nz + 1, x / h - 0.5f, y / h - 0.5f, z / h); }
    float sC(const std::vector<float>& f, float x, float y, float z) const
    { return sampleGrid(f, nx, ny, nz, x / h - 0.5f, y / h - 0.5f, z / h - 0.5f); }

    void advect()
    {
#pragma omp parallel for schedule(static)
        for (int k = 0; k < nz; ++k)
        {
            float z = (k + 0.5f) * h;
            for (int j = 0; j < ny; ++j)
            {
                float y = (j + 0.5f) * h;
                for (int i = 0; i < nx; ++i)
                {
                    float x = i * h;
                    float a = u[CI(i, j, k)], b = sV(x, y, z), cc = sW(x, y, z);
                    uN[CI(i, j, k)] = sU(x - a * dt, y - b * dt, z - cc * dt);
                }
            }
        }
#pragma omp parallel for schedule(static)
        for (int k = 0; k < nz; ++k)
        {
            float z = (k + 0.5f) * h;
            for (int j = 0; j < ny; ++j)
            {
                float y = j * h;
                for (int i = 0; i < nx; ++i)
                {
                    float x = (i + 0.5f) * h;
                    float a = sU(x, y, z), b = v[CI(i, j, k)], cc = sW(x, y, z);
                    vN[CI(i, j, k)] = sV(x - a * dt, y - b * dt, z - cc * dt);
                }
            }
        }
#pragma omp parallel for schedule(static)
        for (int k = 1; k < nz; ++k)
        {
            float z = k * h;
            for (int j = 0; j < ny; ++j)
            {
                float y = (j + 0.5f) * h;
                for (int i = 0; i < nx; ++i)
                {
                    float x = (i + 0.5f) * h;
                    float a = sU(x, y, z), b = sV(x, y, z), cc = w[WI(i, j, k)];
                    wN[WI(i, j, k)] = sW(x - a * dt, y - b * dt, z - cc * dt);
                }
            }
        }
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) { wN[WI(i, j, 0)] = 0; wN[WI(i, j, nz)] = 0; }

#pragma omp parallel for schedule(static)
        for (int k = 0; k < nz; ++k)
        {
            float z = (k + 0.5f) * h;
            for (int j = 0; j < ny; ++j)
            {
                float y = (j + 0.5f) * h;
                for (int i = 0; i < nx; ++i)
                {
                    float x = (i + 0.5f) * h;
                    float bx = x - sU(x, y, z) * dt;
                    float by = y - sV(x, y, z) * dt;
                    float bz = z - sW(x, y, z) * dt;
                    int idx = CI(i, j, k);
                    thN[idx] = sC(th, bx, by, bz);
                    qvN[idx] = sC(qv, bx, by, bz);
                    qcN[idx] = sC(qc, bx, by, bz);
                }
            }
        }
        u.swap(uN); v.swap(vN); w.swap(wN);
        th.swap(thN); qv.swap(qvN); qc.swap(qcN);
    }

    void microphysics()
    {
#pragma omp parallel for schedule(static)
        for (int k = 0; k < nz; ++k)
        {
            float z = (k + 0.5f) * h;
            float qs = satVapour(z);
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    int idx = CI(i, j, k);
                    float excess = qv[idx] - qs;
                    if (excess > 0.0f)
                    {
                        qv[idx] -= excess; qc[idx] += excess;
                        th[idx] += kLatentOverCp * excess;
                    }
                    else if (qc[idx] > 0.0f)
                    {
                        float dq = std::min(qc[idx], -excess);
                        qv[idx] += dq; qc[idx] -= dq;
                        th[idx] -= kLatentOverCp * dq;
                    }
                    qc[idx] -= qc[idx] * 0.00035f * dt;   // fallout
                }
        }
    }

    // Nudges the tangential wind about the forcing column toward a target
    // profile. Relaxation rather than a raw force, so the parameter is a
    // rotation *speed* that can be asked for directly, and the solver is free
    // to deform the result rather than being overdriven by it.
    void swirl()
    {
        float domainX = nx * h, domainY = ny * h;
        auto unwrap = [](float d, float dom) { if (d > dom * 0.5f) d -= dom; if (d < -dom * 0.5f) d += dom; return d; };

        // Rankine-like: rises linearly to swirlRadius, falls off outside.
        auto target = [&](float r, float z) -> float
        {
            if (z < c.swirlBottom || z > c.swirlTop) return 0.0f;
            float band = std::min((z - c.swirlBottom) / 600.0f, (c.swirlTop - z) / 1500.0f);
            band = std::max(0.0f, std::min(1.0f, band));
            float rr = r / c.swirlRadius;
            float shape = rr * std::exp(1.0f - rr);
            return c.swirlSpeed * shape * band;
        };

        // The tangential wind at a face needs BOTH velocity components. Relaxing
        // each component using only its own contribution leaves an error going
        // as cos^2 and sin^2 of the azimuth, which shows up as a spurious
        // four-armed spiral around an otherwise axisymmetric forcing. Both
        // passes read the pre-update fields, then swap.
#pragma omp parallel for schedule(static)
        for (int k = 0; k < nz; ++k)
        {
            float z = (k + 0.5f) * h;
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    float x = i * h, y = (j + 0.5f) * h;
                    float dx = unwrap(x - c.forceX, domainX);
                    float dy = unwrap(y - c.forceY, domainY);
                    float r = std::sqrt(dx * dx + dy * dy);
                    float uu = u[CI(i, j, k)];
                    if (r > 1.0f)
                    {
                        float tx = -dy / r, ty = dx / r;
                        float vtCur = uu * tx + sV(x, y, z) * ty;
                        uu += (target(r, z) - vtCur) * tx * c.swirlRate * dt;
                    }
                    uN[CI(i, j, k)] = uu;
                }
        }
#pragma omp parallel for schedule(static)
        for (int k = 0; k < nz; ++k)
        {
            float z = (k + 0.5f) * h;
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    float x = (i + 0.5f) * h, y = j * h;
                    float dx = unwrap(x - c.forceX, domainX);
                    float dy = unwrap(y - c.forceY, domainY);
                    float r = std::sqrt(dx * dx + dy * dy);
                    float vv = v[CI(i, j, k)];
                    if (r > 1.0f)
                    {
                        float tx = -dy / r, ty = dx / r;
                        float vtCur = sU(x, y, z) * tx + vv * ty;
                        vv += (target(r, z) - vtCur) * ty * c.swirlRate * dt;
                    }
                    vN[CI(i, j, k)] = vv;
                }
        }
        u.swap(uN); v.swap(vN);
    }

    void forces()
    {
        float ramp = (time < c.forceDuration) ? 1.0f
                   : std::max(0.0f, 1.0f - (time - c.forceDuration) / 600.0f);
        if (ramp > 0.0f)
        {
#pragma omp parallel for schedule(static)
            for (int k = 0; k < nz; ++k)
            {
                float z = (k + 0.5f) * h;
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i)
                    {
                        float dx = (i + 0.5f) * h - c.forceX;
                        float dy = (j + 0.5f) * h - c.forceY;
                        float dz = z - c.forceZ;
                        float r2 = (dx * dx + dy * dy + dz * dz)
                                 / (c.forceRadius * c.forceRadius);
                        if (r2 > 9.0f) continue;
                        float wgt = std::exp(-r2) * ramp * dt;
                        th[CI(i, j, k)] += c.forceHeat * wgt;
                        qv[CI(i, j, k)] += c.forceMoisture * wgt;
                    }
            }
        }

        if (c.swirlSpeed > 0.0f) swirl();

#pragma omp parallel for schedule(static)
        for (int k = 1; k < nz; ++k)
        {
            float zb = (k - 0.5f) * h, zt = (k + 0.5f) * h;
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    int kb = CI(i, j, k - 1), kt = CI(i, j, k);
                    float bB = kGravity * ((th[kb] - thetaEnv(zb)) / kTheta0
                                         + 0.61f * (qv[kb] - vapourEnv(zb)) - qc[kb]);
                    float bT = kGravity * ((th[kt] - thetaEnv(zt)) / kTheta0
                                         + 0.61f * (qv[kt] - vapourEnv(zt)) - qc[kt]);
                    w[WI(i, j, k)] += 0.5f * (bB + bT) * dt;
                }
        }

        // Absorbing layer under the lid. No vorticity confinement anywhere:
        // Spike 03 showed it convects a stable atmosphere out of nothing and
        // destroys placement control.
        float zSponge = nz * h - c.spongeTop;
        for (int k = 0; k < nz; ++k)
        {
            float z = (k + 0.5f) * h;
            if (z < zSponge) continue;
            float s = (z - zSponge) / c.spongeTop;
            float damp = std::max(0.0f, 1.0f - s * s * 3.0f * dt);
            float envU = windU(z), envV = windV(z), te = thetaEnv(z);
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    int idx = CI(i, j, k);
                    u[idx] = envU + (u[idx] - envU) * damp;
                    v[idx] = envV + (v[idx] - envV) * damp;
                    w[WI(i, j, k)] *= damp;
                    th[idx] = te + (th[idx] - te) * damp;
                }
        }
    }

    void project()
    {
        double sum = 0.0;
#pragma omp parallel for schedule(static) reduction(+:sum)
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    float d = (u[CI(wrapi(i + 1, nx), j, k)] - u[CI(i, j, k)]) / h
                            + (v[CI(i, wrapi(j + 1, ny), k)] - v[CI(i, j, k)]) / h
                            + (w[WI(i, j, k + 1)] - w[WI(i, j, k)]) / h;
                    div[CI(i, j, k)] = d;
                    sum += d;
                }
        float mean = (float)(sum / ((double)nx * ny * nz));
#pragma omp parallel for schedule(static)
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) div[CI(i, j, k)] -= mean;

        std::fill(phi.begin(), phi.end(), 0.0f);
        for (int it = 0; it < c.jacobi; ++it)
        {
#pragma omp parallel for schedule(static)
            for (int k = 0; k < nz; ++k)
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i)
                    {
                        float xm = phi[CI(wrapi(i - 1, nx), j, k)];
                        float xp = phi[CI(wrapi(i + 1, nx), j, k)];
                        float ym = phi[CI(i, wrapi(j - 1, ny), k)];
                        float yp = phi[CI(i, wrapi(j + 1, ny), k)];
                        float zm = (k > 0)      ? phi[CI(i, j, k - 1)] : phi[CI(i, j, k)];
                        float zp = (k < nz - 1) ? phi[CI(i, j, k + 1)] : phi[CI(i, j, k)];
                        phiN[CI(i, j, k)] = (xm + xp + ym + yp + zm + zp
                                           - h * h * div[CI(i, j, k)]) / 6.0f;
                    }
            phi.swap(phiN);
        }

#pragma omp parallel for schedule(static)
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    u[CI(i, j, k)] -= (phi[CI(i, j, k)] - phi[CI(wrapi(i - 1, nx), j, k)]) / h;
                    v[CI(i, j, k)] -= (phi[CI(i, j, k)] - phi[CI(i, wrapi(j - 1, ny), k)]) / h;
                }
#pragma omp parallel for schedule(static)
        for (int k = 1; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                    w[WI(i, j, k)] -= (phi[CI(i, j, k)] - phi[CI(i, j, k - 1)]) / h;

        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) { w[WI(i, j, 0)] = 0; w[WI(i, j, nz)] = 0; }
    }

    void step() { advect(); forces(); microphysics(); project(); time += dt; }

    // Vertical vorticity at a cell centre.
    float zeta(int i, int j, int k) const
    {
        float dvdx = (v[CI(wrapi(i + 1, nx), j, k)] - v[CI(wrapi(i - 1, nx), j, k)]) / (2 * h);
        float dudy = (u[CI(i, wrapi(j + 1, ny), k)] - u[CI(i, wrapi(j - 1, ny), k)]) / (2 * h);
        return dvdx - dudy;
    }

    float wCentre(int i, int j, int k) const { return 0.5f * (w[WI(i, j, k)] + w[WI(i, j, k + 1)]); }

    Metrics measure() const
    {
        Metrics m;
        const float kCloud = 1.0e-5f;
        float domainX = nx * h, domainY = ny * h;

        auto unwrapX = [&](float x) { float d = x - c.forceX; if (d > domainX * 0.5f) d -= domainX; if (d < -domainX * 0.5f) d += domainX; return d; };
        auto unwrapY = [&](float y) { float d = y - c.forceY; if (d > domainY * 0.5f) d -= domainY; if (d < -domainY * 0.5f) d += domainY; return d; };

        float minZ = 1e9f, maxZ = -1e9f;
        double totalQc = 0.0;
        for (int k = 0; k < nz; ++k)
        {
            float z = (k + 0.5f) * h;
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    float q = qc[CI(i, j, k)];
                    totalQc += q;
                    if (q > kCloud) { minZ = std::min(minZ, z); maxZ = std::max(maxZ, z); }
                }
        }
        if (maxZ < 0) return m;
        m.cloudBase = minZ; m.cloudTop = maxZ; m.condensate = (float)totalQc;

        // Mid-level updraft: centroid, peak vorticity, and the correlation of
        // vertical velocity with vertical vorticity. That correlation is the
        // standard mesocyclone diagnostic - a rotating updraft has w and zeta
        // varying together; a splitting storm has a symmetric couplet and no
        // net correlation.
        int k0 = (int)(3000.0f / h), k1 = (int)(7000.0f / h);
        k0 = std::max(1, k0); k1 = std::min(nz - 2, k1);

        double wsum = 0, xs = 0, ys = 0;
        double sw = 0, sz = 0, sww = 0, szz = 0, swz = 0; int n = 0;
        for (int k = k0; k <= k1; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    float wc = wCentre(i, j, k);
                    float zt = zeta(i, j, k);
                    m.maxZeta = std::max(m.maxZeta, std::fabs(zt));
                    if (wc > 4.0f)
                    {
                        xs += unwrapX((i + 0.5f) * h) * wc;
                        ys += unwrapY((j + 0.5f) * h) * wc;
                        wsum += wc;
                        sw += wc; sz += zt; sww += (double)wc * wc;
                        szz += (double)zt * zt; swz += (double)wc * zt; ++n;
                    }
                }
        if (wsum > 0) { m.updraftX = (float)(xs / wsum); m.updraftY = (float)(ys / wsum); }
        if (n > 8)
        {
            double cov = swz / n - (sw / n) * (sz / n);
            double vw = sww / n - (sw / n) * (sw / n);
            double vz = szz / n - (sz / n) * (sz / n);
            if (vw > 1e-12 && vz > 1e-12) m.wZetaCorr = (float)(cov / std::sqrt(vw * vz));
        }

        // Precipitation centroid in the lowest 2 km of cloud.
        double psum = 0, pxs = 0, pys = 0;
        int kp = std::max(1, (int)(2500.0f / h));
        for (int k = 0; k < kp; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    float q = qc[CI(i, j, k)];
                    if (q <= kCloud) continue;
                    pxs += unwrapX((i + 0.5f) * h) * q;
                    pys += unwrapY((j + 0.5f) * h) * q;
                    psum += q;
                }
        if (psum > 0)
        {
            m.precipX = (float)(pxs / psum); m.precipY = (float)(pys / psum);
            float dx = m.updraftX - m.precipX, dy = m.updraftY - m.precipY;
            m.separation = std::sqrt(dx * dx + dy * dy);
        }

        for (size_t idx = 0; idx < w.size(); ++idx) m.maxUpdraft = std::max(m.maxUpdraft, w[idx]);
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    float a = u[CI(i, j, k)], b = v[CI(i, j, k)], cc = wCentre(i, j, k);
                    float sp = std::sqrt(a * a + b * b + cc * cc);
                    m.maxSpeed = std::max(m.maxSpeed, sp);
                    if (!std::isfinite(sp) || sp > 400.0f) m.blewUp = true;
                }
        return m;
    }
};

// ------------------------------------------------------------ visualisation

struct Image
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    void allocate(int width, int height) { w = width; h = height; rgb.assign((size_t)w * h * 3, 20); }
    void put(int x, int y, float r, float g, float b)
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        size_t k = ((size_t)y * w + x) * 3;
        rgb[k + 0] = (uint8_t)(std::max(0.0f, std::min(1.0f, r)) * 255.0f);
        rgb[k + 1] = (uint8_t)(std::max(0.0f, std::min(1.0f, g)) * 255.0f);
        rgb[k + 2] = (uint8_t)(std::max(0.0f, std::min(1.0f, b)) * 255.0f);
    }
    void block(int x, int y, int s, float r, float g, float b)
    {
        for (int dy = 0; dy < s; ++dy) for (int dx = 0; dx < s; ++dx) put(x + dx, y + dy, r, g, b);
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
        uint8_t* dst = &out[(size_t)(img.h - 1 - y) * rowPadded];
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

// Two views per snapshot: a vertical slice through the forcing column on top,
// and a plan view at 4 km below it. The plan view is where a mesocyclone is
// legible - a single coherent patch of one rotation sense inside the updraft,
// as opposed to the symmetric red/blue couplet of a splitting storm.
static void drawPanel(Image& img, int ox, const Sim& s, int scale)
{
    int jMid = (int)(s.c.forceY / s.h);
    for (int k = 0; k < s.nz; ++k)
    {
        int py = (s.nz - 1 - k) * scale;
        for (int i = 0; i < s.nx; ++i)
        {
            float z = (k + 0.5f) * s.h;
            float base = 0.08f + 0.20f * (1.0f - z / (s.nz * s.h));
            float r = base * 0.55f, g = base * 0.72f, b = base * 1.15f;
            float wc = s.wCentre(i, jMid, k) / 25.0f;
            if (wc > 0) { r += wc * 0.55f; g += wc * 0.18f; }
            else        { b += -wc * 0.45f; }
            float q = s.qc[s.CI(i, jMid, k)];
            if (q > 1.0e-5f)
            {
                float a = std::min(1.0f, std::sqrt(q / 0.0012f));
                float sh = 0.62f + 0.38f * std::min(1.0f, q / 0.0010f);
                r = r * (1 - a) + sh * a; g = g * (1 - a) + sh * a; b = b * (1 - a) + sh * 1.02f * a;
            }
            img.block(ox + i * scale, py, scale, r, g, b);
        }
    }

    int tropoRow = (s.nz - 1 - (int)(s.c.tropopause / s.h)) * scale;
    for (int i = 0; i < s.nx * scale; i += 8) img.block(ox + i, tropoRow, 2, 0.95f, 0.75f, 0.25f);

    int yOff = (s.nz + 2) * scale;
    int kPlan = (int)(4000.0f / s.h);
    for (int j = 0; j < s.ny; ++j)
        for (int i = 0; i < s.nx; ++i)
        {
            float zt = s.zeta(i, j, kPlan) / 0.012f;
            float r = 0.10f, g = 0.11f, b = 0.14f;
            if (zt > 0) { r += zt * 0.85f; g += zt * 0.15f; }
            else        { b += -zt * 0.85f; g += -zt * 0.20f; }
            float q = s.qc[s.CI(i, j, kPlan)];
            if (q > 1.0e-5f)
            {
                float a = std::min(0.75f, std::sqrt(q / 0.0016f));
                r = r * (1 - a) + 0.92f * a; g = g * (1 - a) + 0.92f * a; b = b * (1 - a) + 0.95f * a;
            }
            img.block(ox + i * scale, yOff + (s.ny - 1 - j) * scale, scale, r, g, b);
        }
}

// ------------------------------------------------------------------ driver

static Metrics run(const Config& cfg, const char* filmPath = nullptr,
                   std::vector<Metrics>* history = nullptr)
{
    Sim s; s.c = cfg; s.init();

    const float snapTimes[] = { 600.0f, 1000.0f, 1400.0f, 1800.0f, 2100.0f, 2400.0f };
    const int snapCount = 6, scale = 2;
    Image film;
    int nextSnap = 0;
    if (filmPath)
        film.allocate((cfg.nx * scale) * snapCount + (snapCount - 1) * 6,
                      (cfg.nz + 2 + cfg.ny) * scale);

    int steps = (int)(cfg.duration / cfg.dt);
    float lateFrom = cfg.duration * 0.75f;
    float lateMax = 0.0f;

    for (int n = 0; n < steps; ++n)
    {
        s.step();
        if (s.time >= lateFrom)
            for (size_t idx = 0; idx < s.w.size(); ++idx) lateMax = std::max(lateMax, s.w[idx]);

        if (filmPath && nextSnap < snapCount && s.time >= snapTimes[nextSnap])
        {
            drawPanel(film, nextSnap * (cfg.nx * scale + 6), s, scale);
            ++nextSnap;
        }
        if (history && (n % 20) == 0) history->push_back(s.measure());
    }

    if (filmPath)
    {
        while (nextSnap < snapCount) { drawPanel(film, nextSnap * (cfg.nx * scale + 6), s, scale); ++nextSnap; }
        writeBmp(film, filmPath);
    }
    Metrics m = s.measure();
    m.lateUpdraft = lateMax;
    return m;
}

static void printHeader(const char* t)
{
    std::printf("\n%s\n%s\n", t, std::string(std::strlen(t), '-').c_str());
}

// ------------------------------------------------------------------- tests

static void experimentLongevity(const Config& base)
{
    printHeader("A. Does rotation keep the updraft alive?");
    std::printf("  The question 2D could not answer. Forcing ramps off at 1500 s, so a\n"
                "  storm still running at 2400 s is sustaining itself.\n\n");
    std::printf("  %10s %12s %14s %14s %12s\n",
                "swirl", "cloud top", "peak updraft", "late updraft", "w-zeta corr");
    for (float sw : { 0.0f, 12.0f })
    {
        Config c = base; c.swirlSpeed = sw;
        Metrics m = run(c);
        std::printf("  %7.0f m/s %10.0f m %11.1f m/s %11.1f m/s %12.2f\n",
                    sw, m.cloudTop, m.maxUpdraft, m.lateUpdraft, m.wZetaCorr);
    }
}

static void experimentDirectedRotation(const Config& base)
{
    printHeader("B. Does the mesocyclone track the rotation it is asked for?");
    std::printf("  %10s %14s %14s %14s\n", "swirl", "peak zeta", "w-zeta corr", "late updraft");
    for (float sw : { 0.0f, 6.0f, 12.0f, 18.0f })
    {
        Config c = base; c.swirlSpeed = sw;
        Metrics m = run(c);
        std::printf("  %7.0f m/s %11.4f /s %14.2f %11.1f m/s\n",
                    sw, m.maxZeta, m.wZetaCorr, m.lateUpdraft);
    }
}

static void experimentShearSurvival(const Config& base)
{
    printHeader("C. Does a 3D storm survive shear that killed the 2D one?");
    std::printf("  In 2D, cloud top collapsed from 12.8 km to 9.2 km as shear rose.\n\n");
    std::printf("  %14s %12s %14s %14s\n", "shear", "cloud top", "peak updraft", "late updraft");
    for (float sh : { 0.0f, 12.0f, 22.0f, 32.0f })
    {
        Config c = base; c.shearMag = sh;
        Metrics m = run(c);
        std::printf("  %8.1f m/s/km %10.0f m %11.1f m/s %11.1f m/s\n",
                    sh / (base.shearDepth / 1000.0f), m.cloudTop, m.maxUpdraft, m.lateUpdraft);
    }
}

static void experimentSeparation(const Config& base)
{
    printHeader("D. Does the updraft separate from its own precipitation?");
    std::printf("  A pulse storm rains into its own updraft and chokes. A supercell's\n"
                "  rotation displaces the precipitation to one side.\n\n");
    std::printf("  %10s %16s %14s %14s\n", "swirl", "separation", "cloud top", "late updraft");
    for (float sw : { 0.0f, 6.0f, 12.0f, 18.0f })
    {
        Config c = base; c.swirlSpeed = sw;
        Metrics m = run(c);
        std::printf("  %7.0f m/s %13.0f m %12.0f m %11.1f m/s\n",
                    sw, m.separation, m.cloudTop, m.lateUpdraft);
    }
}

static void experimentEmergentRotation(const Config& base)
{
    printHeader("H. Does hodograph curvature produce rotation on its own?");
    std::printf("  Swirl forcing off, so any rotation here comes from the solver tilting\n"
                "  ambient horizontal vorticity into the vertical.\n\n");
    std::printf("  %10s %14s %14s %14s\n", "curvature", "peak zeta", "w-zeta corr", "late updraft");
    for (float cv : { 0.0f, 0.33f, 0.67f, 1.0f })
    {
        Config c = base; c.curvature = cv; c.swirlSpeed = 0.0f;
        Metrics m = run(c);
        std::printf("  %10.2f %11.4f /s %14.2f %11.1f m/s\n",
                    cv, m.maxZeta, m.wZetaCorr, m.lateUpdraft);
    }
}

static void experimentPlacement(const Config& base)
{
    printHeader("E. Does the updraft still go where it is told, in 3D?");
    std::printf("  %14s %16s %12s\n", "asked for", "updraft centre", "error");
    for (float x : { 8000.0f, 12000.0f, 16000.0f })
    {
        Config c = base; c.forceX = x;
        Metrics m = run(c);
        std::printf("  %10.0f m %14.0f m %10.0f m\n", x, x + m.updraftX, m.updraftX);
    }
}

static void experimentRepeatability(const Config& base)
{
    printHeader("F. Is it the same storm every time?");
    std::printf("  %6s %12s %14s %14s\n", "seed", "cloud top", "peak zeta", "late updraft");
    std::vector<float> tops, zetas;
    for (uint32_t s = 1; s <= 4; ++s)
    {
        Config c = base; c.seed = s;
        Metrics m = run(c);
        tops.push_back(m.cloudTop); zetas.push_back(m.maxZeta);
        std::printf("  %6u %10.0f m %11.4f /s %11.1f m/s\n", s, m.cloudTop, m.maxZeta, m.lateUpdraft);
    }
    auto stats = [](const std::vector<float>& v, float& mean, float& sd)
    {
        mean = 0; for (float x : v) mean += x; mean /= (float)v.size();
        float var = 0; for (float x : v) var += (x - mean) * (x - mean);
        sd = std::sqrt(var / (float)v.size());
    };
    float mt, st, mz, sz;
    stats(tops, mt, st); stats(zetas, mz, sz);
    std::printf("\n  cloud top  mean %.0f m, sd %.0f m (%.1f%%)\n", mt, st, 100 * st / mt);
    std::printf("  peak zeta  mean %.4f /s, sd %.4f /s (%.1f%%)\n", mz, sz, 100 * sz / std::max(mz, 1e-6f));
}

static void experimentStability(const Config& base)
{
    printHeader("G. Stability and cost");
    std::vector<Metrics> history;
    Metrics m = run(base, nullptr, &history);
    float worst = 0; bool blew = false;
    for (const Metrics& x : history) { worst = std::max(worst, x.maxSpeed); blew |= x.blewUp; }
    std::printf("  grid              %d x %d x %d = %.2f M cells at %.0f m\n",
                base.nx, base.ny, base.nz,
                base.nx * base.ny * base.nz / 1.0e6f, base.h);
    std::printf("  duration          %.0f s (%d steps)\n", base.duration, (int)(base.duration / base.dt));
    std::printf("  peak speed        %.1f m/s\n", worst);
    std::printf("  worst CFL         %.2f\n", worst * base.dt / base.h);
    std::printf("  diverged          %s\n", blew ? "YES" : "no");
}

int main(int argc, char** argv)
{
    Config base;
    bool demo = false, all = true;
    std::string film;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if      (a == "--demo")  { demo = true; all = false; }
        else if (a == "--all")   all = true;
        else if (a == "--quick") base.duration = 1500.0f;
        else if (a == "--film" && i + 1 < argc) { film = argv[++i]; all = false; }
        else { std::printf("Unknown argument: %s\n", a.c_str()); return 1; }
    }

    std::printf("\nStorm / Spike 04 - does the mesocyclone work in 3D?\n");
    std::printf("==================================================\n");
    std::printf("%d x %d x %d cells at %.0f m (%.0f x %.0f x %.0f km), dt %.1f s, %.0f s.\n",
                base.nx, base.ny, base.nz, base.h,
                base.nx * base.h / 1000.0f, base.ny * base.h / 1000.0f, base.nz * base.h / 1000.0f,
                base.dt, base.duration);

    if (!film.empty())
    {
        Metrics m = run(base, film.c_str());
        std::printf("\nWrote %s\n", film.c_str());
        std::printf("cloud %.0f-%.0f m, peak zeta %.4f /s, w-zeta corr %.2f, separation %.0f m\n",
                    m.cloudBase, m.cloudTop, m.maxZeta, m.wZetaCorr, m.separation);
        return 0;
    }

    if (demo)
    {
        std::vector<Metrics> history;
        run(base, nullptr, &history);
        printHeader("Evolution");
        std::printf("  %8s %11s %12s %12s %12s\n", "time", "top", "updraft", "peak zeta", "w-z corr");
        for (size_t i = 0; i < history.size(); i += 4)
            std::printf("  %6.0f s %9.0f m %9.1f m/s %11.4f %11.2f\n",
                        i * 20 * base.dt, history[i].cloudTop, history[i].maxUpdraft,
                        history[i].maxZeta, history[i].wZetaCorr);
        return 0;
    }

    if (all)
    {
        experimentLongevity(base);
        experimentDirectedRotation(base);
        experimentShearSurvival(base);
        experimentSeparation(base);
        experimentPlacement(base);
        experimentRepeatability(base);
        experimentStability(base);
        experimentEmergentRotation(base);
        std::printf("\n");
    }
    return 0;
}
