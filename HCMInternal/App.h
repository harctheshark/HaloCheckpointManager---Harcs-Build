#pragma once
#include "Logging.h"
#include "GlobalKill.h"
#include "ModuleCache.h"
#include "ModuleHookManager.h"
#include "D3D11Hook.h"
#include "D3D12Hook.h"
#include "ImGuiManager.h"
#include "PointerDataStore.h"
#include "PointerDataParser.h"
#include "PointerDataGetter.h"
#include "HCMInternalGUI.h"
#include "MessagesGUI.h"
#include "MCCStateHook.h"
#include "HCEStateHook.h"
#include "HeartbeatTimer.h"
#include "GUIServiceInfo.h"
#include "HotkeyManager.h"
#include "HotkeyRenderer.h"
#include "HotkeyRendererImpl.h"
#include "HotkeyDefinitions.h"
#include "GetMCCVersion.h"
#include "GUIRequiredServices.h"
#include "OptionalCheatManager.h"
#include "OptionalCheatInfo.h"
#include "GUIElementStore.h"
#include "GUIElementConstructor.h"
#include "UnhandledExceptionHandler.h"
#include "SettingsSerialiser.h"
#include "SharedMemoryInternal.h"
#include "DynamicStructFactory.h"
#include "IMessagesGUI.h"
#include "HotkeyEventsLambdas.h"
#include "ModalDialogRenderer.h"
#include "ControlServiceContainer.h"
#include "Lapua.h"
#include "OBSBypassManager.h"
#include "ModalDialogFactory.h"
#include "HideWatermarkManager.h"
#include "GetHCMVersion.h"

class App {


public:
	App(HMODULE dllHandle)
	{
        GlobalKill::reviveMe();
        GlobalKill::HCMInternalModuleHandle = dllHandle;

        std::shared_ptr<UnhandledExceptionHandler> unhandled; // init later, but we need it to be the last thing to go out of scope

        // these are needed in the init exception catch block, so declared here
        auto logging = std::make_shared<Logging>();

#ifdef HCM_DEBUG
        logging->initConsoleLogging();
        logging->SetConsoleLoggingLevel(plog::verbose);
#endif
        std::shared_ptr<SharedMemoryInternal> sharedMem;
        
        try
        {
            sharedMem = std::make_shared<SharedMemoryInternal>();
            sharedMem->setStatusFlag(HCMInternalStatus::Initialising);
        }
        catch(HCMInitException ex)
        {
            int msgboxID = MessageBoxA(
                NULL,
                std::format("HCM internal failed to create shared memory, error:\n{}", ex.what()).c_str(),
                "Halo checkpoint manager error",
                MB_OK
            );
            return;
        }

        std::string dirPath = sharedMem->HCMDirPath;
       

        logging->initFileLogging(dirPath);
        logging->SetFileLoggingLevel(plog::verbose);
        PLOG_INFO << "HCMInternal initializing. DirPath: " << dirPath;
        unhandled = std::make_shared<UnhandledExceptionHandler>(dirPath); PLOGV << "unhandled init";
       //curl_global_init(CURL_GLOBAL_DEFAULT);
        try
        {
            // some very important services
            ModuleCache::initialize(); PLOGV << "moduleCache init"; // static singleton still.. blah
            auto mhm = std::make_unique<ModuleHookManager>(); PLOGV << "mhm init"; // is a static singleton still.. blah 
            auto ver = std::make_shared<GetMCCVersion>(); PLOGV << "ver init";// gets the version of MCC that we're currently injected into


            // load dynamic (version & game specific) pointer data
            // latest data is pulled from github page
            std::string pointerXMLData = PointerDataGetter::getXMLDocument(dirPath);

            // parse it into a keyed map of data
            auto [dataMap, parsingErrors] = PointerDataParser::parseVersionedData(ver, pointerXMLData);

            // store it 
            auto ptrStore = std::make_shared<PointerDataStore>(dataMap); PLOGV << "ptrStore init";


            // Is this version of MCC supported according to the pointer data? Exception if not.
            if (auto suppV = PointerDataParser::parseSupportedMCCVersions(pointerXMLData); !suppV.has_value() || suppV.value().find(ver->getMCCVersionAsString().data()) == suppV.value().end())
            {
                if (suppV.has_value())
                    throw HCMInitException(std::format("The current version of MCC ({}) is not yet supported by HCM! ", ver->getMCCVersionAsString()) +
                        "\nYou'll have to wait for Burnt to update it if HCM just got a patch," +
                        "\nor if you're on an old MCC patch, kindly ask him to add support.");
                else
                    throw HCMInitException(std::format("Failed to parse currently supported MCC versions, error: {}", suppV.error()));
            }



            // setup some optional services mainly related to controls, eg freeing the cursor, pausing the game, etc
            auto hotkeyDisabler = std::make_shared< TokenSharedRequestProvider>();
            auto control = std::make_shared<ControlServiceContainer>(ptrStore, hotkeyDisabler);

     
            // Which graphics API are we in? MCC (Steam/WinStore) is D3D11; Halo Campaign Evolved
            // (HaloCampaignEvolved.exe, UE 5.5.4) is D3D12. Decided once, here, and nothing below
            // this point re-evaluates it.
            const bool isCampaignEvolved = (ver->getMCCProcessType() == MCCProcessType::CampaignEvolved);

            // DECLARATION ORDER HERE IS LOAD-BEARING.
            // These are automatic locals, so they are destroyed in REVERSE declaration order: `imm`
            // goes first, then the graphics hook. That ordering is what lets ~ImGuiManager call
            // d3d12->waitForGpuIdle() *before* ImGui_ImplDX12_Shutdown() frees imgui's PSO / root
            // signature / vertex+index ring / font texture - all of which the last submitted overlay
            // command list still references, and none of which D3D12 defers destruction on.
            // Exactly one of d3d/d3d12 is ever non-null.
            std::shared_ptr<D3D11Hook> d3d;
            std::shared_ptr<D3D12Hook> d3d12;
            std::shared_ptr<ImGuiManager> imm;
            if (isCampaignEvolved)
            {
                d3d12 = std::make_shared<D3D12Hook>(ptrStore); PLOGV << "d3d12 init"; // hooks dxgi Present/Present1/ResizeBuffers(1) and d3d12 ExecuteCommandLists
                imm = std::make_shared<ImGuiManager>(d3d12, d3d12->presentHookEvent); PLOGV << "imm init"; // sets up imgui context and fires off imgui render events
            }
            else
            {
                d3d = std::make_shared<D3D11Hook>(ptrStore); PLOGV << "d3d init"; // hooks d3d11 Present and ResizeBuffers
                imm = std::make_shared<ImGuiManager>(d3d, d3d->presentHookEvent); PLOGV << "imm init"; // sets up imgui context and fires off imgui render events
            }

            auto mes = std::make_shared<MessagesGUI>(ImVec2{ 20, 20 }, imm->ForegroundRenderEvent); PLOGV << "mes init";// renders temporary messages to the screen
            auto imes = std::reinterpret_pointer_cast<IMessagesGUI>(mes);
            auto exp = std::make_shared<RuntimeExceptionHandler>(mes); PLOGV << "exp init";// tells user if a cheat hook throws a runtime exception
            auto settings = std::make_shared<SettingsStateAndEvents>(std::make_shared<SettingsSerialiser>(dirPath, exp, mes)); PLOGV << "settings init";
           
            // fires event when game or level changes. HCE is not MCC (no gameEngine/load/menu
            // indicators to hook), so it derives the same MCCState from HaloSimulation_tag_release.dll.
            std::shared_ptr<IMCCStateHook> mccStateHook;
            std::shared_ptr<HCEStateHook> hceStateHook; // concrete handle kept so we can read its cursor flag below
            if (isCampaignEvolved)
            {
                hceStateHook = std::make_shared<HCEStateHook>(); PLOGV << "hceStateHook init";
                mccStateHook = hceStateHook;
            }
            else
            {
                mccStateHook = std::make_shared<MCCStateHook>(ptrStore, exp); PLOGV << "mccStateHook init";
            }
            auto guifail = std::make_shared<GUIServiceInfo>(mes); PLOGV << "guifail init"; // stores info about gui elements that failed to construct. starts empty, filled up later
            auto modal = std::make_shared<ModalDialogRenderer>(imm->ForegroundRenderEvent, control, hotkeyDisabler); PLOGV << "modal init"; // renders modal dialogs that can be called from optionalCheats

            // connect showFailedOptionalServices button to modal dialog
            auto showGUIFailuresCallback = ScopedCallback<ActionEvent>(settings->showGUIFailures, [modal, guifail]() {modal->showVoidDialog(ModalDialogFactory::makeFailedOptionalCheatServicesDialog(guifail)); });

            mes->setSettings(settings);
            // hotkeys

            auto hke = std::make_shared<HotkeyEventsLambdas>(settings); // binds toggle hotkey events to lambdas of toggling settings etc
            auto hkd = std::make_shared<HotkeyDefinitions>(settings); PLOGV << "hkd init";
            auto hkm = std::make_shared<HotkeyManager>(imm->ForegroundRenderEvent, hkd, mes, dirPath, hotkeyDisabler); PLOGV << "hkm init"; // hotkey manager doesn't render but wants to poll inputs every frame to know if keys pressed etc
            
            auto hkrimpl = std::make_unique<HotkeyRendererImpl>(imm->ForegroundRenderEvent, mes, hkm, hkd, hotkeyDisabler);
            auto hkr = std::make_shared<HotkeyRenderer>(std::move(hkrimpl)); PLOGV << "hkr init"; // render hotkeys and rebinding
            
        
            // set up rendering
            // isCursorShowing gates ALL mouse input to the overlay (HCMInternalGUI applies ImGuiWindowFlags_NoInputs
            // when it is false), so it is load-bearing, not cosmetic. MCC exposes its own cursor bool at a known
            // address. HCE has no equivalent we know of, so HCEStateHook derives the same fact from the WIN32 cursor
            // state (GetCursorInfo/CURSOR_SHOWING) and we point at that - engine-agnostic and true to what the flag means.
            uintptr_t isCursorShowingResolved;
            if (isCampaignEvolved)
            {
                isCursorShowingResolved = (uintptr_t)hceStateHook->getCursorShowingFlag();
                PLOG_DEBUG << "HCE: isCursorShowing sourced from live WIN32 cursor state";
            }
            else
            {
                auto isCursorShowingPtr = ptrStore->getData<std::shared_ptr<MultilevelPointer>>("isCursorShowing");
                if (!isCursorShowingPtr->resolve(&isCursorShowingResolved)) throw HCMInitException(std::format("Could not resolve isCursorShowing: {}", MultilevelPointer::GetLastError()));
            }


            // set up optional cheats and optional gui elements
            auto guireq = std::make_shared<GUIRequiredServices>(); PLOGV << "guireq init"; // defines the gui elements we want to build and which optional cheats they will require
            auto cheatfail = std::make_shared<OptionalCheatInfo>(); PLOGV << "cheatfail init"; // stores info about failed optionalCheat construction (starts empty, obviously)
            // Both DirectX render events go in. ImGuiManager fires exactly one of them for the lifetime of the
            // process (D3D11 for MCC, D3D12 for HaloCER - see D3D12RenderEvent.h), and cheats only resolve the
            // one their own game uses, so passing both is inert on either path.
            auto optionalCheats = std::make_shared<OptionalCheatManager>(guireq, cheatfail, settings, ptrStore, ver, mccStateHook, sharedMem, mes, exp, dirPath, modal, control, imm->BackgroundRenderEvent, imm->ForegroundDirectXRenderEvent, imm->ForegroundD3D12RenderEvent, hkd); PLOGV << "optionalCheats init"; // constructs and stores required optional cheats. Needs a lot of dependencies, cheats will only keep what they need.

            auto guistore = std::make_shared<GUIElementStore>(); PLOGV << "guistore init"; // collection starts empty, populated later by GUIElementConstructor
            auto GUICon = std::make_shared<GUIElementConstructor>(guireq, cheatfail, guistore, guifail, settings, ver->getMCCProcessType(), exp); PLOGV << "GUIMan init"; // constructs gui elements, pushing them into guistore
            //guifail->printAllFailures();
            // set up main gui
            // (mccStateHook is already a shared_ptr<IMCCStateHook>, so the old
            // reinterpret_pointer_cast<IMCCStateHook> here is no longer needed.)
            auto HCMGUI = std::make_shared<HCMInternalGUI>(mccStateHook, guistore, hkr, imm->MidgroundRenderEvent, mccStateHook->getMCCStateChangedEvent(), control, settings, (bool*)isCursorShowingResolved); PLOGV << "HCMGUI init";// main gui. Mostly just a canvas for rendering a collection of IGUIElements that will get constructed a bit below.
            mes->setAnchorPoint(HCMGUI);

            auto hb = std::make_shared<HeartbeatTimer>(sharedMem, settings); PLOGV << "hb init";

            auto lap = std::make_shared<Lapua>(); PLOGV << "lapua init";
            // OBS bypass has no D3D12 capture path we have offsets for; the D3D12 overload exists so
            // the service graph is identical on both games and toggling it gives a clean
            // "not supported" message instead of doing nothing.
            auto obsBypass = isCampaignEvolved
                ? std::make_shared<OBSBypassManager>(std::weak_ptr<D3D12Hook>(d3d12), settings->OBSBypassToggle, exp)
                : std::make_shared<OBSBypassManager>(std::weak_ptr<D3D11Hook>(d3d), settings->OBSBypassToggle, exp); PLOGV << "obsBypass init";
            auto hideWatermark = std::make_shared<HideWatermarkManager>(settings->hideWatermark, exp); PLOGV << "hideWatermark init";
            // Installed LAST, once every service that its callbacks touch exists.
            if (isCampaignEvolved)
                d3d12->beginHook();
            else
                d3d->beginHook();

            PLOG_INFO << "All services succesfully initialized! Entering main loop";
            Sleep(100);

            imes->addMessage("HCM successfully initialised!\n");

#ifdef HCM_DEBUG
            for (auto parsingError : parsingErrors)
            {
                imes->addMessage(parsingError.what());
            }
#endif


            std::thread modalFailureWindowThread;
            if (!guifail->getFailureMessagesMap().empty())
            {
                PLOG_DEBUG << "creating showFailedOptionalCheatServices modal dialog ";
                modalFailureWindowThread = std::thread{ ([modal = modal, guifail]() { Sleep(500); modal->showVoidDialog(ModalDialogFactory::makeFailedOptionalCheatServicesDialog(guifail)); }) };
                modalFailureWindowThread.detach();
            }

            // Is this version of HCM up to date? We'll warn the user if not
            auto currentHCMVersion = getHCMVersion();
            if (!currentHCMVersion)
            {
                imes->addMessage(std::format("Could not determine own HCM version, error: {}\nSkipping check for newer HCM versions.\n", currentHCMVersion.error()));
            }
            else
            {
                PLOG_INFO << "Current HCM Version: " << currentHCMVersion.value();
                if (auto suppV = PointerDataParser::parseSupportedHCMVersions(pointerXMLData); !suppV.has_value() || suppV.value().find(currentHCMVersion.value().operator std::string()) == suppV.value().end())
                {
                    if (suppV.has_value())
                    {
                        imes->addMessage("A newer version of HCM exists, probably with bugfixes or new features.\nFind it at github.com/Burnt-o/HaloCheckpointManager/releases\n");
                        PLOG_DEBUG << "Supported versions:"; for (auto& v : suppV.value()) { PLOG_DEBUG << v; }
                    }
                    else
                    {
                        imes->addMessage(std::format("Failed to parse currently supported HCM versions, error: {}\nSkipping check for newer HCM versions.\n", suppV.error()));
                    }

                }
            }
            

            sharedMem->setStatusFlag(HCMInternalStatus::AllGood);

            assert(VersionInfo(3, 1, 1, 1) > VersionInfo(3, 1, 1, 0) && "ee");
            assert(VersionInfo(4, 1, 1, 1) > VersionInfo(3, 1, 1, 2) && "EEEE");
            PLOG_DEBUG << VersionInfo(3, 1, 1, 1).operator std::string();
            PLOG_DEBUG << "a: " << VersionInfo(3, 1, 1, 1).operator std::string().length();
            PLOG_DEBUG << "3.1.1.1";
            PLOG_DEBUG << "b: " << std::string("3.1.1.1").length();
            assert(VersionInfo(3, 1, 1, 1).operator std::string() == "3.1.1.1");


            // We live in this loop 99% of the time
            while (!GlobalKill::isKillSet()) {
                Sleep(10);
            }
            PLOG_INFO << "HCMInternal services are about to fall out of scope";

            if (modalFailureWindowThread.joinable())
                modalFailureWindowThread.join();


            sharedMem->setStatusFlag(HCMInternalStatus::Shutdown);
        }
        catch (HCMInitException& ex) // mandatory services that fail to init will be caught here
        {

            std::ostringstream oss;
            oss << "\n\nHCMInternal failed initializing: " << ex.what() << std::endl
                << "Please send Burnt the log file located at: " << std::endl << logging->GetLogFileDestination();
            PLOG_FATAL << oss.str();

            sharedMem->setStatusFlag(HCMInternalStatus::Error);

            // ⚠ KILL FIRST, ASK LATER. GlobalKill is what lets RealMain fall through to
            // FreeLibraryAndExitThread, so anything BLOCKING in front of it keeps HCMInternal.dll pinned in
            // the game for as long as the block lasts. MessageBoxA on the game's thread, behind a fullscreen
            // game, can block indefinitely - the user never sees the box. That is precisely what left a stale
            // DLL resident and made the next HCM launch fail with "a previous HCMInternal is still loaded".
            // Setting the flag first costs nothing and bounds the damage to "the box is still up".
            GlobalKill::killMe();

            // Only worth showing at all if HCMExternal is still around to have asked for this injection; an
            // orphan instance failing after the external has exited has nobody to tell, and its box would be
            // an invisible modal on the game's thread.
            if (findProcess(L"HCMExternal.exe") || findProcess(L"HaloCheckpointManager.exe"))
            {
                MessageBoxA(
                    NULL,
                    oss.str().c_str(),
                    "Halo Checkpoint Manager error",
                    MB_OK
                );
            }
            else
            {
                PLOG_WARNING << "suppressing the error dialog: HCMExternal is gone, so this instance is an "
                    "orphan and a modal box would only block the game's thread and pin the DLL";
            }
        }
       // curl_global_cleanup(); PLOG_INFO << "Curl cleaned up";
        // Auto managed resources have fallen out of scope
        PLOG_INFO << "HCMInternal services successfully shut down";


#ifdef HCM_DEBUG
        PLOG_DEBUG << "Closing console";
        logging->closeConsole();
#endif 

	}


};