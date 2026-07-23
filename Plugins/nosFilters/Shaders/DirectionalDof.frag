// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
// Directional depth-of-field pass.
// Scatter-as-gather along Direction: every tap within the MaxCoc search radius
// contributes where its own thin-lens CoC reaches this pixel, weighted 1 / CoC
// so energy is conserved along the line. Chain two instances (Direction = (1,0)
// and Direction = (0,1)) for a separable approximation of disc bokeh.

#version 450

#define MASK_THRESHOLD 0.001
// A source pixel cannot spread over less than its own pixel: floor for CoC (px).
#define MIN_COC        0.5
// Half-width of the soft edge on coverage (px); 1 px total anti-aliased edge.
#define COVER_FEATHER  0.5
// Tap spacing the adaptive sample count aims for along the line (px).
#define TAP_SPACING    1.5
// Never take fewer taps per side than this, even for a tiny search radius.
#define MIN_TAPS       4.0

layout(binding = 0) uniform sampler2D Input;
layout(binding = 1) uniform sampler2D Depth;
layout(binding = 2) uniform DirectionalDofParams
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
    // 0 = treat zero depth as "no info, keep sharp"; 1 = treat zero depth as far.
    float BackgroundIsFar;
    vec2 Direction;
    // Ceiling for the adaptive tap count (one side; total taps = 2*N+1). The pass
    // takes as many taps as MaxCoc needs for ~1.5 px spacing, but never more.
    float SampleCount;
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
        // Picking far avoids halos around empty regions.
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

    vec2 Dir = normalize(Params.Direction);

    // Enough taps to keep spacing near TAP_SPACING over the search radius, capped
    // by SampleCount. No per-pixel early-out: a sharp pixel must still receive
    // bleed from defocused neighbors.
    int   N    = int(clamp(Params.MaxCoc / TAP_SPACING, MIN_TAPS, max(MIN_TAPS, Params.SampleCount)));
    float Step = Params.MaxCoc / float(N);

    vec4  Accum  = vec4(0.0);
    float Weight = 0.0;

    for (int i = -N; i <= N; ++i)
    {
        float T   = float(i) * Step;
        vec2  Ofs = Dir * T * TexelSize;

        float ZSamp  = texture(Depth, uv + Ofs).r;
        float CocSmp = CocFromDepth(ZSamp, CocScale);

        // The sample contributes only where its own CoC reaches this pixel;
        // soft edge instead of a binary cut. Weight 1 / CoC spreads its energy
        // over its blur length.
        float Cover = 1.0 - smoothstep(CocSmp - COVER_FEATHER, CocSmp + COVER_FEATHER, abs(T));
        float W     = Cover / max(CocSmp, MIN_COC);

        Accum  += texture(Input, uv + Ofs) * W;
        Weight += W;
    }

    rt = Weight > 1e-4 ? Accum / Weight : CenterColor;
}
