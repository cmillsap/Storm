// Storm - Spike 02
// Nishita-style single-scattering atmosphere.
//
// One function does all of it. Called with a huge distance it returns the sky;
// called with the distance to a cloud sample or the ground it returns the
// aerial perspective for that point. Using the same integral for both is what
// makes distant cloud sit *in* the air rather than in front of a painted
// backdrop, and it produces horizon warming and sunset colour for free.

static const float  kEarthRadius   = 6360000.0;
static const float  kAtmoRadius    = 6420000.0;
static const float3 kBetaRayleigh  = float3(5.80e-6, 13.5e-6, 33.1e-6);
static const float3 kBetaMie       = float3(21.0e-6, 21.0e-6, 21.0e-6);
static const float  kScaleHeightR  = 7994.0;
static const float  kScaleHeightM  = 1200.0;
static const float  kMieG          = 0.76;
static const float  kPi            = 3.14159265;

static const int kViewSamples  = 24;
static const int kLightSamples = 8;

// Intersections of a ray with a sphere centred on the origin.
bool raySphere(float3 ro, float3 rd, float radius, out float t0, out float t1)
{
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) { t0 = 0.0; t1 = 0.0; return false; }
    disc = sqrt(disc);
    t0 = -b - disc;
    t1 = -b + disc;
    return true;
}

// World space has the observer near the origin with +Y up, so shift into
// planet-centred space for the atmosphere integral.
float3 toPlanetSpace(float3 p)
{
    return float3(p.x, p.y + kEarthRadius, p.z);
}

// Integrates in-scattering and transmittance from the camera out to maxDist.
void scatterAtmosphere(float3 ro, float3 rd, float maxDist, float3 sunDir, float intensity,
                       out float3 inscatter, out float3 transmittance)
{
    inscatter     = float3(0.0, 0.0, 0.0);
    transmittance = float3(1.0, 1.0, 1.0);

    float3 origin = toPlanetSpace(ro);

    float a0, a1;
    if (!raySphere(origin, rd, kAtmoRadius, a0, a1)) return;
    a0 = max(a0, 0.0);
    a1 = min(a1, maxDist);
    if (a1 <= a0) return;

    float mu = dot(rd, sunDir);
    float phaseR = 3.0 / (16.0 * kPi) * (1.0 + mu * mu);
    float g  = kMieG;
    float g2 = g * g;
    float phaseM = 3.0 / (8.0 * kPi) * ((1.0 - g2) * (1.0 + mu * mu)) /
                   ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));

    float3 sumR = float3(0.0, 0.0, 0.0);
    float3 sumM = float3(0.0, 0.0, 0.0);
    float  odR = 0.0, odM = 0.0;

    for (int i = 0; i < kViewSamples; ++i)
    {
        // Quadratic sample distribution, clustered toward the camera. A horizon
        // ray crosses a couple of hundred kilometres of atmosphere, and spacing
        // samples uniformly puts them ~12 km apart through the densest, most
        // strongly scattering air right in front of the viewer. The integral
        // then under-reports badly and the sky goes *dark* toward the horizon
        // instead of brightening into haze.
        float f0 = float(i)       / float(kViewSamples);
        float f1 = float(i + 1)   / float(kViewSamples);
        float ta = lerp(a0, a1, f0 * f0);
        float tb = lerp(a0, a1, f1 * f1);
        float segment = tb - ta;

        float3 sp = origin + rd * (ta + segment * 0.5);
        float  height = max(length(sp) - kEarthRadius, 0.0);

        float hr = exp(-height / kScaleHeightR) * segment;
        float hm = exp(-height / kScaleHeightM) * segment;
        odR += hr;
        odM += hm;

        // Optical depth from this sample toward the sun.
        float l0, l1;
        raySphere(sp, sunDir, kAtmoRadius, l0, l1);
        float lSeg = max(l1, 0.0) / float(kLightSamples);
        float lt = 0.0;
        float odLR = 0.0, odLM = 0.0;

        for (int j = 0; j < kLightSamples; ++j)
        {
            float3 lp = sp + sunDir * (lt + lSeg * 0.5);

            // Clamp rather than discard. Dropping a sample outright the moment
            // its light ray dips below the surface leaves a strip of sky near
            // the horizon with zero in-scatter, which reads as a hard dark band
            // across the frame. Clamping to sea level instead accumulates a
            // very large optical depth, so the same region darkens smoothly.
            float lh = max(length(lp) - kEarthRadius, 0.0);
            odLR += exp(-lh / kScaleHeightR) * lSeg;
            odLM += exp(-lh / kScaleHeightM) * lSeg;
            lt += lSeg;
        }

        float3 tau = kBetaRayleigh * (odR + odLR) + kBetaMie * 1.1 * (odM + odLM);
        float3 att = exp(-tau);
        sumR += att * hr;
        sumM += att * hm;
    }

    inscatter     = intensity * (sumR * kBetaRayleigh * phaseR + sumM * kBetaMie * phaseM);
    transmittance = exp(-(kBetaRayleigh * odR + kBetaMie * 1.1 * odM));
}

// Sky along a ray that hits nothing, including the solar disc.
float3 skyRadiance(float3 ro, float3 rd, float3 sunDir, float intensity)
{
    float3 inscatter, transmittance;
    scatterAtmosphere(ro, rd, 1e9, sunDir, intensity, inscatter, transmittance);

    // The sun subtends about half a degree. Softened slightly at the limb so it
    // does not alias into a hard-edged disc.
    float mu   = dot(rd, sunDir);
    float disc = smoothstep(0.99987, 0.99994, mu);
    inscatter += transmittance * disc * intensity * 12.0;

    return inscatter;
}
