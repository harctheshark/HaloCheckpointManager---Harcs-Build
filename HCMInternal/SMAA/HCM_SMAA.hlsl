// HCM addition (2026-11-20, user: "Did we ever add SMAA to HCM? ... i do not want to have to launch reshade seperately").
// The three passes of SMAA 1x (Jorge Jimenez et al., MIT - SMAA.hlsl + SMAA_LICENSE.txt in this folder) for HCM's D3D11
// Present hook (SMAAPostProcess.cpp). Precompiled with fxc into Generated\SMAAShaders.h by build_shaders.py.
//
// 2026-11-21 (user: "it definitely helps but i think we need to tune the sensitivity"): ONE shader set, tuned at run time.
// The edge threshold, search steps, diagonal steps and corner rounding come from the constant buffer (SMAA.hlsl only uses
// them as values, never in #if), so HCM's sliders change them live. Two edge detectors (luma / colour), two blend passes
// (with / without diagonal search - SMAA's Low / Medium presets compile it out), and an edges debug view.
// Neighbourhood blending is gamma-correct: it reads the frame through an sRGB view (linear light, as SMAA recommends) and
// encodes the result back to sRGB when the target is a plain UNORM back buffer (g_linearOut).
//
// Our own porting macros (SMAA_CUSTOM_SL) so the two samplers sit at fixed registers: SMAA.hlsl's own HLSL4 path declares
// them Effects-style ({ Filter = ... }), which a plain compile ignores and binds wherever the compiler likes.
//
// Bindings (SMAAPostProcess.cpp):
//   b0  metrics (1/w, 1/h, w, h); params (threshold, search steps, diagonal steps, corner rounding 0..100);
//       params2 (local contrast factor, linear out 0/1, -, -)
//   s0  linear clamp, s1 point clamp
//   t0  the frame (edges / debug: the plain view; neighbourhood: the sRGB view when there is one)
//   t1  edges   t2 blend weights   t3 area table   t4 search table
// Every pass draws one full-screen triangle (3 vertices, no vertex buffer).

#define SMAA_CUSTOM_SL 1
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)

cbuffer SMAAConstants : register(b0)
{
    float4 g_smaaMetrics;
    float4 g_smaaParams;    // threshold, max search steps, max diagonal steps, corner rounding (0..100; 100 = no corner rounding)
    float4 g_smaaParams2;   // local contrast adaptation factor, linear out (1 = encode to sRGB), -, -
    float4 g_smaaPred;      // depth predication (2026-11-21): log2-depth edge threshold, scale, strength, depth reversed (1) / not (0)
    float4 g_smaaDepthUv;   // back-buffer uv -> game depth copy uv: scale xy, offset zw
};
#define SMAA_RT_METRICS g_smaaMetrics
#define SMAA_THRESHOLD g_smaaParams.x
#define SMAA_MAX_SEARCH_STEPS g_smaaParams.y
#define SMAA_MAX_SEARCH_STEPS_DIAG g_smaaParams.z
#define SMAA_CORNER_ROUNDING g_smaaParams.w
#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR g_smaaParams2.x
#ifdef HCM_NODIAG
#define SMAA_DISABLE_DIAG_DETECTION
#endif
// Predicated thresholding (/D HCM_PRED): the predication texture is the game's depth as log2(depth-to-camera ratio) at back
// buffer size (DepthLogPS). Where it has no edge the threshold is scale x the user's, on a depth edge scale x (1 - strength)
// x it; HCM sets strength = 1 - 1/scale, so geometry edges keep exactly the user's threshold.
#ifdef HCM_PRED
#define SMAA_PREDICATION 1
#define SMAA_PREDICATION_THRESHOLD g_smaaPred.x
#define SMAA_PREDICATION_SCALE g_smaaPred.y
#define SMAA_PREDICATION_STRENGTH g_smaaPred.z
#endif

#include "SMAA.hlsl"

Texture2D colorTex  : register(t0);
Texture2D edgesTex  : register(t1);
Texture2D blendTex  : register(t2);
Texture2D areaTex   : register(t3);
Texture2D searchTex : register(t4);
Texture2D predTex   : register(t5);   // log depth at back-buffer size (DepthLogPS's output), for the predicated edge passes
Texture2D gameDepth : register(t6);   // the game's depth copy (GameDepthTap), for DepthLogPS

// the full-screen triangle: (0,0) (2,0) (0,2) in texture space
void FullScreen(uint id, out float4 position, out float2 texcoord)
{
    texcoord = float2((id << 1) & 2, id & 2);
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// ---- pass 1: edge detection on the gamma-space frame (as SMAA recommends) ----
void EdgeVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0, out float4 offset[3] : TEXCOORD1)
{
    FullScreen(id, position, texcoord);
    SMAAEdgeDetectionVS(texcoord, offset);
}
float4 LumaEdgePS(float4 position : SV_Position, float2 texcoord : TEXCOORD0, float4 offset[3] : TEXCOORD1) : SV_Target
{
#ifdef HCM_PRED
    return float4(SMAALumaEdgeDetectionPS(texcoord, offset, colorTex, predTex), 0.0, 0.0);
#else
    return float4(SMAALumaEdgeDetectionPS(texcoord, offset, colorTex), 0.0, 0.0);
#endif
}
float4 ColorEdgePS(float4 position : SV_Position, float2 texcoord : TEXCOORD0, float4 offset[3] : TEXCOORD1) : SV_Target
{
#ifdef HCM_PRED
    return float4(SMAAColorEdgeDetectionPS(texcoord, offset, colorTex, predTex), 0.0, 0.0);
#else
    return float4(SMAAColorEdgeDetectionPS(texcoord, offset, colorTex), 0.0, 0.0);
#endif
}

// ---- pass 0 (with depth): the game's depth -> log2 of its distance scale, at back-buffer size ----
// Reversed-Z d ~ near / z, ordinary z: 1 - d ~ near / z; log2 of that is -log2(z) + const, so a fixed threshold is a fixed
// RELATIVE depth step (0.07 ~ 5%) at any distance. Sky / cleared depth (0) clamps to a far value.
float4 DepthLogPS(float4 position : SV_Position, float2 texcoord : TEXCOORD0, float4 offset : TEXCOORD1) : SV_Target
{
    float2 uv = texcoord * g_smaaDepthUv.xy + g_smaaDepthUv.zw;
    float d = gameDepth.SampleLevel(PointSampler, uv, 0).r;
    float k = g_smaaPred.w > 0.5 ? d : 1.0 - d;
    return float4(log2(max(k, 1e-7)), 0.0, 0.0, 0.0);
}

// ---- pass 2: blending weights (BlendPS compiled twice: with diagonals, and with /D HCM_NODIAG) ----
void BlendVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0, out float2 pixcoord : TEXCOORD1, out float4 offset[3] : TEXCOORD2)
{
    FullScreen(id, position, texcoord);
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
}
float4 BlendPS(float4 position : SV_Position, float2 texcoord : TEXCOORD0, float2 pixcoord : TEXCOORD1, float4 offset[3] : TEXCOORD2) : SV_Target
{
    return SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edgesTex, areaTex, searchTex, float4(0.0, 0.0, 0.0, 0.0));
}

// ---- pass 3: neighbourhood blending (writes the back buffer) ----
float3 HCMLinearToSRGB(float3 c)
{
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
void NeighborVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0, out float4 offset : TEXCOORD1)
{
    FullScreen(id, position, texcoord);
    SMAANeighborhoodBlendingVS(texcoord, offset);
}
float4 NeighborPS(float4 position : SV_Position, float2 texcoord : TEXCOORD0, float4 offset : TEXCOORD1) : SV_Target
{
    float4 c = SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex);
    if (g_smaaParams2.y > 0.5) c.rgb = HCMLinearToSRGB(c.rgb);
    return c;
}

// ---- debug: the detected edges over the frame (red = an edge on the pixel's left, green = on its top, yellow = both) ----
float4 EdgeDebugPS(float4 position : SV_Position, float2 texcoord : TEXCOORD0, float4 offset : TEXCOORD1) : SV_Target
{
    float4 c = colorTex.SampleLevel(PointSampler, texcoord, 0);
    float2 e = edgesTex.SampleLevel(PointSampler, texcoord, 0).rg;
    c.rgb *= 0.45;   // the frame dimmed, so the edges stand out
    if (e.r + e.g > 0.0) c.rgb = float3(e.r, e.g, 0.0);
    return c;
}
