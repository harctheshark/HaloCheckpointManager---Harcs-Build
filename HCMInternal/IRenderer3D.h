#pragma once
#include "pch.h"
#include "TriggerModel.h"
#include "SettingsEnums.h"
#include "directxtk\VertexTypes.h"
#include "directxtk\PrimitiveBatch.h"
#include "IModel.h"


enum class TextureEnum
{
	SplotchyPattern,
	Crosshair
};

enum class CullingOption
{
	CullNone,
	CullBack,
	CullFront,
};

// How a draw interacts with the overlay's own depth buffer. See drawTriangleCollection.
enum class DepthMode
{
	TestOnly,           // depth-tested, writes no depth. The default and what every MCC caller wants.
	DepthOnlyPrepass,   // writes depth, writes NO colour. First half of a two-pass translucent shell.
};

/// <summary>
/// Provides functions for rendering in 3D.
///
/// This interface is deliberately GRAPHICS-API-NEUTRAL: it contains only drawing and
/// projection/query calls, no D3D11 or D3D12 types. There are two independent implementations and
/// they share NOTHING:
///   * Renderer3DImpl.h   - D3D11 / DirectXTK, used by the six MCC games. Untouched.
///   * Renderer3DImplD3D12.h - D3D12, used only by Halo Campaign Evolved (HaloCER).
///
/// The per-frame camera update is NOT on this interface, because it is the one call that is
/// inherently API-specific (the D3D11 implementation needs a device/context/RTV, the D3D12 one needs
/// a device/command list/RTV handle/format). Each implementation exposes its own concrete
/// `updateCameraData` / `beginFrame`, and Render3DEventProvider - which is the only caller, and which
/// constructs the concrete type - calls it on the concrete type.
/// <see cref="Renderer3DImpl.h"/>
/// <see cref="Renderer3DImplD3D12.h"/>
/// </summary>
///
/// NOTE - NO VIRTUAL DESTRUCTOR, DELIBERATELY. This class never had one, and adding one now would
/// start running ~Renderer3DImpl (releasing DirectXTK/D3D11 state on the HCM shutdown thread) for the
/// six MCC games, on a path that has never executed. That is a behaviour change to shipped, working
/// code, so it is not made here. Instead Render3DEventProvider owns the D3D12 renderer through a
/// unique_ptr to its CONCRETE type, so that one is destroyed correctly.
class IRenderer3D {
public:

	/// <summary>
	/// Calculates the screen position of a world position.
	/// </summary>
	/// <param name="worldPointPosition">The 3D world position.</param>
	/// <param name="shouldFlipBehind">Whether points behind the camera should be flipped to be on the correct side.</param>
	/// <returns>Vector3 with screen space position in pixels in x (width, 0 is left), y (height, 0 is top), and z is clipspace depth.</returns>
	virtual SimpleMath::Vector3 worldPointToScreenPosition(SimpleMath::Vector3 worldPointPosition, bool shouldFlipBehind = true) = 0;

	/// <summary>
	/// Calculates the screen position of a world position, clamping points outside the view frustrum to the edge of the screen.
	/// </summary>
	/// <param name="worldPointPosition">The 3D world position.</param>
	/// <param name="screenEdgeOffset">How far in pixels from the screen to offset clamped positions (negative numbers outside the screen, 0 on edge, positive into the screen)</param>
	/// <param name="appliedClamp">Out - whether the initial world point was in the view frustrum (ie was it clamped?)</param>
	/// <returns>Vector3 with screen space position in pixels in x (width, 0 is left), y (height, 0 is top), and z is clipspace depth.</returns>
	virtual SimpleMath::Vector3 worldPointToScreenPositionClamped(SimpleMath::Vector3 worldPointPosition, int screenEdgeOffset = 0, bool* appliedClamp = nullptr) = 0;

	/// <summary>
	/// Calculates how far from the camera a world position is.
	/// </summary>
	/// <param name="worldPointPosition">The 3D world position.</param>
	/// <returns>The distance between the camera and world position.</returns>
	virtual float cameraDistanceToWorldPoint(SimpleMath::Vector3 worldPointPosition) = 0;

	/// <summary>
	/// Checks if a world position will be visible on screen (is inside the view frustrum)
	/// </summary>
	/// <param name="worldPointPosition">The 3D world position.</param>
	/// <returns>True if world position will be visible.</returns>
	virtual bool pointOnScreen(const SimpleMath::Vector3& worldPointPosition) = 0;



	/// <summary>
	/// Checks if a world position will in the 180 degree semisphere behind the camera
	/// </summary>
	/// <param name="worldPointPosition">The 3D world position.</param>
	/// <returns>True if world position will be behind the camera.</returns>
	virtual bool pointBehindCamera(const SimpleMath::Vector3& worldPointPosition) = 0;

	/// <summary>
	/// Draws a 2D sprite at the specified screen position (anchored top-left). 
	/// </summary>
	/// <param name="texture">Emum of the sprite to draw.</param>
	/// <param name="screenPosition">Screen position to draw the sprite (this will be the top-left corner of the sprite).</param>
	/// <param name="spriteScale">Scaling factor of sprite.</param>
	/// <param name="spriteColor">What colour should be overlaid on the sprite.</param>
	/// <returns></returns>
	virtual RECTF drawSprite(TextureEnum texture, SimpleMath::Vector2 screenPosition, float spriteScale = 1.f, SimpleMath::Vector4 spriteColor = {1.f, 0.5f, 0.f, 1.f}) = 0;

	/// <summary>
	/// Draws a 2D sprite at the specified screen position (center of sprite). 
	/// </summary>
	/// <param name="texture">Enum of the sprite to draw.</param>
	/// <param name="screenPosition">Screen position to draw the sprite (this will be the center of the sprite).</param>
	/// <param name="spriteScale">Scaling factor of sprite.</param>
	/// <param name="spriteColor">What colour should be overlaid on the sprite.</param>
	/// <returns></returns>
	virtual RECTF drawCenteredSprite(TextureEnum texture, SimpleMath::Vector2 screenPosition, float spriteScale = 1.f, SimpleMath::Vector4 spriteColor = { 1.f, 0.5f, 0.f, 1.f }) = 0;


	/// <summary>
	/// Draws a unit sphere (scaled by param) of the specified colour at the specified position.
	/// </summary>
	/// <param name="position">Float3 world position of the center of the sphere.</param>
	/// <param name="color">Float4 of the colour to be drawn.</param>
	/// <param name="scale">Float of scaling (unit scale) to apply to the sphere.</param>
	/// <param name="isWireframe">Whether should be drawn as wireframe. Otherwise, filled volume. </param>
	virtual void renderSphere(const SimpleMath::Vector3& position, const SimpleMath::Vector4& color, const float& scale, const bool& isWireframe) = 0;

	/// <summary>
	/// Gets the cameras current position.
	/// </summary>
	/// <returns>worldPoint position of camera</returns>
	virtual const SimpleMath::Vector3 getCameraPosition() = 0;

	/// <summary>
	/// Gets the cameras frustum (viewing area).
	/// </summary>
	/// <returns>Cameras frustum</returns>
	virtual const DirectX::BoundingFrustum& getCameraFrustum() = 0;


	/// <summary>
	/// Draws a filled triangle of the specified colour at the specified position.
	/// </summary>
	/// <param name="vertexPositions">3x Float3 world position of the triangle vertices.</param>
	/// <param name="color">Float4 of the colour to be drawn.</param>
	/// <param name="texture">Optional enum of the texture to be drawn.</param>
	virtual void drawTriangle(const std::array<SimpleMath::Vector3, 3>& vertexPositions, const SimpleMath::Vector4& color, CullingOption cullingOption = CullingOption::CullNone, std::optional<TextureEnum> texture = std::nullopt) = 0;


	/// <summary>
	/// Draws a collection of triangles of the specified colour and texture.
	/// </summary>
	/// <param name="model">Interface providing access to a VertexCollection and IndiceCollection of the triangle vertices (in sets of 3).</param>
	/// <param name="color">Float4 of the colour to be drawn.</param>
	/// <param name="texture">Optional enum of the texture to be drawn.</param>
	/// <param name="depthMode">How this collection interacts with the depth buffer. TestOnly is the default and
	/// is what every existing caller relies on - trigger volumes must stay visible through each other.
	///
	/// For a CLOSED SHELL (the HaloCER structure-BSP overlay) a single translucent pass is unreadable: every
	/// wall behind the one you are looking at blends in too, and the result is a uniform wash. The fix is a
	/// two-pass DEPTH PRE-PASS - draw once as DepthOnlyPrepass (writes depth, no colour), then again as
	/// TestOnly (blends, writes no depth). Only the nearest surface then survives the depth test, so exactly
	/// one translucent layer lands per pixel and the result does not depend on submission order.
	///
	/// TestAndWrite (blend AND write) is deliberately NOT offered: it is order-dependent and produces exactly
	/// the wash it appears to fix. Implemented on the D3D12 path; the D3D11 path ignores it.</param>
	virtual void drawTriangleCollection(const IModelTriangles* model, const SimpleMath::Vector4& color, CullingOption cullingOption = CullingOption::CullNone, std::optional<TextureEnum> texture = std::nullopt, DepthMode depthMode = DepthMode::TestOnly) = 0;

	/// <summary>
	/// Applies a WORLD-SPACE 3D checker to subsequent filled draws, until switched off again.
	///
	/// This exists because a flat translucent fill cannot answer "is there a surface here, or am I seeing
	/// through a hole to something behind it?" - both look like a wash of colour. A pattern locked to world
	/// space answers it unambiguously: patterned means a surface is present, unpatterned means it is not.
	/// World space rather than screen space on purpose - a screen-space hatch slides across geometry as the
	/// camera moves and conveys nothing about the surface.
	///
	/// Defaults to a NO-OP so every existing caller is unaffected; only the D3D12 path implements it.
	/// ⚠ It is sticky per-renderer state: whoever turns it on must turn it off, or unrelated overlays inherit it.
	/// </summary>
	/// <param name="worldCellSize">Checker cell size in world units. Zero or less disables the pattern.</param>
	/// <param name="contrast">0..1. How far each parity brightens/darkens from the chosen colour.</param>
	virtual void setSurfacePattern(float worldCellSize, float contrast) {}

	/// <summary>
	/// Draws an edge (line of the specified colour at the specified position.
	/// </summary>
	/// <param name="edgeStart">Start world position of the line.</param>
	/// <param name="edgeEnd">End world position of the line.</param>
	/// <param name="color">Float4 of the colour to be drawn.</param>
	virtual void drawEdge(const SimpleMath::Vector3& edgeStart, const SimpleMath::Vector3& edgeEnd, const SimpleMath::Vector4& color) = 0;


	/// <summary>
	/// Draws a collection of edges (line) of the specified colour.
	/// </summary>
	/// <param name="model">Interface providing access to a VertexCollection and IndiceCollection of the edge vertices (in sets of 2).</param>
	/// <param name="color">Float4 of the colour to be drawn.</param>
	virtual void drawEdgeCollection(const IModelEdges* model, const SimpleMath::Vector4& color) = 0;
};
