#include "pch.h"
#include "Renderer3DImplD3D12.h"
#include "IMakeOrGetCheat.h"
#include "directxtk\SimpleMath.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3d12")
#pragma comment(lib, "d3dcompiler.lib")

// See Renderer3DImplD3D12.h for the design, the hook contract and the coordinate-frame rules.

namespace
{
	// ---------------------------------------------------------------------------------------------------------
	// The whole shader. Position-only vertices, one root-constant block, flat colour.
	//
	// NO float4x4 AND NO mul() HERE, DELIBERATELY - THIS IS THE "RENDERS FROM 0,0,0" BUG.
	//
	// SimpleMath::Matrix is a DirectXMath XMFLOAT4X4: ROW-major in memory, row-VECTOR convention (v' = v * M),
	// with the translation in ROW 3. We upload it as a flat run of 16 floats. HLSL's default cbuffer matrix
	// packing is COLUMN-major, so a `float4x4` here is read as the TRANSPOSE unless the packing is overridden.
	// The `row_major` qualifier that used to be on this declaration did not survive into the compiled constant
	// layout, and the failure mode is vicious rather than obvious:
	//
	//     transposed => result_j = dot(v, VP.row(j)), so the camera POSITION - which lives only in VP.row(3) -
	//     contributes ONLY to w. Every vertex's x/y/z then depends on camera ROTATION alone. On screen that is
	//     an overlay anchored at the world origin, that does not shift when the player walks, but that still
	//     swings when you look around and still warps as w changes. Which is exactly what was reported.
	//
	// So the matrix is passed as four explicit float4 ROWS and the row-vector product is written out by hand.
	// A float4 in a constant buffer is always four consecutive floats - there is no layout convention left for a
	// compiler default to get wrong. Do not "tidy" this back into a float4x4 + mul().
	//
	//     v * VP  ==  v.x*row0 + v.y*row1 + v.z*row2 + 1*row3
	//
	// The CPU side is unchanged by this: memcpy of XMFLOAT4X4::m already produces row0..row3 in this order.
	// ---------------------------------------------------------------------------------------------------------
	constexpr char kShaderSource[] = R"(
cbuffer DrawConstants : register(b0)
{
    float4 worldViewProjectionRow0;
    float4 worldViewProjectionRow1;
    float4 worldViewProjectionRow2;
    float4 worldViewProjectionRow3;
    float4 drawColor;
};

struct VSInput
{
    float3 position : POSITION;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    // Row-vector product, written out so no matrix packing convention can invert it.
    output.position = input.position.x * worldViewProjectionRow0
                    + input.position.y * worldViewProjectionRow1
                    + input.position.z * worldViewProjectionRow2
                    + worldViewProjectionRow3;
    output.color = drawColor;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
)";

	constexpr UINT kRootConstantCount = 20;   // 16 floats of matrix + 4 floats of colour
	constexpr UINT kVertexStride = (UINT)sizeof(DirectX::VertexPosition);   // 12 bytes, position only

	// Depth format. D24_UNORM_S8_UINT is what Renderer3DImpl_updateCameraData.cpp uses on the D3D11 path;
	// keeping it identical keeps the depth behaviour identical.
	constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

	constexpr UINT64 kInitialVertexBytes = 64 * 1024;
	constexpr UINT64 kInitialIndexBytes = 32 * 1024;
	constexpr UINT64 kMaxUploadBytes = 32 * 1024 * 1024;   // refuse absurd geometry rather than exhaust the heap

	// Near/far planes, in HaloCER world units (1 world unit = 10 feet).
	//
	// NEAR is small (0.02 units == about 6 cm) because standing INSIDE a trigger volume is exactly when you most
	// want to see it - a near plane of any real size would slice the walls off right in front of the player.
	// FAR is past hceTriggerOverlayRenderDistance's maximum (2000) with room for a volume's own radius, so the
	// projection never clips something the user's own render-distance setting says should be visible.
	// The resulting ratio is only ever used to sort the overlay against itself, which it has ample precision for.
	constexpr float kNearPlane = 0.02f;
	constexpr float kFarPlane = 4000.f;

	// Player origin (feet) -> eye, in HaloCER world units: 1 unit = 10 feet, so ~6 feet is 0.6 units. Only used
	// by the degenerate-camera-entry fallback below, where being a foot out is irrelevant next to being frozen.
	constexpr float kFallbackEyeHeight = 0.6f;

	UINT64 alignUp(UINT64 value, UINT64 alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

	bool compileShader(const char* entryPoint, const char* target, std::vector<uint8_t>& out)
	{
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		// PACK_MATRIX_ROW_MAJOR is belt-and-braces: the shader no longer declares a matrix at all (see
		// kShaderSource), so nothing depends on it - but if one is ever reintroduced it will match the
		// XMFLOAT4X4 memory layout we upload rather than silently transposing.
		const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR;
		const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, nullptr, nullptr,
			entryPoint, target, flags, 0, &blob, &errors);

		if (FAILED(hr) || !blob)
		{
			std::string message = "unknown error";
			if (errors && errors->GetBufferPointer())
				message.assign((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
			PLOG_ERROR << "Renderer3DImplD3D12: failed to compile " << entryPoint << " (" << target << "): " << message;
			if (blob) blob->Release();
			if (errors) errors->Release();
			return false;
		}

		out.resize(blob->GetBufferSize());
		std::memcpy(out.data(), blob->GetBufferPointer(), blob->GetBufferSize());
		blob->Release();
		if (errors) errors->Release();
		return true;
	}

	// Copied from Renderer3DImpl_WorldToScreen.cpp - deliberately, so the two renderers cannot drift.
	// (Duplication between the D3D11 and D3D12 implementations is the explicit policy here.)
	void clampScreenPosition(SimpleMath::Vector3& screenPosition, const SimpleMath::Vector2& screenCenter)
	{
		screenPosition.x -= screenCenter.x;
		screenPosition.y -= screenCenter.y;

		const float screenPosAngle = std::atan2f(screenPosition.y, screenPosition.x);
		const float screenPosSlope = std::tanf(screenPosAngle);

		if (screenPosition.x > 0)
			screenPosition = SimpleMath::Vector3(screenCenter.x, screenCenter.x * screenPosSlope, screenPosition.z);
		else
			screenPosition = SimpleMath::Vector3(-screenCenter.x, -screenCenter.x * screenPosSlope, screenPosition.z);

		if (screenPosition.y > screenCenter.y)
			screenPosition = SimpleMath::Vector3(screenCenter.y / screenPosSlope, screenCenter.y, screenPosition.z);
		else if (screenPosition.y < -screenCenter.y)
			screenPosition = SimpleMath::Vector3(-screenCenter.y / screenPosSlope, -screenCenter.y, screenPosition.z);

		screenPosition.x += screenCenter.x;
		screenPosition.y += screenCenter.y;
	}
}


Renderer3DImplD3D12::Renderer3DImplD3D12(GameState game, IDIContainer& dicon)
	: mSettingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
	mMccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
	mMessagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
	mRuntimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
	mCameraDataWeak(resolveDependentCheat(HCEGetCameraData))
{
	if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
		throw HCMInitException("Renderer3DImplD3D12 only supports Halo Campaign Evolved");

	// Fallback camera only: used for the frames before APlayerCameraManager::DoUpdateCamera has run even once.
	// Optional - without it the renderer just skips those frames.
	try
	{
		mPlayerStateOptionalWeak = resolveDependentCheat(HCEGetPlayerState);
	}
	catch (HCMInitException)
	{
		PLOG_ERROR << "Renderer3DImplD3D12 could not resolve HCEGetPlayerState; no fallback camera";
	}

	// Unit sphere, radius 0.5 so its diameter is 1 - exactly GeometricPrimitive::CreateSphere's default on the
	// MCC path, which is what renderSphere's `scale` parameter is calibrated against.
	{
		constexpr int stacks = 16;
		constexpr int slices = 24;
		mSphereVertices.reserve((stacks + 1) * (slices + 1));
		for (int i = 0; i <= stacks; ++i)
		{
			const float phi = (float)i / (float)stacks * DirectX::XM_PI;
			const float sinPhi = std::sin(phi), cosPhi = std::cos(phi);
			for (int j = 0; j <= slices; ++j)
			{
				const float theta = (float)j / (float)slices * DirectX::XM_2PI;
				mSphereVertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(
					0.5f * sinPhi * std::cos(theta),
					0.5f * sinPhi * std::sin(theta),
					0.5f * cosPhi)));
			}
		}
		mSphereIndices.reserve(stacks * slices * 6);
		for (int i = 0; i < stacks; ++i)
		{
			for (int j = 0; j < slices; ++j)
			{
				const uint16_t a = (uint16_t)(i * (slices + 1) + j);
				const uint16_t b = (uint16_t)(a + 1);
				const uint16_t c = (uint16_t)(a + slices + 1);
				const uint16_t d = (uint16_t)(c + 1);
				mSphereIndices.push_back(a); mSphereIndices.push_back(c); mSphereIndices.push_back(b);
				mSphereIndices.push_back(b); mSphereIndices.push_back(c); mSphereIndices.push_back(d);
			}
		}
	}
}

Renderer3DImplD3D12::~Renderer3DImplD3D12()
{
	PLOG_DEBUG << "~Renderer3DImplD3D12";

	// ABANDON RATHER THAN FREE, once we have ever recorded a frame.
	//
	// We do not own a fence and we never Signal one (D3D12Hook owns the queue and all submission), so from here
	// there is no way to PROVE the GPU has finished with the buffers our last draws referenced. D3D12 does not
	// defer destruction on GPU usage; freeing a resource an in-flight command list still reads is a GPU-side
	// use-after-free -> device removed -> the GAME dies. App.h destroys the optional cheats BEFORE ~D3D12Hook
	// runs its flush, so the flush cannot save us either.
	//
	// This is the same policy ~ImGuiManager applies when it cannot prove the GPU is idle: leak. HCM is shutting
	// down entirely at this point, so a few megabytes in a process that is about to lose HCM is free; a GPU
	// fault is not. Detach() drops our reference-counted ownership without Releasing.
	const bool everRendered = (mFrameOrdinal != 0);
	if (everRendered)
	{
		PLOG_INFO << "Renderer3DImplD3D12: abandoning (leaking) its D3D12 resources rather than freeing memory "
			"the GPU may still be reading - HCM is shutting down";

		for (auto& slot : mUploadRing)
		{
			// Deliberately NOT Unmap'd: unmapping is itself a runtime operation on a resource we are abandoning.
			slot.vertices.cpu = nullptr;
			slot.indices.cpu = nullptr;
			if (slot.vertices.resource) slot.vertices.resource.Detach();
			if (slot.indices.resource) slot.indices.resource.Detach();
		}
		for (auto& retired : mRetiredResources)
			if (retired.object) retired.object.Detach();
		if (mDepthStencil) mDepthStencil.Detach();
		if (mDsvHeap) mDsvHeap.Detach();
		for (auto& pipelineState : mPipelineStates)
			if (pipelineState) pipelineState.Detach();
		if (mRootSignature) mRootSignature.Detach();
	}
	else
	{
		// Nothing was ever submitted, so nothing can be in flight; release normally.
		for (auto& slot : mUploadRing)
		{
			if (slot.vertices.resource && slot.vertices.cpu) slot.vertices.resource->Unmap(0, nullptr);
			if (slot.indices.resource && slot.indices.cpu) slot.indices.resource->Unmap(0, nullptr);
			slot.vertices.cpu = nullptr;
			slot.indices.cpu = nullptr;
		}
	}

	mRetiredResources.clear();
	mCommandList = nullptr;
	mDevice = nullptr;
}


// ================================================================================================================
// Initialisation
// ================================================================================================================

void Renderer3DImplD3D12::fail(const char* what)
{
	if (!mInitFailed)
	{
		mInitFailed = true;
		PLOG_ERROR << "Renderer3DImplD3D12 disabled: " << what
			<< ". The HaloCER overlay falls back to ImGui drawing.";
	}
}

bool Renderer3DImplD3D12::createRootSignatureAndShaders()
{
	if (!compileShader("VSMain", "vs_5_0", mVertexShaderBytecode)) return false;
	if (!compileShader("PSMain", "ps_5_0", mPixelShaderBytecode)) return false;

	// One root parameter: 20 32-bit constants at b0, visible to the vertex shader only (the pixel shader gets
	// the colour through the interpolator). Root constants rather than an actual constant BUFFER on purpose -
	// they need no allocation, no ring, no alignment and no aliasing hazard against in-flight frames, and 20
	// DWORDs is nothing against the 64-DWORD root signature budget.
	D3D12_ROOT_PARAMETER rootParameter{};
	rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	rootParameter.Constants.ShaderRegister = 0;
	rootParameter.Constants.RegisterSpace = 0;
	rootParameter.Constants.Num32BitValues = kRootConstantCount;

	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = 1;
	desc.pParameters = &rootParameter;
	desc.NumStaticSamplers = 0;
	desc.pStaticSamplers = nullptr;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

	ID3DBlob* serialized = nullptr;
	ID3DBlob* errors = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if (FAILED(hr) || !serialized)
	{
		std::string message = "unknown error";
		if (errors && errors->GetBufferPointer())
			message.assign((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
		PLOG_ERROR << "Renderer3DImplD3D12: D3D12SerializeRootSignature failed (" << std::hex << (ULONG)hr
			<< "): " << message;
		if (serialized) serialized->Release();
		if (errors) errors->Release();
		return false;
	}

	hr = mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(mRootSignature.ReleaseAndGetAddressOf()));
	serialized->Release();
	if (errors) errors->Release();

	if (FAILED(hr))
	{
		PLOG_ERROR << "Renderer3DImplD3D12: CreateRootSignature failed: " << std::hex << (ULONG)hr;
		return false;
	}
	return true;
}

bool Renderer3DImplD3D12::createPipelineStates(DXGI_FORMAT backBufferFormat)
{
	if (backBufferFormat == DXGI_FORMAT_UNKNOWN) return false;

	static const D3D12_INPUT_ELEMENT_DESC inputElements[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	struct Variant
	{
		Pso pso;
		D3D12_FILL_MODE fill;
		D3D12_CULL_MODE cull;
		D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
		bool alphaBlended;   // false => blend off + depth WRITE (DirectXTK's "Opaque + DepthDefault")
	};

	static const Variant variants[] =
	{
		{ Pso::TriangleCullNoneAlpha,        D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_NONE,  D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true  },
		{ Pso::TriangleCullFrontFacesAlpha,  D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_FRONT, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true  },
		{ Pso::TriangleCullBackFacesAlpha,   D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_BACK,  D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true  },
		{ Pso::TriangleCullNoneOpaque,       D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_NONE,  D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, false },
		{ Pso::TriangleCullFrontFacesOpaque, D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_FRONT, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, false },
		{ Pso::TriangleCullBackFacesOpaque,  D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_BACK,  D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, false },
		{ Pso::WireframeAlpha,               D3D12_FILL_MODE_WIREFRAME, D3D12_CULL_MODE_NONE,  D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true  },
		{ Pso::WireframeOpaque,              D3D12_FILL_MODE_WIREFRAME, D3D12_CULL_MODE_NONE,  D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, false },
		{ Pso::LineAlpha,                    D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_NONE,  D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,     true  },
		{ Pso::LineOpaque,                   D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_NONE,  D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,     false },
	};

	for (const Variant& variant : variants)
	{
		// Rebuilding after a back buffer format change: the PSO being replaced may still be referenced by a
		// command list in flight, and D3D12 keeps no reference of its own. Retire it instead of releasing it.
		if (auto& existing = mPipelineStates[(size_t)variant.pso])
		{
			mRetiredResources.push_back({ existing, mFrameOrdinal });
			existing.Reset();
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = mRootSignature.Get();
		desc.VS = { mVertexShaderBytecode.data(), mVertexShaderBytecode.size() };
		desc.PS = { mPixelShaderBytecode.data(), mPixelShaderBytecode.size() };
		desc.InputLayout = { inputElements, _countof(inputElements) };
		desc.PrimitiveTopologyType = variant.topology;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = backBufferFormat;
		desc.DSVFormat = kDepthFormat;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.SampleMask = UINT_MAX;
		desc.NodeMask = 0;

		desc.RasterizerState.FillMode = variant.fill;
		desc.RasterizerState.CullMode = variant.cull;
		// FALSE, matching D3D's default and DirectXTK's - which is the entire reason CullClockwise maps to
		// CULL_FRONT (see the Pso enum comment).
		desc.RasterizerState.FrontCounterClockwise = FALSE;
		desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.RasterizerState.MultisampleEnable = FALSE;
		desc.RasterizerState.AntialiasedLineEnable = FALSE;
		desc.RasterizerState.ForcedSampleCount = 0;
		desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		desc.BlendState.AlphaToCoverageEnable = FALSE;
		desc.BlendState.IndependentBlendEnable = FALSE;
		D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
		blend.BlendEnable = variant.alphaBlended ? TRUE : FALSE;
		blend.LogicOpEnable = FALSE;
		blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		blend.BlendOp = D3D12_BLEND_OP_ADD;
		blend.SrcBlendAlpha = D3D12_BLEND_ONE;
		blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		blend.LogicOp = D3D12_LOGIC_OP_NOOP;
		blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		// Depth: ALWAYS tested, written only by the opaque variants. Same rule DirectXTK applies on the MCC
		// path (AlphaBlend + DepthRead vs Opaque + DepthDefault). The buffer is OURS and is cleared to 1.0 at
		// the top of every frame, so this sorts the overlay against ITSELF and never against world geometry.
		desc.DepthStencilState.DepthEnable = TRUE;
		desc.DepthStencilState.DepthWriteMask = variant.alphaBlended ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.DepthStencilState.StencilEnable = FALSE;

		const HRESULT hr = mDevice->CreateGraphicsPipelineState(&desc,
			IID_PPV_ARGS(mPipelineStates[(size_t)variant.pso].ReleaseAndGetAddressOf()));
		if (FAILED(hr))
		{
			PLOG_ERROR << "Renderer3DImplD3D12: CreateGraphicsPipelineState failed for variant "
				<< (size_t)variant.pso << ": " << std::hex << (ULONG)hr;
			return false;
		}
	}
	return true;
}

bool Renderer3DImplD3D12::createDepthStencil(UINT width, UINT height)
{
	if (width == 0 || height == 0) return false;

	if (!mDsvHeap)
	{
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		heapDesc.NumDescriptors = 1;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (FAILED(mDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(mDsvHeap.ReleaseAndGetAddressOf()))))
		{
			PLOG_ERROR << "Renderer3DImplD3D12: could not create the DSV descriptor heap";
			return false;
		}
		mDsvHandle = mDsvHeap->GetCPUDescriptorHandleForHeapStart();
	}

	// The old texture may still be referenced by a command list the GPU has not finished. Retire it rather than
	// dropping the last reference here (see collectRetiredResources).
	if (mDepthStencil)
	{
		mRetiredResources.push_back({ mDepthStencil, mFrameOrdinal });
		mDepthStencil.Reset();
	}

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProperties.CreationNodeMask = 1;
	heapProperties.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resourceDesc.Alignment = 0;
	resourceDesc.Width = width;
	resourceDesc.Height = height;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.Format = kDepthFormat;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.SampleDesc.Quality = 0;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clearValue{};
	clearValue.Format = kDepthFormat;
	clearValue.DepthStencil.Depth = 1.f;
	clearValue.DepthStencil.Stencil = 0;

	const HRESULT hr = mDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(mDepthStencil.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
	{
		PLOG_ERROR << "Renderer3DImplD3D12: could not create the depth stencil texture (" << width << "x"
			<< height << "): " << std::hex << (ULONG)hr;
		return false;
	}

	D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc{};
	viewDesc.Format = kDepthFormat;
	viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	viewDesc.Flags = D3D12_DSV_FLAG_NONE;
	viewDesc.Texture2D.MipSlice = 0;
	mDevice->CreateDepthStencilView(mDepthStencil.Get(), &viewDesc, mDsvHandle);

	mDepthWidth = width;
	mDepthHeight = height;
	return true;
}

bool Renderer3DImplD3D12::initialise(ID3D12Device* pDevice, DXGI_FORMAT backBufferFormat)
{
	mDevice = pDevice;
	mBackBufferFormat = backBufferFormat;

	if (!createRootSignatureAndShaders()) { fail("the root signature or shaders could not be created"); return false; }
	if (!createPipelineStates(backBufferFormat)) { fail("the pipeline states could not be created"); return false; }

	mInitialised = true;
	PLOG_INFO << "Renderer3DImplD3D12 initialised (back buffer format " << (int)backBufferFormat << ")";
	return true;
}


// ================================================================================================================
// Upload ring
// ================================================================================================================

bool Renderer3DImplD3D12::ensureUploadCapacity(UploadBuffer& buffer, UINT64 bytesNeeded, UINT64 initialCapacity)
{
	if (bytesNeeded == 0) return true;
	if (buffer.resource && buffer.used + bytesNeeded <= buffer.capacity) return true;

	UINT64 newCapacity = buffer.capacity ? buffer.capacity * 2 : initialCapacity;
	while (newCapacity < bytesNeeded) newCapacity *= 2;
	if (newCapacity > kMaxUploadBytes)
	{
		LOG_ONCE(PLOG_ERROR << "Renderer3DImplD3D12: refusing an absurd geometry upload; the draw is dropped");
		return false;
	}

	// Retire the old one. It may still be referenced by an in-flight command list; collectRetiredResources
	// frees it once the frame ordinal has moved a whole ring past this point.
	if (buffer.resource)
	{
		if (buffer.cpu) buffer.resource->Unmap(0, nullptr);
		mRetiredResources.push_back({ buffer.resource, mFrameOrdinal });
		buffer.resource.Reset();
		buffer.cpu = nullptr;
	}

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
	heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProperties.CreationNodeMask = 1;
	heapProperties.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Alignment = 0;
	resourceDesc.Width = newCapacity;
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.SampleDesc.Quality = 0;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	if (FAILED(mDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(buffer.resource.ReleaseAndGetAddressOf()))))
	{
		LOG_ONCE(PLOG_ERROR << "Renderer3DImplD3D12: upload buffer allocation failed; the draw is dropped");
		buffer.resource.Reset();
		buffer.capacity = 0;
		buffer.used = 0;
		return false;
	}

	// Persistently mapped. Upload heaps are explicitly designed for this and the CPU never reads back, so the
	// read range is empty.
	D3D12_RANGE emptyRange{ 0, 0 };
	void* mapped = nullptr;
	if (FAILED(buffer.resource->Map(0, &emptyRange, &mapped)) || !mapped)
	{
		LOG_ONCE(PLOG_ERROR << "Renderer3DImplD3D12: upload buffer map failed; the draw is dropped");
		buffer.resource.Reset();
		buffer.capacity = 0;
		buffer.used = 0;
		return false;
	}

	buffer.cpu = (uint8_t*)mapped;
	buffer.capacity = newCapacity;
	// The already-recorded draws this frame reference the RETIRED buffer through their own views, so restarting
	// at 0 in the new one cannot corrupt them.
	buffer.used = 0;
	return true;
}

void Renderer3DImplD3D12::collectRetiredResources()
{
	if (mRetiredResources.empty()) return;
	for (size_t i = mRetiredResources.size(); i-- > 0; )
	{
		// One whole ring past the frame it was retired on. By then D3D12Hook has fenced at least
		// kUploadRingSize >= framesInFlight submissions, so nothing in flight can still reference it.
		if (mFrameOrdinal - mRetiredResources[i].retiredAtOrdinal > (uint64_t)kUploadRingSize)
			mRetiredResources.erase(mRetiredResources.begin() + (ptrdiff_t)i);
	}
}

bool Renderer3DImplD3D12::appendVertices(const DirectX::VertexPosition* vertices, size_t count, D3D12_VERTEX_BUFFER_VIEW& outView)
{
	if (!vertices || count == 0) return false;
	UploadBuffer& buffer = mUploadRing[mSlotIndex].vertices;

	const UINT64 bytes = (UINT64)count * kVertexStride;
	// D3D12 wants a vertex buffer view's BufferLocation to be at least 4-byte aligned; 16 is free and keeps the
	// float3s naturally aligned for the memcpy too.
	buffer.used = alignUp(buffer.used, 16);
	if (!ensureUploadCapacity(buffer, bytes, kInitialVertexBytes)) return false;
	buffer.used = alignUp(buffer.used, 16);
	if (!buffer.cpu || !buffer.resource || buffer.used + bytes > buffer.capacity) return false;

	std::memcpy(buffer.cpu + buffer.used, vertices, (size_t)bytes);

	outView.BufferLocation = buffer.resource->GetGPUVirtualAddress() + buffer.used;
	outView.SizeInBytes = (UINT)bytes;
	outView.StrideInBytes = kVertexStride;
	buffer.used += bytes;
	return true;
}

bool Renderer3DImplD3D12::appendIndices(const uint16_t* indices, size_t count, D3D12_INDEX_BUFFER_VIEW& outView)
{
	if (!indices || count == 0) return false;
	UploadBuffer& buffer = mUploadRing[mSlotIndex].indices;

	const UINT64 bytes = (UINT64)count * sizeof(uint16_t);
	buffer.used = alignUp(buffer.used, 16);
	if (!ensureUploadCapacity(buffer, bytes, kInitialIndexBytes)) return false;
	buffer.used = alignUp(buffer.used, 16);
	if (!buffer.cpu || !buffer.resource || buffer.used + bytes > buffer.capacity) return false;

	std::memcpy(buffer.cpu + buffer.used, indices, (size_t)bytes);

	outView.BufferLocation = buffer.resource->GetGPUVirtualAddress() + buffer.used;
	outView.SizeInBytes = (UINT)bytes;
	outView.Format = DXGI_FORMAT_R16_UINT;
	buffer.used += bytes;
	return true;
}


// ================================================================================================================
// Frame
// ================================================================================================================

// Latched: logs only when the resolved source CHANGES, so this is silent after the first frame of any given
// steady state. Never per-frame.
void Renderer3DImplD3D12::logCameraSourceIfChanged(CameraSource source, const SimpleMath::Vector3& position, float horizontalFovDegrees)
{
	if (source == mLoggedCameraSource) return;

	// ⚠ "Log on change" alone is NOT rate limiting. At a menu the camera legitimately sits at the world origin,
	// so the resolved source ALTERNATES between UePov and OriginRejected every single frame - and a change-latch
	// fires on every flip. That produced 1237 lines in ~40 seconds and buried the diagnostics these logs exist
	// for. Rate-limit as well: a genuine state change still reports promptly, but a flapping one reports once
	// and then stays quiet until it settles.
	const uint32_t now = GetTickCount();
	if (mLastCameraSourceLogTick != 0 && (now - mLastCameraSourceLogTick) < kCameraSourceLogMinIntervalMs)
	{
		mLoggedCameraSource = source;   // still track it, just do not narrate every flip
		return;
	}
	mLastCameraSourceLogTick = now;
	mLoggedCameraSource = source;

	switch (source)
	{
	case CameraSource::UePov:
		PLOG_INFO << "Renderer3DImplD3D12 camera source: UE POV (APlayerCameraManager). Blam position ("
			<< position.x << ", " << position.y << ", " << position.z << "), horizontal FOV " << horizontalFovDegrees;
		break;
	case CameraSource::SimFallback:
		PLOG_WARNING << "Renderer3DImplD3D12 camera source: SIM PLAYER CAMERA FALLBACK - the UE POV was not "
			"readable, so the overlay tracks the player's aim rather than the render camera and will drift in a "
			"vehicle, a cutscene or Freecam. The HCEGetCameraData warning above says WHY the POV was rejected "
			"(in particular whether the midhook callback has fired at all). Blam position ("
			<< position.x << ", " << position.y << ", " << position.z << "), horizontal FOV " << horizontalFovDegrees;
		break;
	case CameraSource::OriginRejected:
		// The world origin is never a legitimate camera position in a loaded level, and rendering from it is
		// EXACTLY the reported bug ("its rendering from 0,0,0"). Refusing the frame is strictly better than
		// drawing a confidently wrong overlay - and it hands the feature back to the ImGui fallback.
		PLOG_WARNING << "Renderer3DImplD3D12: the resolved camera position was exactly the world origin, which a "
			"loaded level never produces. Treating the camera as NOT READY and skipping frames rather than "
			"rendering the overlay from (0,0,0).";
		break;
	case CameraSource::NoCameraService:
		PLOG_ERROR << "Renderer3DImplD3D12 has no HCEGetCameraData service; it cannot resolve a camera and will "
			"render nothing (the overlay falls back to ImGui drawing)";
		break;
	case CameraSource::NoFallbackAvailable:
		PLOG_ERROR << "Renderer3DImplD3D12 could not read the UE POV and has no HCEGetPlayerState fallback; "
			"rendering nothing (the overlay falls back to ImGui drawing)";
		break;
	case CameraSource::FallbackThrew:
		PLOG_DEBUG << "Renderer3DImplD3D12 camera unavailable (no level loaded / player dead / mid-load); "
			"skipping frames until it comes back";
		break;
	case CameraSource::RejectedValues:
		PLOG_ERROR << "Renderer3DImplD3D12 rejected the camera it resolved as implausible - position ("
			<< position.x << ", " << position.y << ", " << position.z << "), horizontal FOV "
			<< horizontalFovDegrees << ". Rendering nothing.";
		break;
	default:
		break;
	}
}

bool Renderer3DImplD3D12::updateCamera(SimpleMath::Vector2 screenSize)
{
	SimpleMath::Vector3 position, forward, right, up;
	float horizontalFovDegrees = 0.f;

	auto cameraData = mCameraDataWeak.lock();
	if (!cameraData)
	{
		logCameraSourceIfChanged(CameraSource::NoCameraService, {}, 0.f);
		return false;
	}

	HCEGetCameraData::UeCamera ueCamera;
	if (cameraData->getUeCamera(ueCamera))
	{
		position = ueCamera.positionBlam;
		HCEGetCameraData::ueRotationToBlamBasis(ueCamera.pitchDegrees, ueCamera.yawDegrees, ueCamera.rollDegrees,
			forward, right, up);
		horizontalFovDegrees = ueCamera.horizontalFovDegrees;
		logCameraSourceIfChanged(CameraSource::UePov, position, horizontalFovDegrees);
	}
	else if (mPlayerStateOptionalWeak.has_value())
	{
		// Fallback: the sim's PLAYER camera. Tracks the player rather than the render camera, so it drifts in a
		// vehicle or a cutscene - it only exists for the frames before DoUpdateCamera has run.
		// NOTE this is NOT a unit conversion path: getCameraView already returns BLAM world units. The /304.8
		// and the Y negation belong exclusively to the UE POV read; applying them here would double-convert.
		auto playerState = mPlayerStateOptionalWeak.value().lock();
		if (!playerState)
		{
			logCameraSourceIfChanged(CameraSource::NoFallbackAvailable, {}, 0.f);
			return false;
		}
		try
		{
			SimpleMath::Vector2 viewAngle;
			playerState->getCameraView(position, viewAngle);   // ONE tls walk for both

			// THE CAMERA-ENTRY POSITION DOES NOT TRACK THE PLAYER ON THIS BUILD.
			// Observed in game: it reads back (0, 0, 22.5454) - X and Y EXACTLY zero - and stays there while the
			// player walks around, which is precisely the reported symptom ("the volume doesnt get closer to me").
			// The VIEW ANGLE from the same call is fine (rotation responds correctly), so only the position half
			// is bad. This path was never actually exercised before, because until the UE POV hook broke, the
			// fallback simply never ran - so "unchanged since the working commit" does not mean "known good".
			//
			// getPlayerPositionAndVelocity IS confirmed good (the trigger vertex sphere and the in-game-verified
			// Teleport/Launch cheats all use it), so fall through to it and lift by an eye height. A camera at
			// the player's eye with the player's aim is exactly what this fallback is meant to approximate.
			if (position.x == 0.f && position.y == 0.f)
			{
				SimpleMath::Vector3 playerPosition, unusedVelocity;
				playerState->getPlayerPositionAndVelocity(playerPosition, unusedVelocity);
				if (playerPosition.x != 0.f || playerPosition.y != 0.f)
				{
					position = playerPosition;
					position.z += kFallbackEyeHeight;
					if (!mLoggedFallbackEyeSubstitution)
					{
						mLoggedFallbackEyeSubstitution = true;
						PLOG_WARNING << "Renderer3DImplD3D12: the sim camera-entry position read back degenerate "
							"(x and y exactly zero), so the fallback camera is using the PLAYER position plus an "
							"eye height instead. This tracks movement correctly; it does not follow a "
							"vehicle/cinematic camera.";
					}
				}
			}

			HCEGetCameraData::simViewAngleToBlamBasis(viewAngle, forward, right, up);
			horizontalFovDegrees = cameraData->getLastGoodHorizontalFov();
			logCameraSourceIfChanged(CameraSource::SimFallback, position, horizontalFovDegrees);
		}
		catch (HCMRuntimeException)
		{
			logCameraSourceIfChanged(CameraSource::FallbackThrew, {}, 0.f);
			return false;   // no level loaded / player dead / mid-load. Normal; skip the frame.
		}
	}
	else
	{
		logCameraSourceIfChanged(CameraSource::NoFallbackAvailable, {}, 0.f);
		return false;
	}

	if (screenSize.x < 1.f || screenSize.y < 1.f) return false;
	if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)
		|| forward.LengthSquared() < 1e-9f || up.LengthSquared() < 1e-9f)
	{
		logCameraSourceIfChanged(CameraSource::RejectedValues, position, horizontalFovDegrees);
		return false;
	}

	// EXACTLY the world origin means the source handed us a zeroed/unresolved position, not a real camera - no
	// loaded Halo level puts the camera there. Drawing from it is the reported bug, so refuse the frame and let
	// the ImGui fallback carry the overlay instead.
	if (position == SimpleMath::Vector3::Zero)
	{
		logCameraSourceIfChanged(CameraSource::OriginRejected, position, horizontalFovDegrees);
		return false;
	}

	// THE UE CAMERA REPORTS A HORIZONTAL FOV. XMMatrixPerspectiveFovRH wants a VERTICAL one.
	const float horizontalFov = DirectX::XMConvertToRadians(std::clamp(horizontalFovDegrees, 10.f, 170.f));
	const float aspectRatio = screenSize.x / screenSize.y;   // width / height
	const float verticalFov = 2.f * std::atan(std::tan(horizontalFov * 0.5f) / aspectRatio);
	if (!std::isfinite(verticalFov) || verticalFov <= 0.0001f) return false;

	mCameraPosition = position;
	mCameraForward = forward;
	mCameraUp = up;

	// RIGHT-HANDED, because the Blam frame is right-handed. XMMatrixLookAtRH builds xaxis = cross(up, eye-at)
	// = forward x up, which at yaw 0 in Blam is (0,-1,0) - Blam's RIGHT, matching the basis above. Swapping in
	// an LH builder mirrors the whole overlay.
	mProjectionMatrix = SimpleMath::Matrix::CreatePerspectiveFieldOfView(verticalFov, aspectRatio, kNearPlane, kFarPlane);
	mViewMatrix = SimpleMath::Matrix::CreateLookAt(mCameraPosition, mCameraPosition + mCameraForward, mCameraUp);
	mViewProjectionMatrix = mViewMatrix * mProjectionMatrix;

	mScreenSize = screenSize;
	mScreenCenter = { screenSize.x / 2.f, screenSize.y / 2.f };

	DirectX::BoundingFrustum::CreateFromMatrix(mFrustumViewWorld, mProjectionMatrix, true); // view space
	mFrustumViewWorld.Transform(mFrustumViewWorld, mViewMatrix.Invert());                   // -> world space
	return true;
}

bool Renderer3DImplD3D12::beginFrame(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
	SimpleMath::Vector2 screenSize, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV, DXGI_FORMAT backBufferFormat)
{
	// Nothing in here may throw into the game's render thread.
	try
	{
		if (mInitFailed) return false;
		if (!pDevice || !pCommandList) return false;
		if (backBufferFormat == DXGI_FORMAT_UNKNOWN) return false;
		if (screenSize.x < 1.f || screenSize.y < 1.f) return false;

		if (!mInitialised)
		{
			if (!initialise(pDevice, backBufferFormat)) return false;
		}
		else if (pDevice != mDevice)
		{
			// The game recreated its device under us. Everything we own belongs to the old one and cannot be
			// rebuilt safely from inside a command list recorded on the new one.
			fail("the game's D3D12 device changed");
			return false;
		}
		else if (backBufferFormat != mBackBufferFormat)
		{
			// A format change (HDR toggle, for instance) invalidates every PSO. Rebuild them; the old ones are
			// held by ComPtr until CreateGraphicsPipelineState replaces them, and PSOs are not resources the
			// GPU reads asynchronously in a way ExecuteCommandLists has not already captured.
			PLOG_INFO << "Renderer3DImplD3D12: back buffer format changed (" << (int)mBackBufferFormat << " -> "
				<< (int)backBufferFormat << "); rebuilding pipeline states";
			if (!createPipelineStates(backBufferFormat)) { fail("the pipeline states could not be rebuilt"); return false; }
			mBackBufferFormat = backBufferFormat;
		}

		// Advance the ring FIRST, so retirement bookkeeping and this frame's slot agree.
		++mFrameOrdinal;
		mSlotIndex = (size_t)(mFrameOrdinal % kUploadRingSize);
		mUploadRing[mSlotIndex].vertices.used = 0;
		mUploadRing[mSlotIndex].indices.used = 0;
		collectRetiredResources();

		const UINT width = (UINT)screenSize.x;
		const UINT height = (UINT)screenSize.y;
		if (!mDepthStencil || width != mDepthWidth || height != mDepthHeight)
		{
			if (!createDepthStencil(width, height))
			{
				// Not latched as a permanent failure: a transient allocation failure during a resolution change
				// should not cost the overlay for the rest of the session.
				LOG_ONCE(PLOG_ERROR << "Renderer3DImplD3D12: depth stencil creation failed; skipping the frame");
				return false;
			}
		}

		if (!updateCamera(screenSize)) return false;

		// ---- record this frame's setup ------------------------------------------------------------------
		// D3D12Hook has already reset this list, transitioned the back buffer to RENDER_TARGET, bound the RTV
		// and bound its SRV heap. It does NOT set a viewport or scissor (imgui sets its own), so we set ours.
		D3D12_VIEWPORT viewport{};
		viewport.TopLeftX = 0.f;
		viewport.TopLeftY = 0.f;
		viewport.Width = screenSize.x;
		viewport.Height = screenSize.y;
		viewport.MinDepth = 0.f;
		viewport.MaxDepth = 1.f;
		pCommandList->RSSetViewports(1, &viewport);

		D3D12_RECT scissor{ 0, 0, (LONG)width, (LONG)height };
		pCommandList->RSSetScissorRects(1, &scissor);

		// Bind OUR depth buffer alongside the game's back buffer, then clear it. This is the D3D12 equivalent
		// of Renderer3DImpl_updateCameraData.cpp's OMSetRenderTargets + ClearDepthStencilView, and it is what
		// makes the overlay depth-sort against ITSELF and nothing else. endFrame() unbinds it again.
		pCommandList->OMSetRenderTargets(1, &backBufferRTV, FALSE, &mDsvHandle);
		pCommandList->ClearDepthStencilView(mDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);

		pCommandList->SetGraphicsRootSignature(mRootSignature.Get());

		mCommandList = pCommandList;
		mBackBufferRtv = backBufferRTV;
		mCurrentPso = Pso::Count;   // force the first draw to set one
		return true;
	}
	catch (...)
	{
		fail("an unexpected exception was thrown while beginning a frame");
		mCommandList = nullptr;
		return false;
	}
}

void Renderer3DImplD3D12::endFrame()
{
	if (!mCommandList) return;
	try
	{
		// Re-bind the render target WITHOUT a depth buffer. ImGui_ImplDX12_RenderDrawData runs immediately after
		// us on this same list with a PSO whose DSVFormat is UNKNOWN; leaving a DSV bound would be a
		// PSO/render-target mismatch. imgui re-establishes viewport, scissor, root signature, PSO, topology and
		// vertex/index buffers itself, so those need no restoring - only this one does.
		mCommandList->OMSetRenderTargets(1, &mBackBufferRtv, FALSE, nullptr);
	}
	catch (...)
	{
		// Cannot happen (it is a vtable call into the runtime) but this must never unwind into DXGI.
	}
	mCommandList = nullptr;
	mCurrentPso = Pso::Count;
}


// ================================================================================================================
// Drawing
// ================================================================================================================

// static
Renderer3DImplD3D12::Pso Renderer3DImplD3D12::trianglePsoFor(CullingOption cullingOption)
{
	// Mirrors Renderer3DImpl_Geometry.cpp::setCullMode exactly. See the Pso enum comment for why CullBack maps
	// to CULL_FRONT.
	switch (cullingOption)
	{
	case CullingOption::CullNone:  return Pso::TriangleCullNoneAlpha;
	case CullingOption::CullBack:  return Pso::TriangleCullFrontFacesAlpha;
	case CullingOption::CullFront: return Pso::TriangleCullBackFacesAlpha;
	default:                       return Pso::TriangleCullNoneAlpha;
	}
}

bool Renderer3DImplD3D12::beginDraw(Pso pso, const SimpleMath::Matrix& worldViewProjection, const SimpleMath::Vector4& color)
{
	if (!mCommandList) return false;
	auto& pipelineState = mPipelineStates[(size_t)pso];
	if (!pipelineState) return false;

	if (mCurrentPso != pso)
	{
		mCommandList->SetPipelineState(pipelineState.Get());
		mCurrentPso = pso;
	}

	// The matrix's four ROWS (XMFLOAT4X4::m is already row-major, so this memcpy IS row0..row3 in order), then
	// the colour. Matches the four float4 rows + float4 colour declared in kShaderSource.
	float constants[kRootConstantCount];
	std::memcpy(constants, worldViewProjection.m, sizeof(float) * 16);
	constants[16] = color.x;
	constants[17] = color.y;
	constants[18] = color.z;
	constants[19] = color.w;
	mCommandList->SetGraphicsRoot32BitConstants(0, kRootConstantCount, constants, 0);
	return true;
}

bool Renderer3DImplD3D12::drawIndexed(Pso pso, const SimpleMath::Matrix& worldViewProjection, const SimpleMath::Vector4& color,
	const DirectX::VertexPosition* vertices, size_t vertexCount, const uint16_t* indices, size_t indexCount,
	D3D_PRIMITIVE_TOPOLOGY topology)
{
	if (!mCommandList || vertexCount == 0 || indexCount == 0) return false;
	if (vertexCount > 0xFFFFu) return false;   // 16-bit indices; IModel's IndexCollection is uint16_t anyway

	D3D12_VERTEX_BUFFER_VIEW vertexView{};
	D3D12_INDEX_BUFFER_VIEW indexView{};
	if (!appendVertices(vertices, vertexCount, vertexView)) return false;
	if (!appendIndices(indices, indexCount, indexView)) return false;
	if (!beginDraw(pso, worldViewProjection, color)) return false;

	mCommandList->IASetPrimitiveTopology(topology);
	mCommandList->IASetVertexBuffers(0, 1, &vertexView);
	mCommandList->IASetIndexBuffer(&indexView);
	mCommandList->DrawIndexedInstanced((UINT)indexCount, 1, 0, 0, 0);
	return true;
}

void Renderer3DImplD3D12::drawTriangle(const std::array<SimpleMath::Vector3, 3>& vertexPositions,
	const SimpleMath::Vector4& color, CullingOption cullingOption, std::optional<TextureEnum> texture)
{
	if (texture.has_value() && !mLoggedTextureStub)
	{
		mLoggedTextureStub = true;
		PLOG_INFO << "Renderer3DImplD3D12: textured draws are not implemented on the D3D12 path; drawing untextured";
	}

	const DirectX::VertexPosition vertices[3] =
	{
		DirectX::VertexPosition(DirectX::XMFLOAT3(vertexPositions[0].x, vertexPositions[0].y, vertexPositions[0].z)),
		DirectX::VertexPosition(DirectX::XMFLOAT3(vertexPositions[1].x, vertexPositions[1].y, vertexPositions[1].z)),
		DirectX::VertexPosition(DirectX::XMFLOAT3(vertexPositions[2].x, vertexPositions[2].y, vertexPositions[2].z)),
	};
	static const uint16_t indices[3] = { 0, 1, 2 };

	drawIndexed(trianglePsoFor(cullingOption), mViewProjectionMatrix, color, vertices, 3, indices, 3,
		D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer3DImplD3D12::drawTriangleCollection(const IModelTriangles* model, const SimpleMath::Vector4& color,
	CullingOption cullingOption, std::optional<TextureEnum> texture)
{
	if (!model) return;
	if (texture.has_value() && !mLoggedTextureStub)
	{
		mLoggedTextureStub = true;
		PLOG_INFO << "Renderer3DImplD3D12: textured draws are not implemented on the D3D12 path; drawing untextured";
	}

	const VertexCollection& vertices = model->getTriangleVertices();
	const IndexCollection& indices = model->getTriangleIndices();
	drawIndexed(trianglePsoFor(cullingOption), mViewProjectionMatrix, color,
		vertices.data(), vertices.size(), indices.data(), indices.size(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer3DImplD3D12::drawEdge(const SimpleMath::Vector3& edgeStart, const SimpleMath::Vector3& edgeEnd,
	const SimpleMath::Vector4& color)
{
	const DirectX::VertexPosition vertices[2] =
	{
		DirectX::VertexPosition(DirectX::XMFLOAT3(edgeStart.x, edgeStart.y, edgeStart.z)),
		DirectX::VertexPosition(DirectX::XMFLOAT3(edgeEnd.x, edgeEnd.y, edgeEnd.z)),
	};
	static const uint16_t indices[2] = { 0, 1 };

	drawIndexed(Pso::LineAlpha, mViewProjectionMatrix, color, vertices, 2, indices, 2,
		D3D_PRIMITIVE_TOPOLOGY_LINELIST);
}

void Renderer3DImplD3D12::drawEdgeCollection(const IModelEdges* model, const SimpleMath::Vector4& color)
{
	if (!model) return;
	const VertexCollection& vertices = model->getEdgeVertices();
	const IndexCollection& indices = model->getEdgeIndices();
	drawIndexed(Pso::LineAlpha, mViewProjectionMatrix, color,
		vertices.data(), vertices.size(), indices.data(), indices.size(), D3D_PRIMITIVE_TOPOLOGY_LINELIST);
}

void Renderer3DImplD3D12::renderSphere(const SimpleMath::Vector3& position, const SimpleMath::Vector4& color,
	const float& scale, const bool& isWireframe)
{
	if (mSphereVertices.empty() || mSphereIndices.empty()) return;

	// Same rule GeometricPrimitive::Draw applies on the MCC path: translucent geometry blends and does not write
	// depth; fully opaque geometry writes depth. A solid sphere is also back-face culled there
	// (CommonStates::CullCounterClockwise == CULL_BACK), which is TriangleCullBackFaces* here.
	const bool opaque = color.w >= 1.f;
	Pso pso;
	if (isWireframe) pso = opaque ? Pso::WireframeOpaque : Pso::WireframeAlpha;
	else             pso = opaque ? Pso::TriangleCullBackFacesOpaque : Pso::TriangleCullBackFacesAlpha;

	const SimpleMath::Matrix world = SimpleMath::Matrix::CreateScale(scale) * SimpleMath::Matrix::CreateTranslation(position);
	drawIndexed(pso, world * mViewProjectionMatrix, color,
		mSphereVertices.data(), mSphereVertices.size(), mSphereIndices.data(), mSphereIndices.size(),
		D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}


// ================================================================================================================
// Projection / queries. Pure maths, ported from Renderer3DImpl_WorldToScreen.cpp.
// ================================================================================================================

bool Renderer3DImplD3D12::pointBehindCamera(const SimpleMath::Vector3& point)
{
	return (point - mCameraPosition).Dot(mCameraForward) < 0;
}

bool Renderer3DImplD3D12::pointOnScreen(const SimpleMath::Vector3& worldPointPosition)
{
	return mFrustumViewWorld.Contains(worldPointPosition) != DirectX::ContainmentType::DISJOINT;
}

float Renderer3DImplD3D12::cameraDistanceToWorldPoint(SimpleMath::Vector3 worldPointPosition)
{
	return SimpleMath::Vector3::Distance(mCameraPosition, worldPointPosition);
}

SimpleMath::Vector3 Renderer3DImplD3D12::worldPointToScreenPosition(SimpleMath::Vector3 worldPointPosition, bool shouldFlipBehind)
{
	SimpleMath::Vector4 clipSpace = SimpleMath::Vector4::Transform(
		SimpleMath::Vector4(worldPointPosition.x, worldPointPosition.y, worldPointPosition.z, 1.f),
		mViewProjectionMatrix);

	if (std::abs(clipSpace.w) < 1e-9f) clipSpace.w = (clipSpace.w < 0.f) ? -1e-9f : 1e-9f;
	clipSpace = clipSpace / clipSpace.w;

	if (shouldFlipBehind && pointBehindCamera(worldPointPosition))
	{
		// points in the semisphere behind the camera need their clip space mirrored back the other way
		clipSpace.x *= -1.f;
		clipSpace.y *= -1.f;
	}

	return SimpleMath::Vector3
	(
		(1.f + clipSpace.x) * 0.5f * mScreenSize.x,
		(1.f - clipSpace.y) * 0.5f * mScreenSize.y,
		clipSpace.z
	);
}

SimpleMath::Vector3 Renderer3DImplD3D12::worldPointToScreenPositionClamped(SimpleMath::Vector3 worldPointPosition,
	int screenEdgeOffset, bool* appliedClamp)
{
	auto screenPosition = worldPointToScreenPosition(worldPointPosition, true);

	if (pointOnScreen(worldPointPosition))
	{
		if (appliedClamp) *appliedClamp = false;
		return screenPosition;
	}
	if (appliedClamp) *appliedClamp = true;

	clampScreenPosition(screenPosition, mScreenCenter);
	return screenPosition;
}


// ================================================================================================================
// Sprites - NOT IMPLEMENTED. See the header for why. No-ops, never throws, never crashes.
// ================================================================================================================

RECTF Renderer3DImplD3D12::drawSprite(TextureEnum texture, SimpleMath::Vector2 screenPosition, float spriteScale, SimpleMath::Vector4 spriteColor)
{
	LOG_ONCE(PLOG_INFO << "Renderer3DImplD3D12::drawSprite is not implemented on the D3D12 path; ignoring");
	return { screenPosition.x, screenPosition.y, screenPosition.x, screenPosition.y };
}

RECTF Renderer3DImplD3D12::drawCenteredSprite(TextureEnum texture, SimpleMath::Vector2 screenPosition, float spriteScale, SimpleMath::Vector4 spriteColor)
{
	LOG_ONCE(PLOG_INFO << "Renderer3DImplD3D12::drawCenteredSprite is not implemented on the D3D12 path; ignoring");
	return { screenPosition.x, screenPosition.y, screenPosition.x, screenPosition.y };
}
