#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "ScopedCallback.h"
#include "ControlDefs.h"          // ActionEvent
#include "DirectXRenderEvent.h"
#include <atomic>
#include <mutex>
#include <string>
#include <optional>
#include <filesystem>

class SettingsStateAndEvents;
class IMessagesGUI;

// Save / load a FULL HCM settings snapshot (every toggle, value and hotkey) to a named .hcm preset file.
//
// The "Save Preset..." / "Load Preset..." buttons fire ActionEvents on GUISimpleButton's own detached worker
// thread; we run the modal file dialog there (a dialog CANNOT be opened on the render/present thread - that
// deadlocks the game). The catch: while a modal dialog is open the game keeps rendering and HCM's cursor state is
// frozen over the button, so clicks you make INSIDE the dialog leak back into ImGui as more button presses. Left
// unguarded that spawns a fresh dialog per stray click (the "close it 5-10 times" bug). mDialogOpen is a
// non-blocking guard: while one dialog is open every extra fire returns immediately, so it's always one dialog.
//
// For LOAD we don't apply on the worker thread - we stash the chosen path and apply inside the render event, so
// every setting's valueChangedEvent fires on the render thread (the same thread the GUI toggles use). The apply is
// just quick settings work (no dialog), so it's safe on the render thread.
//
// Game-agnostic: constructs for every game and the main menu (presets are pure config, no game memory touched).
class PresetManager : public IOptionalCheat
{
private:
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::string mPresetDir; // <hcm dir>/presets

	ScopedCallback<ActionEvent> mSavePresetCallback;
	ScopedCallback<ActionEvent> mLoadPresetCallback;
	std::unique_ptr<ScopedCallback<DirectXRenderEvent>> mRenderCallback;

	// one-dialog-at-a-time is enforced by the shared ModalDialogGuard (see onSavePreset/onLoadPreset).

	// load path handed from the dialog (worker thread) to the apply (render thread). Static because the DI builds one
	// PresetManager per game: any instance's render callback may be the one to apply it, and there's only ever one
	// pending load at a time.
	static inline std::mutex mPendingMutex;
	static inline std::string mPendingLoadPath;
	static inline std::atomic_bool mLoadPending{ false };

	std::optional<std::filesystem::path> browsePreset(bool saving); // modal file dialog (worker thread)
	void onSavePreset(); // worker thread: dialog + write file
	void onLoadPreset(); // worker thread: dialog + queue path for the render thread
	void onRenderEvent(ID3D11Device* device, ID3D11DeviceContext* ctx, SimpleMath::Vector2 screenSize, ID3D11RenderTargetView* rtv); // render thread: apply queued preset

public:
	PresetManager(GameState gameImpl, IDIContainer& dicon);
	~PresetManager();

	std::string_view getName() override { return nameof(PresetManager); }
};
