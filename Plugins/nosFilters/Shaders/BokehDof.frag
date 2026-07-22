// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
// Single-pass 2D bokeh depth-of-field with a kernel-texture shaping the bokeh.
//
// Computes a per-pixel circle of confusion (CoC) from a linear view-space Z
// input, then gathers samples on a Vogel (golden-angle) disc within that CoC.
// Each sample's contribution is weighted by BokehShape sampled at the same
// unit-disc position, so the bokeh takes on the shape painted into BokehShape
// (regular polygon, ring, custom artwork, etc.).

#version 450

#define MASK_THRESHOLD 0.001
#define GOLDEN_ANGLE   2.39996322972865332

layout(binding = 0) uniform sampler2D Input;
layout(binding = 1) uniform sampler2D Depth;
layout(binding = 2) uniform sampler2D BokehShape;
layout(binding = 3) uniform BokehDofParams
{
    // Focus distance in the same units as the Depth input (linear view-space Z).
    float FocusDistance;
    // Thin-lens radius in the same world units as the Depth input.
    float Aperture;
    // Vertical field of view in degrees; projects the world-space blur radius to pixels.
    float Fov;
    // Maximum CoC radius in pixels.
    float MaxRadius;
    // Skip the gather when CoC <= MinRadius (keeps focused regions crisp & cheap).
    float MinRadius;
    // 0 = treat zero depth as "near focus" (stays sharp); 1 = treat as far plane.
    float BackgroundIsFar;
    // Total Vogel-disc sample count. ~32 = soft, ~64 = clean, ~128 = no banding.
    float SampleCount;
    // Rotate the kernel lookup (radians). Useful for animated highlights.
    float KernelRotation;
}
Params;

layout(location = 0) out vec4 rt;
layout(location = 0) in vec2 uv;

// Thin lens: a point at Z with focus at F blurs to a world-space radius
// Aperture * |Z - F| / F at its own plane; dividing by Z projects it to screen.
// CocScale converts that to pixels: ImageHeight / (2 * tan(Fov / 2)).
float CocFromDepth(float Z, float CocScale)
{
    float F = max(Params.FocusDistance, 1e-4);
    float Coc;
    if (Z <= 0.0)
        // Z -> infinity limit of |Z - F| / (F * Z) is 1 / F; 0 keeps zero depth sharp.
        Coc = Params.BackgroundIsFar * Params.Aperture / F * CocScale;
    else
        Coc = Params.Aperture * abs(Z - F) / (F * Z) * CocScale;
    return clamp(Coc, 0.0, Params.MaxRadius);
}

void main()
{
    vec2 TextureSize = textureSize(Input, 0);
    vec2 TexelSize   = 1.0 / TextureSize;

    float CocScale = TextureSize.y / (2.0 * tan(radians(Params.Fov) * 0.5));

    vec4  CenterColor = texture(Input, uv);
    float CenterZ     = texture(Depth, uv).r;
    float CenterCoC   = CocFromDepth(CenterZ, CocScale);

    if (CenterCoC <= Params.MinRadius || Params.MaxRadius < MASK_THRESHOLD)
    {
        rt = CenterColor;
        return;
    }

    int   N        = int(max(1.0, Params.SampleCount));
    float CosR     = cos(Params.KernelRotation);
    float SinR     = sin(Params.KernelRotation);

    // Vogel disc: golden-angle spiral with sqrt radius for uniform area density.
    // Sample 0 is the center; included implicitly via CenterColor initialization.
    vec4  Accum  = CenterColor;
    float Weight = texture(BokehShape, vec2(0.5)).r;
    Accum       *= Weight;

    for (int i = 1; i < N; ++i)
    {
        float Frac = float(i) / float(N);
        float R    = sqrt(Frac);                          // unit-disc radius
        float Th   = float(i) * GOLDEN_ANGLE;
        vec2  Unit = vec2(cos(Th) * R, sin(Th) * R);      // unit disc position

        // Rotated lookup into the bokeh kernel.
        vec2 ShapeUv = vec2(Unit.x * CosR - Unit.y * SinR,
                            Unit.x * SinR + Unit.y * CosR) * 0.5 + 0.5;
        float WShape = texture(BokehShape, ShapeUv).r;
        if (WShape <= MASK_THRESHOLD)
            continue;

        vec2  Ofs    = Unit * CenterCoC * TexelSize;
        vec4  Sample = texture(Input, uv + Ofs);
        float ZSamp  = texture(Depth, uv + Ofs).r;
        float CocSmp = CocFromDepth(ZSamp, CocScale);

        // Per-sample CoC rejection prevents in-focus pixels bleeding outward.
        // A sample contributes only if its own CoC is at least its distance from center.
        float Dist = R * CenterCoC;
        float WCoc = Dist <= CocSmp ? 1.0 : 0.0;

        float W = WShape * WCoc;
        Accum  += Sample * W;
        Weight += W;
    }

    rt = Accum / max(Weight, 1e-4);
}
