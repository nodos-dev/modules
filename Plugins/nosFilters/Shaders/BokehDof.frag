// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
// Single-pass thin-lens depth-of-field: scatter-as-gather disk sampler over a
// colour texture and a linearised depth texture (R = Z in metres, <= 0 =
// background). The gather radius is the center pixel's CoC; each tap is
// weighted by how far its own CoC disc reaches, which prevents sharp
// foregrounds bleeding onto a blurred background. Each tap is additionally
// shaped by the BokehShape kernel texture, sampled where this pixel sits inside
// the tap's own CoC disc; a plain white disc kernel gives a circular aperture.

#version 450

#define GOLDEN_ANGLE 2.39996323
// Tap spacing the adaptive sample count aims for inside the gather disc (px).
#define TAP_SPACING  1.5
// Never take fewer taps than this, even for a tiny gather radius.
#define MIN_TAPS     8.0

layout(binding = 0) uniform sampler2D Input;
layout(binding = 1) uniform sampler2D Depth;
layout(binding = 2) uniform sampler2D BokehShape;
layout(binding = 3) uniform BokehDofParams
{
    // Lens focal length in millimetres.
    float FocalMm;
    // Aperture diameter in millimetres (focal length / f-stop).
    float ApertureMm;
    // Focus distance in metres.
    float FocusM;
    // Sensor-millimetre to output-pixel scale.
    float PxPerMm;
    // Largest CoC (pixels) this pass can represent; also the gather ceiling.
    float MaxBlurPx;
    // Render the CoC heatmap (green sharp -> red max blur) instead of the image.
    bool ShowCoC;
    // Ceiling for the adaptive tap count. The pass takes as many disc taps as the
    // gather radius needs for ~1.5 px spacing, but never more than this.
    float SampleCount;
    // Rotate the kernel lookup (radians). Useful for animated highlights.
    float KernelRotation;
}
Params;

layout(location = 0) out vec4 rt;
layout(location = 0) in vec2 uv;

// Thin lens: CoC in mm at the sensor, projected to pixels. Z <= 0 stays sharp.
float ComputeCoc(float Z)
{
    if (Z <= 0.0)
        return 0.0;
    float Fm    = Params.FocalMm * 0.001;                // focal length, metres
    float Denom = max(Z * (Params.FocusM - Fm), 1e-4);   // guard near singularity
    float CocMm = Params.FocalMm * Params.ApertureMm * abs(Z - Params.FocusM) / (Denom * 1000.0);
    return clamp(CocMm * Params.PxPerMm, 0.0, Params.MaxBlurPx);
}

void main()
{
    vec2 Resolution = vec2(textureSize(Input, 0));

    float CenterZ   = texture(Depth, uv).r;
    float CenterCoc = ComputeCoc(CenterZ);

    if (Params.ShowCoC)
    {
        float N = clamp(CenterCoc / max(Params.MaxBlurPx, 1.0), 0.0, 1.0);
        rt = vec4(mix(vec3(0.0, 1.0, 0.2), vec3(1.0, 0.0, 0.0), N), 1.0);
        return;
    }

    float GatherR = max(CenterCoc, 1.0);

    // Enough taps to keep spacing inside the gather disc near TAP_SPACING,
    // capped by SampleCount. Sharp pixels have a ~1 px radius and stay cheap.
    int N = int(clamp(GatherR * GatherR / (TAP_SPACING * TAP_SPACING),
                      MIN_TAPS, max(MIN_TAPS, Params.SampleCount)));

    float CosR = cos(Params.KernelRotation);
    float SinR = sin(Params.KernelRotation);

    vec4  Accum  = texture(Input, uv);
    float Weight = 1.0;

    for (int i = 0; i < N; ++i)
    {
        float T     = (float(i) + 0.5) / float(N);
        float R     = sqrt(T);
        float Theta = float(i) * GOLDEN_ANGLE;

        vec2 OffsetPx = vec2(cos(Theta), sin(Theta)) * R * GatherR;
        vec2 SampleUv = uv + OffsetPx / Resolution;

        vec4  SampleColor = texture(Input, SampleUv);
        float SampleZ     = texture(Depth, SampleUv).r;
        float SampleCoc   = ComputeCoc(SampleZ);

        float Cover = clamp(SampleCoc - length(OffsetPx) + 0.5, 0.0, 1.0);

        // This pixel's position inside the tap's own CoC disc, in kernel UVs.
        vec2  KPos    = -OffsetPx / max(SampleCoc, 1.0);
        vec2  ShapeUv = vec2(KPos.x * CosR - KPos.y * SinR,
                             KPos.x * SinR + KPos.y * CosR) * 0.5 + 0.5;
        float WShape  = texture(BokehShape, clamp(ShapeUv, 0.0, 1.0)).r;

        float W = Cover * WShape;
        Accum  += SampleColor * W;
        Weight += W;
    }

    rt = Accum / max(Weight, 1e-4);
}
