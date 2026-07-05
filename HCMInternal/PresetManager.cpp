#include "pch.h"
#include "PresetManager.h"
#include "SettingsStateAndEvents.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "DirPathContainer.h"
#include "DirectXRenderEvent.h"
#include "ModalDialogGuard.h"
#include <Windows.h>
#include <commdlg.h>

PresetManager::PresetManager(GameState gameImpl, IDIContainer& dicon)
	: settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
	messagesWeak(dicon.Resolve<IMessagesGUI>()),
	mSavePresetCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->presetSaveEvent, [this]() { onSavePreset(); }),
	mLoadPresetCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->presetLoadEvent, [this]() { onLoadPreset(); })
{
	mPresetDir = (std::filesystem::path(dicon.Resolve<DirPathContainer>().lock()->dirPath) / "presets").string();

	// apply-on-load runs on the render thread so setting valueChangedEvents fire on the same thread GUI toggles do
	mRenderCallback = std::make_unique<ScopedCallback<DirectXRenderEvent>>(
		dicon.Resolve<DirectXRenderEvent>().lock(),
		[this](ID3D11Device* d, ID3D11DeviceContext* c, SimpleMath::Vector2 s, ID3D11RenderTargetView* r) { onRenderEvent(d, c, s, r); });
}

PresetManager::~PresetManager()
{
	PLOG_DEBUG << "~" << getName();
	// stop the render apply first, then the button callbacks - so nothing fires mid-teardown
	mRenderCallback.reset();
	mSavePresetCallback.removeCallback();
	mLoadPresetCallback.removeCallback();
}

std::optional<std::filesystem::path> PresetManager::browsePreset(bool saving)
{
	std::error_code ec; std::filesystem::create_directories(mPresetDir, ec);
	std::wstring initDir = std::filesystem::path(mPresetDir).wstring();
	wchar_t fileBuf[MAX_PATH]; fileBuf[0] = L'\0';
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = GetForegroundWindow();
	ofn.lpstrFilter = L"HCM Preset (*.hcm)\0*.hcm\0All Files (*.*)\0*.*\0";
	ofn.lpstrFile = fileBuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrInitialDir = initDir.c_str();
	ofn.lpstrDefExt = L"hcm";
	if (saving)
	{
		ofn.lpstrTitle = L"Save HCM Preset";
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
		if (GetSaveFileNameW(&ofn)) return std::filesystem::path(fileBuf);
	}
	else
	{
		ofn.lpstrTitle = L"Load HCM Preset";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetOpenFileNameW(&ofn)) return std::filesystem::path(fileBuf);
	}
	return std::nullopt; // cancelled
}

void PresetManager::onSavePreset()
{
	// one shared dialog at a time across ALL feature instances (this cheat is built once per game) - see ModalDialogGuard
	auto claim = ModalDialogGuard::tryClaim();
	if (!claim) return;
	try
	{
		lockOrThrow(settingsWeak, settings);
		auto chosen = browsePreset(true);
		if (!chosen) return; // cancelled
		settings->savePresetToFile(chosen->string()); // reads settings only, no valueChanged fired - safe here
		if (auto m = messagesWeak.lock()) m->addMessage("Preset saved: " + chosen->filename().string());
	}
	catch (HCMRuntimeException&) {}
}

void PresetManager::onLoadPreset()
{
	auto claim = ModalDialogGuard::tryClaim();
	if (!claim) return;
	try
	{
		lockOrThrow(settingsWeak, settings);
		auto chosen = browsePreset(false);
		if (!chosen) return; // cancelled
		// hand the path to the render thread; the actual apply (fires valueChanged on every setting) runs there
		{
			std::lock_guard<std::mutex> lk(mPendingMutex);
			mPendingLoadPath = chosen->string();
		}
		mLoadPending.store(true, std::memory_order_release);
	}
	catch (HCMRuntimeException&) {}
}

void PresetManager::onRenderEvent(ID3D11Device*, ID3D11DeviceContext*, SimpleMath::Vector2, ID3D11RenderTargetView*)
{
	if (!mLoadPending.exchange(false, std::memory_order_acq_rel)) return;
	std::string path;
	{
		std::lock_guard<std::mutex> lk(mPendingMutex);
		path = mPendingLoadPath;
	}
	if (path.empty()) return;
	try
	{
		lockOrThrow(settingsWeak, settings);
		settings->loadPresetFromFile(path); // fires each setting's valueChangedEvent on this (render) thread
		if (auto m = messagesWeak.lock())
			m->addMessage("Preset loaded: " + std::filesystem::path(path).filename().string());
	}
	catch (HCMRuntimeException&) {}
}
