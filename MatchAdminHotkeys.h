#pragma once

#include "GuiBase.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#if __has_include("bakkesmod/wrappers/GameObject/TeamWrapper.h")
#include "bakkesmod/wrappers/GameObject/TeamWrapper.h"
#elif __has_include("bakkesmod/wrappers/GameEvent/TeamWrapper.h")
#include "bakkesmod/wrappers/GameEvent/TeamWrapper.h"
#else
#error "TeamWrapper.h not found in expected locations (GameObject/ or GameEvent/)."
#endif

#include "version.h"
constexpr auto plugin_version =
stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);

class MatchAdminHotkeys
	: public BakkesMod::Plugin::BakkesModPlugin
	, public BakkesMod::Plugin::PluginSettingsWindow
{
	void onLoad() override;

	void AdjustBlueScore(int delta);
	void AdjustOrangeScore(int delta);
	void DoPauseToggle();
	void DoKickoffReset();

	void RenderSettings() override;
	std::string GetPluginName() override;
	void SetImGuiContext(uintptr_t ctx) override;

	void SaveCfg();
	void LoadKeyCvarsToUi();
};
