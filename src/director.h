// Storm - the camera, and the acts it exists to show.
//
// Phases 00 to 04 shipped one fixed viewpoint, 18 km from the storm, because
// that is the distance a 14 km cumulonimbus fits into a 55-degree frame. Phase
// 04 then hung a tornado under it that is 680 m across - two degrees from
// there - and that settled the argument the plan had been deferring: the storm
// has outgrown a single camera.
//
// Shots are cylindrical rather than Cartesian: a distance from the storm's
// axis, an azimuth around it, a height, and a height on the axis to aim at.
// Interpolating those gives a camera that arcs around the storm and always
// looks at it. Interpolating positions instead gives chords, and a chord
// between two points either side of a supercell goes through the supercell.
#pragma once

#include "view.h"

struct Simulation;

// One keyframe.
struct Shot
{
    float at;         // storm seconds this shot is centred on
    float distance;   // m from the storm's vertical axis
    float azimuth;    // radians; 0 puts the camera where Phases 00-04 stood
    float height;     // m above ground
    float aim;        // m above ground, on the axis, that the camera looks at
    float fov;        // degrees, vertical
};

struct Director
{
    // The whole film, in storm time. The times are deliberately expressed
    // against the arc rather than as absolutes, so that changing the storm's
    // pacing moves the camera with it - see Director::create.
    static const int kMaxShots = 10;
    Shot  shots[kMaxShots] = {};
    int   shotCount = 0;

    // A slow drift added on top of everything, so the camera is never quite
    // still even when a shot is holding. Without it a held shot reads as a
    // freeze rather than as a camera.
    float driftAmplitude = 0.035f;   // radians of azimuth
    float driftPeriod    = 47.0f;    // display seconds

    // Builds the shot list from the storm's own timings.
    void create(const Simulation& sim);

    // The camera for a moment of storm time. displaySeconds drives only the
    // drift, which belongs on the display's clock rather than the storm's.
    Camera frame(float stormTime, float displaySeconds, const Simulation& sim) const;
};
