// Storm - what the user gets to decide, and where it is kept.
//
// A screensaver's settings live in the registry under HKCU: it has no install
// directory it can rely on writing to, and Windows launches it with whatever
// working directory it feels like.
#pragma once

#include "common.h"

struct Settings
{
    // Auto measures the frame time and picks; the rest pin a tier. Auto is the
    // default because this runs on whatever machine it is installed on, and
    // the frame cost varies by more than a factor of five across the three
    // adapters in this one.
    enum Quality { QualityAuto = 0, QualityHigh, QualityMedium, QualityLow };

    int  quality = QualityAuto;
    int  frameCap = 30;          // fps
    bool cycleStorms = true;     // a new storm every couple of minutes
    int  idleMinutes = 0;        // stop simulating after this long; 0 = never

    // On battery, quality is capped and the frame rate halved whatever the
    // settings say. A screensaver that flattens a laptop is a bug.
    bool respectBattery = true;

    static Settings load();
    void save() const;

    // Number of march steps and whether the light volume rebuilds on every
    // simulation step, for a given tier.
    static int  marchSteps(int tier);
    static bool lightVolumeEveryStep(int tier);
};

// The configuration dialog, shown for /c. Returns true if the user accepted.
bool ShowSettingsDialog(HINSTANCE instance, HWND parent, Settings& settings);
