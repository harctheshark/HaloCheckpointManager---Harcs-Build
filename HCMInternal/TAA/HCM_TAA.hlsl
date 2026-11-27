// HCM_TAA.hlsl - Halo 2 (MCC 1.3528) temporal anti-aliasing resolve (H2TAA.cpp, 2026-11-23).
// Precompiled by TAA\build_shaders.py (fxc) into TAA\Generated\TAAShaders.h - run it again after editing this file.
//
// One full-screen pass inside the game's frame (sub_180951EC0 entry: the finished 3D frame, before bloom and the HUD):
//   t0 the current (jittered) frame, a copy of the scene target
//   t1 the history (RGBA16F, last frame's result)
//   t2 the WORLD depth (GameDepthTap's copy at the first-person pass entry - the gun is not in it)
//   t3 the scene depth at resolve time: the first-person pass cleared it, so it holds only the gun (the weapon mask)
// Output: SV_Target0 the new history, SV_Target1 the scene target itself (alpha kept from the current frame).
//
// Reprojection: no velocity buffer exists. Each pixel's clip position (ndc x / y, raw device depth, 1) goes through
// gReproj = inverse(current view-projection) * previous view-projection (both unjittered, built on the CPU in double from
// the engine's world_to_view 0x1996B10 + projection 0x1996B88, the reversed depth folded in as the Collision Viewer does),
// i.e. camera motion only. Depth at the clear value (sky) uses the rotation-only version (a direction, no parallax).
// Gun pixels get zero motion. History: Catmull-Rom sample, clipped to the current 3x3 neighbourhood (YCoCg, variance box).
//
// v2 (2026-11-23):
//   t4 the OBJECT motion (RG16F, pixels to add to a pixel's position to get where it was last frame; x >= 20000 = none) -
//      written by HCM_TAAProxy.hlsl for the pixels of objects that moved by themselves (H2TAAObjects.cpp); those pixels use
//      it instead of the camera's motion.
//   Motion is taken from the pixel of the 3x3 neighbourhood that is NEAREST the camera (the usual "closest depth" velocity
//   dilation): an anti-aliased silhouette pixel is part object, part background, and must follow the object in front.

cbuffer TAAConstants : register(b0)
{
    row_major float4x4 gReproj;     // current clip -> previous clip (row vector: mul(v, M))
    row_major float4x4 gReprojSky;  // the same without the camera translation (sky / far plane)
    float4 gRect;                   // the view inside the scene target: left, top, width, height (pixels)
    float4 gTex;                    // 1 / width, 1 / height, width, height of the scene target (= history)
    float4 gParams;                 // x weight of the current frame, y 1 = no history this frame, z debug view, w 1 = reversed depth
    float4 gDepth;                  // xy world-depth texels per scene pixel, zw the same for the gun depth
    float4 gParams2;                // x 1 = the object motion texture is valid this frame, y the current frame's weight for moving
                                    // pixels (<= gParams.x: no speed limit), z the screen speed (pixels a frame) at which it is reached
    // the first-person model (arms + gun), all in world space RELATIVE TO THIS FRAME'S CAMERA (small numbers):
    row_major float4x4 gFpInvVP;    // the first-person pass's clip (ndc x, ndc y, gun depth, 1) -> position
    row_major float4x4 gFpPrevVP;   // position -> last frame's first-person clip
    float4 gFp;                     // x number of bones in gFpNodes (0 = the gun keeps zero motion), y furthest a gun pixel can be
};

// one bone of the first-person model that has a pose from last frame too
struct FpNode
{
    float4 origin;      // xyz where the bone is now
    float4 d0, d1, d2;  // position now -> position last frame (rows)
};

Texture2D<float4> gCur    : register(t0);
Texture2D<float4> gHist   : register(t1);
Texture2D<float>  gWorldZ : register(t2);
Texture2D<float>  gFpZ    : register(t3);
Texture2D<float2> gObj    : register(t4);
StructuredBuffer<FpNode> gFpNodes : register(t5);
SamplerState gLinear : register(s0);

void FullscreenVS(uint id : SV_VertexID, out float4 pos : SV_Position)
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 RGBToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 YCoCgToRGB(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Playdead (Inside, 2016): pull q towards the box centre until it is inside the box
float3 ClipAABB(float3 bmin, float3 bmax, float3 q)
{
    const float3 centre = 0.5 * (bmax + bmin);
    const float3 extent = 0.5 * (bmax - bmin) + 1e-5;
    const float3 v = q - centre;
    const float3 a = abs(v / extent);
    const float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? centre + v / m : q;
}

// Catmull-Rom history sample from 9 bilinear taps (sharper than one bilinear tap: less blur while moving)
float3 SampleHistory(float2 uv)
{
    const float2 pos = uv * gTex.zw;
    const float2 t1 = floor(pos - 0.5) + 0.5;
    const float2 f = pos - t1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 u0 = (t1 - 1.0) * gTex.xy;
    const float2 u12 = (t1 + w2 / w12) * gTex.xy;
    const float2 u3 = (t1 + 2.0) * gTex.xy;
    float3 r = 0;
    r += gHist.SampleLevel(gLinear, float2(u0.x, u0.y), 0).rgb * (w0.x * w0.y);
    r += gHist.SampleLevel(gLinear, float2(u12.x, u0.y), 0).rgb * (w12.x * w0.y);
    r += gHist.SampleLevel(gLinear, float2(u3.x, u0.y), 0).rgb * (w3.x * w0.y);
    r += gHist.SampleLevel(gLinear, float2(u0.x, u12.y), 0).rgb * (w0.x * w12.y);
    r += gHist.SampleLevel(gLinear, float2(u12.x, u12.y), 0).rgb * (w12.x * w12.y);
    r += gHist.SampleLevel(gLinear, float2(u3.x, u12.y), 0).rgb * (w3.x * w12.y);
    r += gHist.SampleLevel(gLinear, float2(u0.x, u3.y), 0).rgb * (w0.x * w3.y);
    r += gHist.SampleLevel(gLinear, float2(u12.x, u3.y), 0).rgb * (w12.x * w3.y);
    r += gHist.SampleLevel(gLinear, float2(u3.x, u3.y), 0).rgb * (w3.x * w3.y);
    return max(r, 0.0);
}

struct PSOut
{
    float4 hist  : SV_Target0;
    float4 scene : SV_Target1;
};

PSOut ResolvePS(float4 svpos : SV_Position)
{
    PSOut o;
    const int2 p = int2(svpos.xy);
    const int2 lo = int2(gRect.xy);
    const int2 hi = int2(gRect.xy + gRect.zw) - 1;
    const float4 cur4 = gCur.Load(int3(p, 0));
    const float3 cur = RGBToYCoCg(cur4.rgb);

    // the current frame's 3x3 neighbourhood: min / max and mean / variance
    float3 mn = cur, mx = cur, m1 = cur, m2 = cur * cur;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0) continue;
            const float3 s = RGBToYCoCg(gCur.Load(int3(clamp(p + int2(x, y), lo, hi), 0)).rgb);
            mn = min(mn, s); mx = max(mx, s);
            m1 += s; m2 += s * s;
        }
    }
    m1 *= (1.0 / 9.0); m2 *= (1.0 / 9.0);
    const float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
    const float3 bmin = max(mn, m1 - 1.25 * sigma);
    const float3 bmax = min(mx, m1 + 1.25 * sigma);

    // where this pixel was in the previous frame
    const float fz = gFpZ.Load(int3(int2(svpos.xy * gDepth.zw), 0));
    const bool weapon = fz > 0.0 && fz < 1.0;   // the gun's depth (the first-person pass cleared to 0 or 1 - either convention)
    float2 prevPix = svpos.xy;
    bool onScreen = true;
    bool objectMotion = false;
    bool gunMotion = false;
    if (weapon && gFp.x > 0.5)
    {
        // The first-person arms and gun (2026-11-24). They are not objects and have no collision model, but the engine
        // keeps every bone's world matrix for the frame (halo2+0x165C508). Which bone a pixel hangs on is not known, so it
        // follows the bones NEAR it: its position (from the gun's own depth and the first-person projection) is carried
        // back to last frame by each bone's motion, weighted by 1 / distance^4 to the bone - the nearest one or two
        // decide, and the result is smooth from bone to bone. Zero motion (the gun rigid with the camera) stays the
        // fallback: no bones, an implausible position, or a pixel that was behind the camera.
        const float2 ndc = (svpos.xy - gRect.xy) / gRect.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
        const float4 wh = mul(float4(ndc, fz, 1.0), gFpInvVP);
        if (abs(wh.w) > 1e-9)
        {
            const float3 P = wh.xyz / wh.w;
            const float dist2 = dot(P, P);
            if (dist2 > 0.0001 && dist2 < gFp.y * gFp.y)
            {
                const float4 P4 = float4(P, 1.0);
                float3 acc = float3(0.0, 0.0, 0.0);
                float wsum = 0.0;
                const uint n = (uint)gFp.x;
                [loop] for (uint i = 0; i < n; ++i)
                {
                    const FpNode N = gFpNodes[i];
                    const float3 d = P - N.origin.xyz;
                    float w = 1.0 / (dot(d, d) + 1e-5);
                    w *= w;
                    acc += w * float3(dot(N.d0, P4), dot(N.d1, P4), dot(N.d2, P4));
                    wsum += w;
                }
                if (wsum > 0.0)
                {
                    const float4 pc = mul(float4(acc / wsum, 1.0), gFpPrevVP);
                    if (pc.w > 1e-6)
                    {
                        const float2 was = (pc.xy / pc.w * float2(0.5, -0.5) + 0.5) * gRect.zw + gRect.xy;
                        // a whole-screen jump is not a gun swaying: keep zero motion
                        if (all(abs(was - svpos.xy) < 0.25 * gRect.zw)) { prevPix = was; gunMotion = true; }
                    }
                }
            }
        }
    }
    if (!weapon)
    {
        // the pixel of the 3x3 neighbourhood nearest the camera: its motion is this pixel's
        const bool reversed = gParams.w > 0.5;
        int2 q = p;
        float z = gWorldZ.Load(int3(int2(svpos.xy * gDepth.xy), 0));
        [unroll] for (int dy = -1; dy <= 1; ++dy)
        {
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0) continue;
                const int2 n = clamp(p + int2(dx, dy), lo, hi);
                const float zn = gWorldZ.Load(int3(int2((float2(n) + 0.5) * gDepth.xy), 0));
                if (reversed ? (zn > z) : (zn < z)) { z = zn; q = n; }
            }
        }
        const float2 qpos = float2(q) + 0.5;
        float2 om = gParams2.x > 0.5 ? gObj.Load(int3(q, 0)) : float2(30000.0, 30000.0);
        if (gParams2.x > 0.5 && om.x > 28000.0)
        {
            // The drawn mesh reaches a little past its collision model's outline (the object pass only marks what the
            // collision triangles cover): an unmarked pixel takes the motion of a marked one 3 pixels away when both are
            // about equally far from the camera (device depth ~ 1 / distance: within 3 %).
            const float zl = reversed ? z : 1.0 - z;
            static const int2 kEdge[4] = { int2(-3, 0), int2(3, 0), int2(0, -3), int2(0, 3) };
            bool found = false;
            [unroll] for (int t = 0; t < 4; ++t)
            {
                const int2 n = clamp(q + kEdge[t], lo, hi);
                const float2 on = gObj.Load(int3(n, 0));
                const float zn = gWorldZ.Load(int3(int2((float2(n) + 0.5) * gDepth.xy), 0));
                const float znl = reversed ? zn : 1.0 - zn;
                if (!found && on.x < 20000.0 && abs(znl - zl) <= 0.03 * zl) { om = on; found = true; }
            }
        }
        if (om.x >= 20000.0 && om.x < 28000.0) onScreen = false;   // an object that jumped this frame (a teleport): no history for it
        else if (om.x < 20000.0)
        {
            // an object that moved by itself (H2TAAObjects.cpp): its own motion
            prevPix = svpos.xy + om;
            objectMotion = true;
        }
        else
        {
            const bool far = reversed ? (z <= 0.0) : (z >= 1.0);
            const float2 ndc = (qpos - gRect.xy) / gRect.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
            const float4 pc = mul(float4(ndc, z, 1.0), far ? gReprojSky : gReproj);
            if (pc.w > 1e-6)
                prevPix = svpos.xy + ((pc.xy / pc.w * float2(0.5, -0.5) + 0.5) * gRect.zw + gRect.xy - qpos);
            else
                onScreen = false;
        }
    }
    onScreen = onScreen && all(prevPix >= gRect.xy) && all(prevPix <= gRect.xy + gRect.zw);
    const bool useHist = onScreen && gParams.y < 0.5;

    float3 res = cur;
    if (useHist)
    {
        const float3 h = ClipAABB(bmin, bmax, RGBToYCoCg(SampleHistory(prevPix * gTex.xy)));
        // The speed limit on the history (user, 2026-11-24): a pixel that moves on screen takes more of the new frame. What
        // is fetched for it is only as good as its motion (transparents, the gun and thin parts have none of their own),
        // and every resampled frame softens it. Still pixels keep the full history.
        float weight = gParams.x;
        if (gParams2.y > weight)
            weight = lerp(weight, gParams2.y, saturate(length(prevPix - svpos.xy) / gParams2.z));
        res = lerp(h, cur, weight);
    }
    const float3 rgb = max(YCoCgToRGB(res), 0.0);
    o.hist = float4(rgb, 1.0);
    o.scene = float4(rgb, cur4.a);

    if (gParams.z > 0.5)
    {
        // debug view: red = horizontal motion, green = vertical motion (8 pixels = full), blue = the gun (no motion),
        // orange tint = the gun following its bones, cyan tint = an object's own motion (v2), magenta = no usable history
        // (off screen / reset), over the dimmed frame
        const float2 motion = prevPix - svpos.xy;
        const float l = dot(cur4.rgb, float3(0.299, 0.587, 0.114)) * 0.35;
        float3 d = float3(l, l, l);
        d.r += saturate(abs(motion.x) / 8.0);
        d.g += saturate(abs(motion.y) / 8.0);
        if (weapon) d.b += 0.6;
        if (gunMotion) d = lerp(d, float3(1.0, 0.6, 0.0), 0.4);   // orange tint = the gun follows its bones; plain blue = zero motion
        if (objectMotion) d = lerp(d, float3(0.0, 1.0, 1.0), 0.55);
        if (!useHist) d = float3(0.8, 0.0, 0.8) + l * 0.5;
        o.scene = float4(saturate(d), cur4.a);
    }
    return o;
}
