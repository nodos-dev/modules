// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
// Single-pass 2D bokeh depth-of-field with a kernel-texture shaping the bokeh.
//
// Scatter-as-gather: every pixel within the MaxCoc search radius is treated as
// a scatterer whose radiance spreads over its own thin-lens CoC disc, shaped by
// BokehShape and normalized by disc area (1 / CoC^2) so energy is conserved. A
// sample contributes to this pixel only where its disc reaches it, so defocused
// foregrounds bleed over sharp neighbors as a real lens does.

#version 450

#define MASK_THRESHOLD 0.001
#define GOLDEN_ANGLE   2.39996322972865332
// A source pixel cannot spread over less than its own pixel: floor for CoC (px).
#define MIN_COC        0.5
// Half-width of the soft edge on disc coverage (px); 1 px total anti-aliased edge.
#define COVER_FEATHER  0.5
// Tap spacing the adaptive sample count aims for inside the search disc (px).
#define TAP_SPACING    1.5
// Never take fewer taps than this, even for a tiny search radius.
#define MIN_TAPS       8.0

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
    // Largest CoC (pixels) this pass can represent, and the gather search radius.
    // Blur beyond it is truncated. Zero disables the effect.
    float MaxCoc;
    // 0 = treat zero depth as "near focus" (stays sharp); 1 = treat as far plane.
    float BackgroundIsFar;
    // Ceiling for the adaptive tap count. The pass takes as many Vogel-disc taps
    // as MaxCoc needs for ~1.5 px spacing, but never more than this.
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
    return clamp(Coc, 0.0, Params.MaxCoc);
}

void main()
{
    vec2 TextureSize = textureSize(Input, 0);
    vec2 TexelSize   = 1.0 / TextureSize;

    float CocScale = TextureSize.y / (2.0 * tan(radians(Params.Fov) * 0.5));

    vec4 CenterColor = texture(Input, uv);
    if (Params.MaxCoc < MASK_THRESHOLD)
    {
        rt = CenterColor;
        return;
    }

    // Enough taps to keep spacing inside the search disc near TAP_SPACING, capped
    // by SampleCount. No per-pixel early-out: a sharp pixel must still receive
    // bleed from defocused neighbors.
    int N = int(clamp(Params.MaxCoc * Params.MaxCoc / (TAP_SPACING * TAP_SPACING),
                      MIN_TAPS, max(MIN_TAPS, Params.SampleCount)));

    float CosR = cos(Params.KernelRotation);
    float SinR = sin(Params.KernelRotation);

    // Vogel disc over the fixed search radius; sample 0 is the center tap.
    vec4  Accum  = vec4(0.0);
    float Weight = 0.0;

    for (int i = 0; i < N; ++i)
    {
        float Frac = float(i) / float(N);
        float R    = sqrt(Frac);                          // unit-disc radius
        float Th   = float(i) * GOLDEN_ANGLE;
        vec2  Unit = vec2(cos(Th) * R, sin(Th) * R);      // unit disc position

        vec2  Ofs    = Unit * Params.MaxCoc * TexelSize;
        float ZSamp  = texture(Depth, uv + Ofs).r;
        float CocSmp = CocFromDepth(ZSamp, CocScale);
        float DistPx = R * Params.MaxCoc;                 // sample-to-center distance

        // The sample contributes only where its own disc reaches this pixel;
        // soft edge instead of a binary cut.
        float Cover = 1.0 - smoothstep(CocSmp - COVER_FEATHER, CocSmp + COVER_FEATHER, DistPx);
        if (Cover <= 0.0)
            continue;

        // Kernel lookup in the source disc's own coordinates: this pixel sits at
        // -Unit * MaxCoc / CoC inside the sample's disc.
        float CocSafe = max(CocSmp, MIN_COC);
        vec2  KPos    = -Unit * (Params.MaxCoc / CocSafe);
        vec2  ShapeUv = vec2(KPos.x * CosR - KPos.y * SinR,
                             KPos.x * SinR + KPos.y * CosR) * 0.5 + 0.5;
        float WShape  = texture(BokehShape, clamp(ShapeUv, 0.0, 1.0)).r;
        if (WShape <= MASK_THRESHOLD)
            continue;

        // Energy conservation: the source spreads its radiance over its disc area.
        float W = WShape * Cover / (CocSafe * CocSafe);
        Accum  += texture(Input, uv + Ofs) * W;
        Weight += W;
    }

    // Ring-shaped kernels can zero out every tap of a sharp pixel; pass through.
    rt = Weight > 1e-4 ? Accum / Weight : CenterColor;
}
