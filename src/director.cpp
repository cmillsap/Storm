#include "director.h"

#include "simulation.h"

#include <algorithm>
#include <cmath>

static const float kPi = 3.14159265f;

// Catmull-Rom through four control values. Chosen over a smoothstep between
// neighbours because it is C1 across the joins: a camera that changes speed
// discontinuously at every keyframe reads as a stutter even when the position
// is perfectly continuous.
static float Spline(float a, float b, float c, float d, float t)
{
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * b)
                 + (-a + c) * t
                 + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2
                 + (-a + 3.0f * b - 3.0f * c + d) * t3);
}

void Director::create(const Simulation& sim)
{
    const StormArc& arc = sim.arc;

    // Where the tornado act sits, taken from the arc rather than restated, so
    // the camera cannot drift out of step with the storm it is filming.
    const float funnelStart = arc.mature + arc.sustain * arc.tornadoOnset;
    const float onGround    = funnelStart + arc.tornadoDescend;
    const float ropeOut     = onGround + arc.tornadoHold;
    const float gone        = ropeOut + arc.tornadoRope;

    // Azimuth: 0 is where the fixed camera stood. The tornado act swings round
    // toward the inflow side, which is the quadrant a supercell keeps free of
    // rain and therefore the only one you can see a funnel from.
    const float inflow = 0.95f;

    int n = 0;
    auto shot = [&](float at, float distance, float azimuth, float height,
                    float aim, float fov)
    {
        if (n < kMaxShots) shots[n++] = { at, distance, azimuth, height, aim, fov };
    };

    // Act I. Far off and low, so the first cumulus is small in a large sky -
    // which is what it is.
    shot(0.0f,            23000.0f, -0.22f, 260.0f,  1400.0f, 52.0f);
    shot(arc.cumulus,     19000.0f, -0.10f, 220.0f,  1800.0f, 52.0f);

    // Act II, the congestus. Closing, and starting to look up.
    shot(arc.congestus,   15500.0f,  0.04f, 200.0f,  3600.0f, 55.0f);

    // Act III, the cumulonimbus. Back off to get the anvil in frame; this is
    // the widest the storm ever is and the shot has to hold all of it.
    shot(arc.mature,      17500.0f,  0.16f, 180.0f,  6200.0f, 58.0f);
    shot(funnelStart,     14000.0f,  0.55f, 160.0f,  5200.0f, 56.0f);

    // Act IV. Round to the inflow side and in, as the funnel comes down.
    shot(onGround,         6800.0f, inflow, 120.0f,  1500.0f, 52.0f);
    shot((onGround + ropeOut) * 0.5f,
                           4200.0f, inflow + 0.28f, 90.0f, 850.0f, 50.0f);
    shot(ropeOut,          5200.0f, inflow + 0.52f, 110.0f, 1100.0f, 52.0f);

    // And out again as it ropes out and the storm dies.
    shot(gone,            11000.0f, inflow + 0.75f, 200.0f, 3400.0f, 55.0f);
    shot(arc.duration() + 600.0f,
                          21000.0f, inflow + 0.95f, 300.0f, 5000.0f, 54.0f);
    shotCount = n;
}

Camera Director::frame(float stormTime, float displaySeconds, const Simulation& sim) const
{
    Camera camera;
    if (shotCount == 0) return camera;

    // Find the pair the moment falls between, then the four control shots the
    // spline needs, clamping at both ends.
    int i = 0;
    while (i < shotCount - 2 && stormTime >= shots[i + 1].at) ++i;

    const Shot& b = shots[i];
    const Shot& c = shots[std::min(i + 1, shotCount - 1)];
    const Shot& a = shots[std::max(i - 1, 0)];
    const Shot& d = shots[std::min(i + 2, shotCount - 1)];

    const float span = std::max(c.at - b.at, 1.0f);
    const float t = std::min(std::max((stormTime - b.at) / span, 0.0f), 1.0f);

    const float distance = Spline(a.distance, b.distance, c.distance, d.distance, t);
    const float height   = Spline(a.height,   b.height,   c.height,   d.height,   t);
    const float aim      = Spline(a.aim,      b.aim,      c.aim,      d.aim,      t);
    const float fov      = Spline(a.fov,      b.fov,      c.fov,      d.fov,      t);
    float azimuth        = Spline(a.azimuth,  b.azimuth,  c.azimuth,  d.azimuth,  t);

    azimuth += driftAmplitude * std::sin(displaySeconds * 2.0f * kPi / driftPeriod);

    // The axis the whole film is about: the mesocyclone, which sits upstream of
    // the domain centre by however far the forcing was offset.
    const float axisX = sim.centre(0) + sim.sounding.forceOffset[0]
                      + sim.sounding.rotationTilt * sim.sounding.rotationBase;
    const float axisZ = sim.centre(2) + sim.sounding.forceOffset[1];

    // Distance is never allowed inside the debris cloud, whatever a shot asks
    // for: a spline overshoots between keyframes, and one that overshoots
    // through the tornado puts the camera inside it.
    const float safe = std::max(distance, 1600.0f);

    camera.position[0] = axisX + std::sin(azimuth) * safe;
    camera.position[1] = std::max(height, 40.0f);
    camera.position[2] = axisZ - std::cos(azimuth) * safe;

    // Aim at the axis. Yaw is measured the same way the renderer builds its
    // basis: forward is (sin yaw cos pitch, sin pitch, cos yaw cos pitch).
    camera.baseYaw = std::atan2(axisX - camera.position[0], axisZ - camera.position[2]);
    camera.yaw = camera.baseYaw;
    camera.pitch = std::atan2(aim - camera.position[1], safe);
    camera.fovDegrees = fov;
    return camera;
}
