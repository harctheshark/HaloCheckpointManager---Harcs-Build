// HCM_TAAProxy.hlsl - Halo 2 TAA v2: the OBJECT MOTION pass (H2TAA.cpp / H2TAAObjects.cpp, 2026-11-23; triangles 2026-11-24).
// Precompiled by TAA\build_shaders.py (fxc) into TAA\Generated\TAAShaders.h - run it again after editing this file.
//
// Halo 2 classic has no velocity buffer. The CPU sends, for every object that moved since last frame, what the pass needs
// to say which pixels are that object's and where they were last frame. Two kinds of proxy, one motion target:
//
// TRIANGLES (TriVS / TriPS) - objects with a collision model (bipeds, vehicles, crates, machines). The collision model's
//   triangles (node-local, one static vertex buffer per object definition) are drawn with this frame's node matrices and
//   this frame's view-projection. A pixel is the object's only when the SCENE surface there (world position from the WORLD
//   depth - GameDepthTap's copy, the same one the resolve uses) lies within a tolerance of the triangle's own surface: the
//   collision model is only close to the drawn mesh, never equal to it, and the floor / wall behind it is far from it.
//   (The first version used the nodes' bounding boxes instead: a box around a thin diagonal wing is mostly air, so the
//   floor inside it took the wing's motion - user's screenshots, 2026-11-24.)
// BOXES (ProxyVS / ProxyPS) - objects with no collision model (weapons, projectiles, equipment): the render model's
//   bounding box; a pixel whose world position is inside the box is the object's.
//
// Either way the pixel's position last frame (the node's rigid transform back to last frame, then last frame's
// view-projection) gives the motion: pixels to add to this pixel to get where it was. Where proxies overlap, the best fit
// wins (written as depth, tested LESS): triangles by how near the scene surface is to them (0 .. 0.4), then what lies
// inside a collision hull (0.45 .. 0.7), boxes after every
// triangle (0.75 .. 1). Pixels nothing claims keep the cleared "none" value and use the camera's motion.

struct Proxy
{
    float4 rect;        // ndc x min, y min, x max, y max
    float4 w2l0, w2l1, w2l2;   // world -> node-local rows
    float4 centre;      // node-local
    float4 half;        // node-local half extents
    float4 d0, d1, d2;  // world now -> world last frame rows
};

struct Node
{
    float4 l0, l1, l2;  // node-local -> world rows (this frame)
    float4 d0, d1, d2;  // world now -> world last frame rows
};

cbuffer ProxyConstants : register(b0)
{
    row_major float4x4 gInvVP;      // this frame's clip (ndc x, ndc y, device depth, 1) -> world (row vector: mul(v, M)), un-jittered
    row_major float4x4 gPrevVP;     // world -> last frame's clip, un-jittered
    row_major float4x4 gCurVP;      // world -> this frame's clip, un-jittered (the depth as the device has it)
    float4 gRect;                   // the view inside the target: left, top, width, height (pixels)
    float4 gDepth;                  // xy world-depth texels per target pixel
    float4 gParams;                 // x 1 = reversed depth, y triangle tolerance (world units), zw this frame's jitter in ndc
    float4 gDraw;                   // x first node of this draw (triangles), yz this frame's jitter in pixels, w how far in front of a
                                    // collision triangle the scene surface may be and still be the object (world units)
};

StructuredBuffer<Proxy> gProxies : register(t0);
Texture2D<float> gWorldZ : register(t1);
StructuredBuffer<Node> gNodes : register(t2);
Texture2D<float> gExtent : register(t3);   // TriPS: view depth of the farthest collision face at each pixel (TriExtentPS)

struct ProxyOut
{
    float2 motion : SV_Target0;
    float depth : SV_Depth;
};

// the scene's world position at a pixel; false = sky / nothing usable
bool SceneWorld(float2 pixel, float2 unjittered, out float3 world)
{
    world = float3(0.0, 0.0, 0.0);
    const float z = gWorldZ.Load(int3(int2(pixel * gDepth.xy), 0));
    const bool far = gParams.x > 0.5 ? (z <= 0.0) : (z >= 1.0);
    if (far) return false;
    const float2 ndc = (unjittered - gRect.xy) / gRect.zw * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float4 wh = mul(float4(ndc, z, 1.0), gInvVP);
    if (abs(wh.w) < 1e-9) return false;
    world = wh.xyz / wh.w;
    return true;
}

// ---------------------------------------------------------------- triangles ----------------------------------------------------------------

void TriVS(float3 lpos : POSITION, uint slot : SLOT,
    out float4 pos : SV_Position, out float3 wpos : WPOS, out float viewW : VIEWW, out nointerpolation uint node : NODE)
{
    node = (uint)gDraw.x + slot;
    const Node N = gNodes[node];
    const float4 l = float4(lpos, 1.0);
    wpos = float3(dot(N.l0, l), dot(N.l1, l), dot(N.l2, l));
    pos = mul(float4(wpos, 1.0), gCurVP);
    viewW = pos.w;
    pos.xy += gParams.zw * pos.w;   // the frame was drawn jittered: cover the same pixels
}

// The pass before TriPS, same triangles, blend MAX into gExtent: how far from the camera the collision hulls reach at
// each pixel (view depth of their farthest face).
float TriExtentPS(float4 svpos : SV_Position, float3 wpos : WPOS, float viewW : VIEWW, nointerpolation uint node : NODE) : SV_Target0
{
    return viewW;
}

ProxyOut TriPS(float4 svpos : SV_Position, float3 wpos : WPOS, float viewW : VIEWW, nointerpolation uint node : NODE)
{
    ProxyOut o;
    o.motion = float2(30000.0, 30000.0);
    o.depth = 1.0;
    const float2 unj = svpos.xy - gDraw.yz;   // where this pixel's surface is in the un-jittered picture
    float3 w;
    if (!SceneWorld(svpos.xy, unj, w)) discard;   // sky: no object
    // the depth buffer gets coarse with distance: the tolerance grows with it
    const float tol = gParams.y * (1.0 + 0.02 * max(viewW, 0.0));
    const float d = distance(w, wpos);
    float priority;
    if (d < tol) priority = 0.4 * d / tol;   // the scene surface IS this triangle's (as near as the collision model gets)
    else
    {
        // Not at this triangle - but INSIDE the hull? The drawn mesh has parts well inside its collision hull (a cockpit,
        // the seam between a wing and the body - the "holes" in the user's screenshot, 2026-11-24): what is seen behind
        // this triangle and no further than the hull's farthest face belongs to the object too. The floor / wall behind
        // the object is beyond that face; something in front of the object is in front of this triangle.
        const float sceneW = dot(float4(w, 1.0), float4(gCurVP[0].w, gCurVP[1].w, gCurVP[2].w, gCurVP[3].w));
        const float behind = sceneW - viewW;
        const float last = gExtent.Load(int3(int2(svpos.xy), 0));
        if (behind > 0.0)
        {
            if (!(sceneW < last + tol)) discard;
            priority = 0.45 + 0.20 * behind / (behind + 1.0);   // after every surface match; the triangle nearest in front wins
        }
        else
        {
            // IN FRONT of this triangle, but only just: a part of the drawn mesh that sticks out of its collision hull (a
            // canopy rim, control handles, the rider's arms - the spots that stayed unmarked in the user's screenshot of
            // h2_taa_71). Whatever covers an object on screen from this close is the object or something riding on it;
            // an unrelated thing in front of it is further away than this.
            const float front = gDraw.w * (1.0 + 0.02 * max(viewW, 0.0));
            if (!(-behind < front)) discard;
            priority = 0.66 + 0.08 * (-behind / front);   // last among the triangle claims
        }
    }
    const Node N = gNodes[node];
    if (N.d0.w > 1e29)
    {
        // the node jumped (a teleport): this pixel is the object's, and it has no history
        o.motion = float2(25000.0, 25000.0);
        o.depth = priority;
        return o;
    }
    const float4 w4 = float4(w, 1.0);
    const float4 was = float4(dot(N.d0, w4), dot(N.d1, w4), dot(N.d2, w4), 1.0);
    const float4 pc = mul(was, gPrevVP);
    if (pc.w <= 1e-6) discard; // it was behind the camera
    const float2 prevPix = (pc.xy / pc.w * float2(0.5, -0.5) + 0.5) * gRect.zw + gRect.xy;
    o.motion = clamp(prevPix - unj, -16000.0, 16000.0);
    o.depth = priority;
    return o;
}

// ---------------------------------------------------------------- boxes ----------------------------------------------------------------

void ProxyVS(uint vid : SV_VertexID, uint iid : SV_InstanceID, out float4 pos : SV_Position, out nointerpolation uint proxy : PROXY)
{
    const float4 r = gProxies[iid].rect;
    pos = float4((vid & 1) ? r.z : r.x, (vid & 2) ? r.w : r.y, 0.5, 1.0);
    proxy = iid;
}

ProxyOut ProxyPS(float4 svpos : SV_Position, nointerpolation uint proxy : PROXY)
{
    ProxyOut o;
    o.motion = float2(30000.0, 30000.0);
    o.depth = 1.0;
    float3 w3;
    if (!SceneWorld(svpos.xy, svpos.xy, w3)) discard;   // sky: no object
    const float4 w = float4(w3, 1.0);
    const Proxy P = gProxies[proxy];
    const float3 l = float3(dot(P.w2l0, w), dot(P.w2l1, w), dot(P.w2l2, w)) - P.centre.xyz;
    const float3 a = abs(l) / P.half.xyz;
    const float m = max(a.x, max(a.y, a.z));
    if (!(m < 1.0)) discard;   // not in this box
    if (P.d0.w > 1e29)
    {
        // the object jumped (a teleport): this pixel is the object's, and it has no history
        o.motion = float2(25000.0, 25000.0);
        o.depth = 0.75 + 0.25 * m;
        return o;
    }
    const float4 was = float4(dot(P.d0, w), dot(P.d1, w), dot(P.d2, w), 1.0);
    const float4 pc = mul(was, gPrevVP);
    if (pc.w <= 1e-6) discard; // it was behind the camera
    const float2 prevPix = (pc.xy / pc.w * float2(0.5, -0.5) + 0.5) * gRect.zw + gRect.xy;
    o.motion = clamp(prevPix - svpos.xy, -16000.0, 16000.0);
    o.depth = 0.75 + 0.25 * m;   // after every triangle
    return o;
}
