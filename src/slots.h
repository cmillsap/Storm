// Storm - descriptor table layout.
//
// One fixed table for the whole renderer, so the shaders can name registers
// directly: the UAV range maps onto u0..u17 and the SRV range onto t0..t14, in
// this order. Shared by the renderer and the simulation, which is why it lives
// on its own rather than in either.
#pragma once

enum Slot
{
    // UAVs, u0 upward.
    kUavSharedTarget = 0,
    kUavCloudCurrent,
    kUavCloudHistory0,
    kUavCloudHistory1,
    kUavLightVolume,
    kUavBaseNoise,
    kUavDetailNoise,
    kUavSimU0,          // u7 - the simulation owns everything from here
    kUavSimU1,
    kUavSimV0,
    kUavSimV1,
    kUavSimW0,
    kUavSimW1,
    kUavSimS0,
    kUavSimS1,
    kUavSimP0,
    kUavSimP1,
    kUavSimDiv,         // u17
    kUavCount,

    // SRVs, t0 upward.
    kSrvSharedTarget = kUavCount,
    kSrvCloudCurrent,
    kSrvCloudHistory0,
    kSrvCloudHistory1,
    kSrvLightVolume,
    kSrvBaseNoise,
    kSrvDetailNoise,
    kSrvSimU0,          // t7
    kSrvSimU1,
    kSrvSimV0,
    kSrvSimV1,
    kSrvSimW0,
    kSrvSimW1,
    kSrvSimS0,
    kSrvSimS1,          // t14
    kSlotCount
};

static const unsigned kSrvCount = kSlotCount - kUavCount;

// Root parameter indices, matching the root signature in renderer.cpp.
enum RootParam
{
    kRootTable = 0,     // the descriptor table above
    kRootFrame,         // b0, frame constants
    kRootPush,          // b1, root constants that change between dispatches
    kRootSim,           // b2, simulation constants
};

// Mirrors the Push cbuffer in common.hlsli.
struct PushConstants
{
    float   cropScale[2];
    float   cropOffset[2];
    int     simPhase;
    int     jacobiPhase;
    int     pad[2];
};
