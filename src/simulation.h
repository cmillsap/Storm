// Storm - the cloud simulation.
//
// A staggered-grid solver on the GPU, ported from the CPU solvers validated in
// Spikes 03 and 04. It runs at its own rate, decoupled from the display: cloud
// dynamics at 20 Hz is more than enough, and freeing it from the refresh rate
// means a 144 Hz monitor does not cost 144 fluid steps a second.
//
// No vorticity confinement. Spike 03 measured it convecting a statically stable
// atmosphere from rest to 29.9 m/s and destroying placement control.
#pragma once

#include "gpu.h"
#include "slots.h"

// Mirrors the SimParams cbuffer in sim.hlsli.
struct alignas(16) SimConstants
{
    float   origin[3];      float cellSize;
    int32_t resolution[3];  float forceDepth;
    float   dt;             float time;         float forceRamp;    float qcRef;
    float   forceCentre[3]; float forceRadius;
    float   forceHeat;      float forceMoisture; float mixedTop;    float lapseMixed;
    float   tropopause;     float lapseTropo;   float lapseStrato;  float surfaceRH;
    float   upperRH;        float rhTransition; float satSurface;   float satScale;
};

struct Simulation
{
    Gpu* gpu = nullptr;

    // 128^3 at 50 m gives a 6.4 km cube: a congestus two to three kilometres
    // across is forty to sixty cells wide, which is ample once render-time
    // noise supplies the detail below that scale.
    UINT resolution = 128;
    float cellSize = 50.0f;
    float origin[3] = { -3200.0f, 0.0f, 3800.0f };

    // Storm time per step. Semi-Lagrangian advection is unconditionally stable,
    // so this is chosen for how fast the cloud should evolve rather than for a
    // CFL limit: 1 s per step at 20 steps a second runs the sky twenty times
    // faster than real weather. At half this the cloud is meteorologically
    // more comfortable and takes forty seconds to appear, which is too long to
    // wait; and because the step count per second is unchanged, the speed-up
    // is free. A 25 m/s updraft still moves only half a cell per step.
    float stepSeconds = 1.0f;
    int   stepsPerSecond = 20;
    int   jacobiIterations = 20;

    ComPtr<ID3D12Resource> velocity[3][2];   // u, v, w - each ping-ponged
    ComPtr<ID3D12Resource> scalars[2];       // theta, vapour, condensate
    ComPtr<ID3D12Resource> pressure[2];
    ComPtr<ID3D12Resource> divergence;
    ComPtr<ID3D12Resource> constantBuffer;
    uint8_t* constantsMapped = nullptr;

    ComPtr<ID3D12PipelineState> psoInitialise, psoAdvect, psoForces, psoBuoyancy;
    ComPtr<ID3D12PipelineState> psoMicrophysics, psoDivergence, psoJacobi, psoProject;

    int   phase = 0;              // which set holds the current data
    float simulatedTime = 0.0f;
    float accumulator = 0.0f;
    bool  initialised = false;
    bool  currentIsReadable = false;

    bool create(Gpu& g, ID3D12RootSignature* rootSignature);
    void shutdown();

    // Advances by real elapsed seconds, stepping zero or more times. Returns
    // true if the volume changed, so the caller knows to rebuild the light
    // volume - which is the plan's amortisation: lighting at simulation rate,
    // not frame rate.
    bool advance(float elapsedSeconds);

    void reset();                                     // re-seed the domain
    D3D12_GPU_VIRTUAL_ADDRESS constantsAddress() const;

    // Leaves the current data in a state the render passes can sample.
    void transitionForReading();

    UINT cellCount() const { return resolution * resolution * resolution; }
    float extent() const { return (float)resolution * cellSize; }

private:
    void fillConstants(SimConstants& c) const;
    void step();
    void bindPhase(int simPhase, int jacobiPhase);
    void barrierUav(ID3D12Resource* r);
    void setSetState(int index, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

    ID3D12RootSignature* m_rootSignature = nullptr;
    D3D12_RESOURCE_STATES m_setState[2] = {};
};
