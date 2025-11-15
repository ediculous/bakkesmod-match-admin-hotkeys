#include "pch.h"
#include "MatchAdminHotkeys.h"
#include <optional>
#include <utility>
#include <string>
#include <fstream>
#include <filesystem>
#include <set>
#include <regex>
#include <vector>
#include "imgui/imgui.h"

#if __has_include("bakkesmod/wrappers/GameObject/ServerWrapper.h")
#include "bakkesmod/wrappers/GameObject/ServerWrapper.h"
#elif __has_include("bakkesmod/wrappers/GameEvent/ServerWrapper.h")
#include "bakkesmod/wrappers/GameEvent/ServerWrapper.h"
#else
#error "ServerWrapper.h not found in expected locations (GameObject/ or GameEvent/)."
#endif

#include "bakkesmod/wrappers/ArrayWrapper.h"

#if __has_include("bakkesmod/wrappers/PlayerControllerWrapper.h")
#include "bakkesmod/wrappers/PlayerControllerWrapper.h"
#elif __has_include("bakkesmod/wrappers/GameObject/PlayerControllerWrapper.h")
#include "bakkesmod/wrappers/GameObject/PlayerControllerWrapper.h"
#elif __has_include("bakkesmod/wrappers/GameEvent/PlayerControllerWrapper.h")
#include "bakkesmod/wrappers/GameEvent/PlayerControllerWrapper.h"
#else
#error "PlayerControllerWrapper.h not found (checked wrappers/, GameObject/, GameEvent/)"
#endif

BAKKESMOD_PLUGIN(MatchAdminHotkeys, "Match Admin Hotkeys", plugin_version, PLUGINTYPE_FREEPLAY)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

static constexpr auto NOTI_BLUE_PLUS = "mah_blue_plus";
static constexpr auto NOTI_ORANGE_PLUS = "mah_orange_plus";
static constexpr auto NOTI_BLUE_MINUS = "mah_blue_minus";
static constexpr auto NOTI_ORANGE_MINUS = "mah_orange_minus";
static constexpr auto NOTI_PAUSE_TOGGLE = "mah_pause_toggle";
static constexpr auto NOTI_RESET = "mah_reset_kickoff";

static constexpr auto CVAR_PAUSE_CMD = "mah_pause_cmd";
static constexpr auto CVAR_RESET_CMD = "mah_reset_cmd";
static constexpr auto CVAR_ENABLED = "mah_enabled";

static constexpr auto CVAR_KEY_BLUE_PLUS = "mah_key_blue_plus";
static constexpr auto CVAR_KEY_ORANGE_PLUS = "mah_key_orange_plus";
static constexpr auto CVAR_KEY_BLUE_MINUS = "mah_key_blue_minus";
static constexpr auto CVAR_KEY_ORANGE_MINUS = "mah_key_orange_minus";
static constexpr auto CVAR_KEY_PAUSE = "mah_key_pause_toggle";
static constexpr auto CVAR_KEY_RESET = "mah_key_reset_kickoff";

static std::string ui_key_blue_plus;
static std::string ui_key_orange_plus;
static std::string ui_key_blue_minus;
static std::string ui_key_orange_minus;
static std::string ui_key_pause;
static std::string ui_key_reset;

static std::string last_blue_plus, last_orange_plus, last_blue_minus, last_orange_minus, last_pause, last_reset;
static bool unsavedToastShown = false;

static void ExecMulti(const std::shared_ptr<CVarManagerWrapper>& cvars, const std::string& cmdsSemicolonSeparated)
{
	if (cmdsSemicolonSeparated.empty()) return;
	cvars->executeCommand(cmdsSemicolonSeparated);
}

static std::optional<std::pair<TeamWrapper, TeamWrapper>> FindTeams(GameWrapper* gw)
{
	if (!gw || !gw->IsInGame()) return std::nullopt;
	auto server = gw->GetCurrentGameState();
	if (!server) return std::nullopt;
	ArrayWrapper<TeamWrapper> teams = server.GetTeams();
	if (teams.IsNull() || teams.Count() < 2) return std::nullopt;
	TeamWrapper blue = teams.Get(0);
	TeamWrapper orange = teams.Get(1);
	if (!blue || !orange) return std::nullopt;
	return std::make_pair(blue, orange);
}

static std::optional<PlayerControllerWrapper> GetLocalPC(GameWrapper* gw, ServerWrapper server)
{
	if (!gw) return std::nullopt;
	PlayerControllerWrapper pc = gw->GetPlayerController();
	if (pc) return pc;
	ArrayWrapper<PlayerControllerWrapper> locals = server.GetLocalPlayers();
	if (!locals.IsNull())
	{
		const int n = locals.Count();
		for (int i = 0; i < n; ++i)
		{
			PlayerControllerWrapper p = locals.Get(i);
			if (p) return p;
		}
	}
	return std::nullopt;
}

static void NormalizeKey(std::string& v)
{
	if (v.empty()) return;
	char c = v[0];
	if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
	v.assign(1, c);
}

static void UnbindAllManagedKeys(const std::shared_ptr<CVarManagerWrapper>& cvars)
{
	std::string cmd;
	cmd.reserve(1000);
	for (char c = 'A'; c <= 'Z'; ++c)
	{
		cmd += "unbind ";
		cmd += c;
		cmd += ";unbind ";
		cmd += static_cast<char>(c + 32);
		cmd += ";";
	}
	cmd += "unbind Slash;";
	cvars->executeCommand(cmd);
}

static void PersistBinds(const std::shared_ptr<CVarManagerWrapper>& cvars)
{
	cvars->executeCommand("writeconfig");
}

static void ScrubBindsCfgPhantoms(const std::shared_ptr<CVarManagerWrapper>& cvars, GameWrapper* gw)
{
	if (!gw) return;
	std::filesystem::path bindsPath = gw->GetBakkesModPath() / "cfg" / "binds.cfg";
	std::ifstream in(bindsPath);
	if (!in.is_open()) return;
	static const std::regex lineRe(R"REG(^\s*bind\s+([^\s]+)\s+"([^"]+)")REG", std::regex::icase);
	std::set<std::string> targets = {
		NOTI_BLUE_PLUS, NOTI_ORANGE_PLUS, NOTI_BLUE_MINUS, NOTI_ORANGE_MINUS, NOTI_PAUSE_TOGGLE, NOTI_RESET
	};
	std::string line;
	std::vector<std::string> keysToUnbind;
	while (std::getline(in, line))
	{
		std::smatch m;
		if (std::regex_search(line, m, lineRe))
		{
			if (m.size() >= 3)
			{
				std::string key = m[1].str();
				std::string cmd = m[2].str();
				for (const auto& t : targets)
				{
					if (cmd.find(t) != std::string::npos)
					{
						keysToUnbind.push_back(key);
						break;
					}
				}
			}
		}
	}
	in.close();
	if (!keysToUnbind.empty())
	{
		std::string unbindCmd;
		for (auto& k : keysToUnbind)
		{
			unbindCmd += "unbind ";
			unbindCmd += k;
			unbindCmd += ";";
		}
		cvars->executeCommand(unbindCmd);
		PersistBinds(cvars);
		LOG("MAH: Scrubbed phantom binds for %zu key(s) from binds.cfg", keysToUnbind.size());
	}
}

static inline bool IsDirtyNow()
{
	return ui_key_blue_plus != last_blue_plus
		|| ui_key_blue_minus != last_blue_minus
		|| ui_key_orange_plus != last_orange_plus
		|| ui_key_orange_minus != last_orange_minus
		|| ui_key_pause != last_pause
		|| ui_key_reset != last_reset;
}

static inline bool IsAtDefaultsSaved()
{
	return last_blue_plus == "U"
		&& last_blue_minus == "J"
		&& last_orange_plus == "I"
		&& last_orange_minus == "K"
		&& last_pause == "P"
		&& last_reset == "O";
}

static inline void SnapshotLastSaved()
{
	last_blue_plus = ui_key_blue_plus;
	last_blue_minus = ui_key_blue_minus;
	last_orange_plus = ui_key_orange_plus;
	last_orange_minus = ui_key_orange_minus;
	last_pause = ui_key_pause;
	last_reset = ui_key_reset;
	unsavedToastShown = false;
}

static bool ButtonMaybeDisabled(const char* label, bool disabled, const ImVec2& size)
{
	if (disabled)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
	}
	bool pressed = ImGui::Button(label, size);
	if (disabled)
	{
		ImGui::PopStyleVar();
	}
	return !disabled && pressed;
}

static bool EmphasizedSaveButton(const char* label, bool emphasize, bool disabled, const ImVec2& size)
{
	if (emphasize && !disabled)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
	}
	bool pressed = ButtonMaybeDisabled(label, disabled, size);
	if (emphasize && !disabled)
	{
		ImGui::PopStyleColor(2);
	}
	return pressed;
}

void MatchAdminHotkeys::onLoad()
{
	_globalCvarManager = cvarManager;
	LOG("Match Admin Hotkeys loaded {}", plugin_version);
	if (gameWrapper) {
		gameWrapper->Toast("MatchAdminHotkeys", "Loaded " + std::string(plugin_version));
	}
	cvarManager->registerCvar(CVAR_KEY_BLUE_PLUS, "U", "Key");
	cvarManager->registerCvar(CVAR_KEY_ORANGE_PLUS, "I", "Key");
	cvarManager->registerCvar(CVAR_KEY_BLUE_MINUS, "J", "Key");
	cvarManager->registerCvar(CVAR_KEY_ORANGE_MINUS, "K", "Key");
	cvarManager->registerCvar(CVAR_KEY_PAUSE, "P", "Key");
	cvarManager->registerCvar(CVAR_KEY_RESET, "O", "Key");

	cvarManager->executeCommand("exec matchadminhotkeys.cfg");
	cvarManager->registerCvar(CVAR_PAUSE_CMD, "", "Optional");
	cvarManager->registerCvar(CVAR_RESET_CMD, "", "Optional");
	cvarManager->registerCvar(CVAR_ENABLED, "1", "Enable");

	cvarManager->registerNotifier(NOTI_BLUE_PLUS, [this](std::vector<std::string>) { AdjustBlueScore(+1); }, "Add 1 to Blue", PERMISSION_ALL);
	cvarManager->registerNotifier(NOTI_ORANGE_PLUS, [this](std::vector<std::string>) { AdjustOrangeScore(+1); }, "Add 1 to Orange", PERMISSION_ALL);
	cvarManager->registerNotifier(NOTI_BLUE_MINUS, [this](std::vector<std::string>) { AdjustBlueScore(-1); }, "Remove 1 from Blue", PERMISSION_ALL);
	cvarManager->registerNotifier(NOTI_ORANGE_MINUS, [this](std::vector<std::string>) { AdjustOrangeScore(-1); }, "Remove 1 from Orange", PERMISSION_ALL);
	cvarManager->registerNotifier(NOTI_PAUSE_TOGGLE, [this](std::vector<std::string>) { DoPauseToggle(); }, "Toggle server pause", PERMISSION_ALL);
	cvarManager->registerNotifier(NOTI_RESET, [this](std::vector<std::string>) { DoKickoffReset(); }, "Reset to kickoff", PERMISSION_ALL);

	LoadKeyCvarsToUi();
	SnapshotLastSaved();
}

void MatchAdminHotkeys::AdjustBlueScore(int delta)
{
	if (delta == 0) return;
	if (cvarManager->getCvar(CVAR_ENABLED).getBoolValue() == false) { LOG("MAH: Ignored Blue score change (disabled)"); return; }
	auto teamsOpt = FindTeams(gameWrapper.get());
	if (!teamsOpt) { LOG("MAH: Could not resolve teams"); return; }
	auto [blue, orange] = *teamsOpt;
	int current = blue.GetScore();
	int next = current + delta;
	if (next < 0) next = 0;
	blue.SetScore(next);
	LOG("MAH: Blue score {} -> {}", current, next);
}

void MatchAdminHotkeys::AdjustOrangeScore(int delta)
{
	if (delta == 0) return;
	if (cvarManager->getCvar(CVAR_ENABLED).getBoolValue() == false) { LOG("MAH: Ignored Orange score change (disabled)"); return; }
	auto teamsOpt = FindTeams(gameWrapper.get());
	if (!teamsOpt) { LOG("MAH: Could not resolve teams"); return; }
	auto [blue, orange] = *teamsOpt;
	int current = orange.GetScore();
	int next = current + delta;
	if (next < 0) next = 0;
	orange.SetScore(next);
	LOG("MAH: Orange score {} -> {}", current, next);
}

void MatchAdminHotkeys::DoPauseToggle()
{
	if (cvarManager->getCvar(CVAR_ENABLED).getBoolValue() == false) { LOG("MAH: Ignored pause toggle (disabled)"); return; }
	if (!gameWrapper || !gameWrapper->IsInGame()) {
		LOG("MAH: Pause toggle skipped (not in game).");
		return;
	}
	ServerWrapper server = gameWrapper->GetCurrentGameState();
	if (!server) {
		LOG("MAH: Pause toggle skipped (no server).");
		return;
	}
	PlayerControllerWrapper pauser = server.GetPauser();
	if (pauser) {
		server.SetPaused(pauser, 0);
		LOG("MAH: Unpaused via ServerWrapper::SetPaused(false).");
		return;
	}
	auto pcOpt = GetLocalPC(gameWrapper.get(), server);
	if (pcOpt.has_value()) {
		server.SetPaused(pcOpt.value(), 1);
		LOG("MAH: Paused via ServerWrapper::SetPaused(true).");
		return;
	}
	std::string userCmds = cvarManager->getCvar(CVAR_PAUSE_CMD).getStringValue();
	if (!userCmds.empty()) {
		ExecMulti(cvarManager, userCmds);
		LOG("MAH: Executed fallback pause cmd(s): {}", userCmds);
	}
	else {
		LOG("MAH: Could not resolve a PlayerController to pause/unpause, and no pause commands configured.");
	}
}

void MatchAdminHotkeys::DoKickoffReset()
{
	if (cvarManager->getCvar(CVAR_ENABLED).getBoolValue() == false) { LOG("MAH: Ignored kickoff reset (disabled)"); return; }
	if (!gameWrapper || !gameWrapper->IsInGame()) {
		LOG("MAH: Kickoff reset skipped (not in game).");
		return;
	}
	ServerWrapper server = gameWrapper->GetCurrentGameState();
	if (!server) {
		LOG("MAH: Kickoff reset skipped (no server).");
		return;
	}
	{
		std::string cmd = cvarManager->getCvar(CVAR_RESET_CMD).getStringValue();
		if (!cmd.empty()) {
			ExecMulti(cvarManager, cmd);
			LOG("MAH: Executed reset cmd(s): {}", cmd);
		}
	}
	server.StartNewRound();
	LOG("MAH: StartNewRound() called for kickoff reset.");
	PlayerControllerWrapper currentPauser = server.GetPauser();
	if (currentPauser) {
		LOG("MAH: Server already paused after StartNewRound().");
		return;
	}
	auto pcOpt = GetLocalPC(gameWrapper.get(), server);
	if (pcOpt.has_value()) {
		server.SetPaused(pcOpt.value(), 1);
		LOG("MAH: Paused after StartNewRound() to await manual unpause.");
		return;
	}
	std::string pauseCmds = cvarManager->getCvar(CVAR_PAUSE_CMD).getStringValue();
	if (!pauseCmds.empty()) {
		ExecMulti(cvarManager, pauseCmds);
		LOG("MAH: Executed fallback post-reset pause cmd(s): {}", pauseCmds);
	}
	else {
		LOG("MAH: Could not resolve PlayerController to pause after reset, and no pause commands configured.");
	}
}

void MatchAdminHotkeys::RenderSettings()
{
	const float leftPadding = 24.f;

	ImGui::Dummy(ImVec2(0.f, 20.f));
	ImGui::Indent(leftPadding);

	bool enabled = cvarManager->getCvar(CVAR_ENABLED).getBoolValue();
	if (ImGui::Checkbox("Enable MatchAdminHotkeys", &enabled))
	{
		cvarManager->getCvar(CVAR_ENABLED).setValue(enabled);
		LOG("MAH: Settings toggled enabled={}", enabled ? "true" : "false");
	}

	ImGui::Dummy(ImVec2(0.f, 16.f));
	ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.f),
		"It's best to avoid binding keys already assigned \n"
		"to actions in Rocket League's control settings.");
	ImGui::Dummy(ImVec2(0.f, 20.f));

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->ChannelsSplit(2);
	dl->ChannelsSetCurrent(1);

	const ImVec2 cardPad = ImVec2(12.f, 12.f);
	const float  cardRounding = 8.f;
	const ImU32  cardBg = ImGui::GetColorU32(ImVec4(0.12f, 0.13f, 0.16f, 1.0f));
	const ImU32  cardBorder = ImGui::GetColorU32(ImVec4(0.18f, 0.19f, 0.22f, 1.0f));
	const ImU32  lineGrey = ImGui::GetColorU32(ImVec4(0.40f, 0.40f, 0.40f, 0.6f));

	const ImVec4 hdrKeybinds = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
	const ImVec4 hdrBlue = ImVec4(0.70f, 0.85f, 1.00f, 1.0f);
	const ImVec4 hdrOrange = ImVec4(1.00f, 0.72f, 0.60f, 1.0f);
	const ImVec4 hdrAdmin = ImVec4(0.70f, 1.00f, 0.60f, 1.0f);

	ImGui::BeginGroup();

	ImGui::PushStyleColor(ImGuiCol_Text, hdrKeybinds);
	ImGui::TextUnformatted("Keybinds");
	ImGui::PopStyleColor();

	float sepY1 = ImGui::GetCursorScreenPos().y;
	ImGui::Dummy(ImVec2(0.f, 6.f));

	const float keyFieldW = 48.f;
	ImGuiStyle& style = ImGui::GetStyle();
	ImVec2 oldPad = style.FramePadding;

	auto drawCharField = [&](const char* label, std::string& val) {
		char buf[2] = { 0,0 };
		if (!val.empty()) buf[0] = val[0];
		ImGui::SetNextItemWidth(keyFieldW);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(oldPad.x + 13.f, oldPad.y + 3.f));
		if (ImGui::InputText(label, buf, sizeof(buf),
			ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_AutoSelectAll))
		{
			val.assign(1, buf[0] ? buf[0] : '\0');
			NormalizeKey(val);
		}
		ImGui::PopStyleVar();
		if (val.size() > 1) { val.resize(1); NormalizeKey(val); }
		};

	float fullW = ImGui::GetContentRegionAvail().x;
	float gap = 24.f;
	float colW = (fullW - 2 * gap) / 3.f;

	ImGui::BeginGroup();
	ImGui::PushStyleColor(ImGuiCol_Text, hdrBlue);
	ImGui::TextUnformatted("Blue");
	ImGui::PopStyleColor();
	ImGui::PushItemWidth(colW * 0.6f);
	drawCharField("Blue +1", ui_key_blue_plus);
	drawCharField("Blue -1", ui_key_blue_minus);
	ImGui::PopItemWidth();
	ImGui::EndGroup();

	ImGui::SameLine(0.f, gap);

	ImGui::BeginGroup();
	ImGui::PushStyleColor(ImGuiCol_Text, hdrOrange);
	ImGui::TextUnformatted("Orange");
	ImGui::PopStyleColor();
	ImGui::PushItemWidth(colW * 0.6f);
	drawCharField("Orange +1", ui_key_orange_plus);
	drawCharField("Orange -1", ui_key_orange_minus);
	ImGui::PopItemWidth();
	ImGui::EndGroup();

	ImGui::SameLine(0.f, gap);

	ImGui::BeginGroup();
	ImGui::PushStyleColor(ImGuiCol_Text, hdrAdmin);
	ImGui::TextUnformatted("Admin");
	ImGui::PopStyleColor();
	ImGui::PushItemWidth(colW * 0.6f);
	drawCharField("Pause / Unpause", ui_key_pause);
	drawCharField("Reset to Kickoff", ui_key_reset);
	ImGui::PopItemWidth();
	ImGui::EndGroup();

	ImGui::Dummy(ImVec2(0.f, 6.f));
	float sepY2 = ImGui::GetCursorScreenPos().y;

	bool dirty = IsDirtyNow();
	bool atDefaultsSaved = IsAtDefaultsSaved();

	auto hasDuplicates = [&]()->std::optional<char> {
		std::set<char> s;
		const char keys[6] = {
			ui_key_blue_plus.empty() ? '\0' : ui_key_blue_plus[0],
			ui_key_blue_minus.empty() ? '\0' : ui_key_blue_minus[0],
			ui_key_orange_plus.empty() ? '\0' : ui_key_orange_plus[0],
			ui_key_orange_minus.empty() ? '\0' : ui_key_orange_minus[0],
			ui_key_pause.empty() ? '\0' : ui_key_pause[0],
			ui_key_reset.empty() ? '\0' : ui_key_reset[0]
		};
		for (char k : keys) {
			if (k == '\0') continue;
			if (!s.insert(k).second) return k;
		}
		return std::nullopt;
		};
	std::optional<char> dupKey = hasDuplicates();
	bool hasDup = dupKey.has_value();

	if (dirty && !unsavedToastShown) {
		gameWrapper->Toast("MatchAdminHotkeys", "You have unsaved changes. Click Save to apply.");
		unsavedToastShown = true;
	}
	if (dirty && !hasDup) {
		ImGui::BulletText("You have unsaved changes.");
	}
	if (hasDup) {
		ImGui::TextColored(ImVec4(1.f, 0.5f, 0.5f, 1.f),
			"Duplicate key: %c is assigned to multiple actions", *dupKey);
	}

	ImGui::Dummy(ImVec2(0, 6));

	const float buttonW = 280.f;
	bool disableSave = (!dirty) || hasDup;
	const char* saveLabel =
		hasDup ? "Save Keybinds (duplicate)" :
		dirty ? "Save Keybinds (unsaved)" :
		"Save Keybinds";

	bool doSave = EmphasizedSaveButton(saveLabel, (dirty && !hasDup), disableSave, ImVec2(buttonW, 0));
	bool doRevert = ButtonMaybeDisabled("Revert Unsaved Changes", !dirty, ImVec2(buttonW, 0));
	bool doReset = ButtonMaybeDisabled("Reset to Defaults", atDefaultsSaved, ImVec2(buttonW, 0));

	ImGui::EndGroup();

	ImVec2 groupMin = ImGui::GetItemRectMin();
	ImVec2 groupMax = ImGui::GetItemRectMax();
	ImVec2 bgMin = ImVec2(groupMin.x - cardPad.x, groupMin.y - cardPad.y);
	ImVec2 bgMax = ImVec2(groupMax.x + cardPad.x, groupMax.y + cardPad.y);

	dl->ChannelsSetCurrent(0);
	dl->AddRectFilled(bgMin, bgMax, cardBg, cardRounding);
	dl->AddRect(bgMin, bgMax, cardBorder, cardRounding, 0, 1.0f);
	dl->ChannelsSetCurrent(1);
	dl->AddLine(ImVec2(groupMin.x, sepY1), ImVec2(groupMax.x, sepY1), lineGrey, 1.0f);
	dl->AddLine(ImVec2(groupMin.x, sepY2), ImVec2(groupMax.x, sepY2), lineGrey, 1.0f);
	dl->ChannelsMerge();

	if (doSave && dirty && !hasDup)
	{
		NormalizeKey(ui_key_blue_plus);
		NormalizeKey(ui_key_blue_minus);
		NormalizeKey(ui_key_orange_plus);
		NormalizeKey(ui_key_orange_minus);
		NormalizeKey(ui_key_pause);
		NormalizeKey(ui_key_reset);

		if (auto dup2 = hasDuplicates()) {
			std::string msg = std::string("Duplicate key: ") + *dup2 + " is assigned to multiple actions";
			gameWrapper->Toast("MatchAdminHotkeys", msg);
			LOG("MAH: Save blocked - %s", msg.c_str());
			ImGui::Unindent(leftPadding);
			return;
		}

		ScrubBindsCfgPhantoms(cvarManager, gameWrapper.get());
		UnbindAllManagedKeys(cvarManager);
		PersistBinds(cvarManager);

		cvarManager->getCvar(CVAR_KEY_BLUE_PLUS).setValue(ui_key_blue_plus);
		cvarManager->getCvar(CVAR_KEY_BLUE_MINUS).setValue(ui_key_blue_minus);
		cvarManager->getCvar(CVAR_KEY_ORANGE_PLUS).setValue(ui_key_orange_plus);
		cvarManager->getCvar(CVAR_KEY_ORANGE_MINUS).setValue(ui_key_orange_minus);
		cvarManager->getCvar(CVAR_KEY_PAUSE).setValue(ui_key_pause);
		cvarManager->getCvar(CVAR_KEY_RESET).setValue(ui_key_reset);

		SaveCfg();

		UnbindAllManagedKeys(cvarManager);
		cvarManager->executeCommand("exec matchadminhotkeys.cfg");
		PersistBinds(cvarManager);

		SnapshotLastSaved();
		gameWrapper->Toast("MatchAdminHotkeys", "Keybinds saved");
		LOG("MAH: Keybinds saved; scrubbed binds.cfg, double-unbound, exec'd cfg, persisted");
	}

	if (doRevert && dirty)
	{
		ui_key_blue_plus = last_blue_plus;
		ui_key_blue_minus = last_blue_minus;
		ui_key_orange_plus = last_orange_plus;
		ui_key_orange_minus = last_orange_minus;
		ui_key_pause = last_pause;
		ui_key_reset = last_reset;
		unsavedToastShown = false;
		LOG("MAH: Reverted fields from last saved snapshot");
	}

	if (doReset && !atDefaultsSaved)
	{
		ui_key_blue_plus = "U";
		ui_key_blue_minus = "J";
		ui_key_orange_plus = "I";
		ui_key_orange_minus = "K";
		ui_key_pause = "P";
		ui_key_reset = "O";

		ScrubBindsCfgPhantoms(cvarManager, gameWrapper.get());
		UnbindAllManagedKeys(cvarManager);
		PersistBinds(cvarManager);

		cvarManager->getCvar(CVAR_KEY_BLUE_PLUS).setValue(ui_key_blue_plus);
		cvarManager->getCvar(CVAR_KEY_BLUE_MINUS).setValue(ui_key_blue_minus);
		cvarManager->getCvar(CVAR_KEY_ORANGE_PLUS).setValue(ui_key_orange_plus);
		cvarManager->getCvar(CVAR_KEY_ORANGE_MINUS).setValue(ui_key_orange_minus);
		cvarManager->getCvar(CVAR_KEY_PAUSE).setValue(ui_key_pause);
		cvarManager->getCvar(CVAR_KEY_RESET).setValue(ui_key_reset);

		SaveCfg();

		UnbindAllManagedKeys(cvarManager);
		cvarManager->executeCommand("exec matchadminhotkeys.cfg");
		PersistBinds(cvarManager);

		SnapshotLastSaved();
		gameWrapper->Toast("MatchAdminHotkeys", "Defaults restored");
		LOG("MAH: Defaults restored; scrubbed binds.cfg, double-unbound, exec'd cfg, persisted");
	}

	ImGui::Unindent(leftPadding);
}


std::string MatchAdminHotkeys::GetPluginName()
{
	return "MatchAdminHotkeys";
}

void MatchAdminHotkeys::SetImGuiContext(uintptr_t ctx)
{
	ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

void MatchAdminHotkeys::LoadKeyCvarsToUi()
{
	ui_key_blue_plus = cvarManager->getCvar(CVAR_KEY_BLUE_PLUS).getStringValue();
	ui_key_orange_plus = cvarManager->getCvar(CVAR_KEY_ORANGE_PLUS).getStringValue();
	ui_key_blue_minus = cvarManager->getCvar(CVAR_KEY_BLUE_MINUS).getStringValue();
	ui_key_orange_minus = cvarManager->getCvar(CVAR_KEY_ORANGE_MINUS).getStringValue();
	ui_key_pause = cvarManager->getCvar(CVAR_KEY_PAUSE).getStringValue();
	ui_key_reset = cvarManager->getCvar(CVAR_KEY_RESET).getStringValue();

	NormalizeKey(ui_key_blue_plus);
	NormalizeKey(ui_key_blue_minus);
	NormalizeKey(ui_key_orange_plus);
	NormalizeKey(ui_key_orange_minus);
	NormalizeKey(ui_key_pause);
	NormalizeKey(ui_key_reset);
}

void MatchAdminHotkeys::SaveCfg()
{
	if (!gameWrapper) return;

	std::filesystem::path bmPath = gameWrapper->GetBakkesModPath();
	std::filesystem::path cfgPath = bmPath / "cfg" / "matchadminhotkeys.cfg";

	std::string pauseCmd = cvarManager->getCvar(CVAR_PAUSE_CMD).getStringValue();
	std::string resetCmd = cvarManager->getCvar(CVAR_RESET_CMD).getStringValue();

	std::ofstream f(cfgPath, std::ios::trunc);
	if (!f.is_open())
	{
		LOG("MAH: Failed to open cfg for writing: {}", cfgPath.string());
		return;
	}

	f << "// MatchAdminHotkeys configuration\n";
	f << "// Single-letter keys only.\n";
	f << "// To change keys: edit the letters below or use the plugin's settings UI, then Save.\n";
	f << "// The unbind section clears A-Z (upper/lower) and legacy keys to prevent duplicates.\n";
	f << "// It's best to avoid binding keys already assigned to actions in Rocket League's control settings.\n";
	f << "\n";

	f << "alias mah_unbind_all \"";
	for (char c = 'A'; c <= 'Z'; ++c)
	{
		f << "unbind " << c << ";unbind " << static_cast<char>(c + 32) << ";";
	}
	f << "unbind Slash;";
	f << "\"\n";

	f << "alias mah_bind_letters \"mah_unbind_all;"
		<< "bind " << ui_key_blue_plus << " " << NOTI_BLUE_PLUS << ";"
		<< "bind " << ui_key_blue_minus << " " << NOTI_BLUE_MINUS << ";"
		<< "bind " << ui_key_orange_plus << " " << NOTI_ORANGE_PLUS << ";"
		<< "bind " << ui_key_orange_minus << " " << NOTI_ORANGE_MINUS << ";"
		<< "bind " << ui_key_pause << " " << NOTI_PAUSE_TOGGLE << ";"
		<< "bind " << ui_key_reset << " " << NOTI_RESET
		<< "\"\n";

	f << "mah_bind_letters\n";
	f << CVAR_PAUSE_CMD << " \"" << pauseCmd << "\"\n";
	f << CVAR_RESET_CMD << " \"" << resetCmd << "\"\n";

	f.close();
	LOG("MAH: Wrote cfg to {}", cfgPath.string());
}
