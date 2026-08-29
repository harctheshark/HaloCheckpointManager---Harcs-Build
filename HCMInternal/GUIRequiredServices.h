#pragma once
#include "IGUIRequiredServices.h"

// A sequence of pairs, where the first element of a pair is the GUIElementEnum name, and the second element is a tuple of supported games for that GUIElementEnum
#define TOPGUIELEMENTS_RELEASE \
((presetsHeadingGUI,(ALL_GAMES_AND_MAINMENU)))\
((controlHeadingGUI,(ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
((saveManagementHeadingGUI,(ALL_SUPPORTED_GAMES_AND_HALOCER)))\
((cheatsHeadingGUI,(ALL_SUPPORTED_GAMES_AND_HALOCER)))\
((overlaysHeadingGUI,(ALL_SUPPORTED_GAMES_AND_HALOCER)))\
((cameraHeadingGUI,(ALL_SUPPORTED_GAMES_AND_HALOCER)))\
((theaterHeadingGUI,(Halo3,Halo3ODST,HaloReach,Halo4)))\
((debugHeadingGUI, (ALL_SUPPORTED_GAMES)))\
((hceScriptHeadingGUI, (HALOCER_ONLY)))

#define TOPGUIELEMENTS_DEBUG \
((HCMDebugHeadingGUI, (ALL_SUPPORTED_GAMES)))

#ifdef HCM_DEBUG

#define TOPGUIELEMENTS_ANDSUPPORTEDGAMES BOOST_PP_CAT(TOPGUIELEMENTS_RELEASE, TOPGUIELEMENTS_DEBUG)

#else 

#define TOPGUIELEMENTS_ANDSUPPORTEDGAMES TOPGUIELEMENTS_RELEASE

#endif


class GUIRequiredServices : public IGUIRequiredServices {
private:
	static const std::vector<std::pair< GUIElementEnum, GameState>> toplevelGUIElements;
	static const std::map <GUIElementEnum, std::vector<OptionalCheatEnum>> requiredServicesPerGUIElement;
	static const std::map<GUIElementEnum, std::set<GameState>> supportedGamesPerGUIElement;

public:
	virtual const std::vector<std::pair< GUIElementEnum, GameState>>& getToplevelGUIElements() override
	{
		return toplevelGUIElements;
	}
	virtual const std::map < GUIElementEnum, std::vector<OptionalCheatEnum>>& getRequiredServicesPerGUIElement() override
	{
		return requiredServicesPerGUIElement;
	}

	virtual const std::map < GUIElementEnum, std::set<GameState>>& getSupportedGamesPerGUIElement() override
	{
		return supportedGamesPerGUIElement;
	}

	virtual const std::set<std::pair<GameState, OptionalCheatEnum>>& getAllRequiredServices() override;

	GUIRequiredServices() { PLOG_DEBUG << "GUIRequiredServices con"; }
	~GUIRequiredServices() { PLOG_DEBUG << "~GUIRequiredServices"; }
};