// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

// Nodal spherical projection: re-photograph a perspective plate from the centre
// of a sphere it is projected onto. Projector (the plate's camera) and the virtual
// viewer share one optical centre, so this is a pure rotation-only reprojection -
// no geometry or depth needed. For each output pixel we build a view ray from the
// virtual camera, rotate it by pan/tilt/roll, then project that world direction
// through the plate's pinhole to fetch the source pixel. Both cameras look down -Z
// with +Y up (matches the vp-projector reference).

#version 450

layout(binding = 0) uniform sampler2D Input;
layout(binding = 1) uniform SphereProjectParams
{
    float PlateFocal;    // projector focal length (mm)
    float SensorWidth;   // horizontal sensor width (mm), shared by plate & camera
    float CameraFocal;   // virtual camera focal length (mm) - zoom
    float Pan;           // yaw about world +Y (degrees)
    float Tilt;          // pitch about world +X (degrees)
    float Roll;          // roll about the view axis (degrees)
    uvec2 Resolution;    // output resolution; the node sizes the render target to it
    vec4 Background;     // fill for directions outside the plate cone
}
Params;

layout(location = 0) out vec4 rt;
layout(location = 0) in vec2 uv;

const float DEG2RAD = 0.01745329251994329577;

mat3 rotY(float a) { float c = cos(a), s = sin(a); return mat3(c, 0.0, -s,  0.0, 1.0, 0.0,  s, 0.0, c); }
mat3 rotX(float a) { float c = cos(a), s = sin(a); return mat3(1.0, 0.0, 0.0,  0.0, c, s,  0.0, -s, c); }
mat3 rotZ(float a) { float c = cos(a), s = sin(a); return mat3(c, s, 0.0,  -s, c, 0.0,  0.0, 0.0, 1.0); }

void main()
{
    // Virtual camera ray in camera space (looks -Z, +Y up). Horizontal half-angle
    // tangent from focal/sensor; vertical follows the output aspect.
    float aspect = float(Params.Resolution.x) / float(Params.Resolution.y);
    float tanX = Params.SensorWidth / (2.0 * Params.CameraFocal);
    float tanY = tanX / aspect;
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec3 rayCam = normalize(vec3(ndc.x * tanX, ndc.y * tanY, -1.0));

    // Pan/tilt/roll rig: roll on the lens, then tilt, then pan.
    mat3 R = rotY(Params.Pan * DEG2RAD) * rotX(Params.Tilt * DEG2RAD) * rotZ(Params.Roll * DEG2RAD);
    vec3 d = R * rayCam;

    // Project the world direction through the plate's pinhole (axis -Z, image y-down).
    float behind = -d.z;
    if (behind <= 1e-6) { rt = Params.Background; return; }

    vec2 plateSize = vec2(textureSize(Input, 0));
    float fpx = (Params.PlateFocal / Params.SensorWidth) * plateSize.x;
    vec2 c = plateSize * 0.5;
    float upx = c.x + fpx * (d.x / behind);
    float vpx = c.y - fpx * (d.y / behind); // world +Y up -> image v down
    vec2 sampleUV = vec2(upx, vpx) / plateSize;

    if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0))))
    {
        rt = Params.Background;
        return;
    }
    rt = texture(Input, sampleUV);
}
