#pragma once
#include "pch.h"
#include <d3d12.h>

// The D3D12 counterpart of DirectXRenderEvent (see DirectXRenderEvent.h).
//
// WHY A SECOND EVENT INSTEAD OF MAKING DirectXRenderEvent BACKEND-AGNOSTIC:
// DirectXRenderEvent hands subscribers a live ID3D11Device* / ID3D11DeviceContext* /
// ID3D11RenderTargetView* and every one of its four subscribers (Render3DEventProvider,
// H2ArmorColour, H2ShadowResolution, PresetManager) dereferences them immediately - they create
// D3D11 buffers, set D3D11 state, and walk D3D11 device internals. There is no honest
// "backend-agnostic" payload for that: under D3D12 those three pointers simply do not exist, so
// erasing them behind void* would just move a guaranteed crash from compile time to run time,
// and widening the signature would force an edit to all four MCC cheats (which must stay
// byte-for-byte unchanged).
//
// So: on the D3D11 path ImGuiManager fires ForegroundDirectXRenderEvent exactly as before, and on
// the D3D12 path it fires ForegroundD3D12RenderEvent instead and NEVER fires the D3D11 one.
// The three backend-agnostic RenderEvents (Background/Midground/Foreground, which carry HCM's
// entire ImGui GUI - HCMInternalGUI, MessagesGUI, ModalDialogRenderer, HotkeyManager/Renderer)
// are fired on BOTH paths, so nothing that matters today renders nothing under HCE.
//
// Contract for subscribers (mirrors D3D12PresentEvent in D3D12Hook.h):
//   - pCommandList is already Reset() for this frame and is in the recording state,
//   - the back buffer is already in D3D12_RESOURCE_STATE_RENDER_TARGET,
//   - OMSetRenderTargets(backBufferRTV) and SetDescriptorHeaps(hook's SRV heap) are already done,
//   - the subscriber records draws and NOTHING ELSE: no Close(), no ExecuteCommandLists(), no
//     Signal(), no ResourceBarrier back to PRESENT. D3D12Hook owns all of that.
//   - a subscriber that throws costs the whole overlay: D3D12Hook catches, closes the list and
//     kills HCM, because an exception unwinding through DXGI/UE5 frames is not survivable.
//
// WHY backBufferFormat IS PART OF THE PAYLOAD: a D3D12 PSO bakes in RTVFormats[0], and there is no
// way to recover a resource's format from a D3D12_CPU_DESCRIPTOR_HANDLE. A subscriber that wants to
// record its own draws (Renderer3DImplD3D12) therefore cannot build a pipeline state at all without
// being told the swap chain's format - and guessing it wrong is not a cosmetic bug, it is a GPU
// validation failure in the GAME's command queue. D3D12Hook already knows it
// (D3D12Hook::getRenderTargetFormat), so ImGuiManager passes it through.
//
// This event is NEVER constructed-into or fired on the D3D11/MCC path, so its signature is free to
// change without touching any MCC behaviour.
typedef eventpp::CallbackList<void(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, SimpleMath::Vector2 screenSize, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV, DXGI_FORMAT backBufferFormat)> D3D12RenderEvent;
