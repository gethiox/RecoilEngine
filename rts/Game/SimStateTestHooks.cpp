/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SimStateTestHooks.h"

#include <cstdio>
#include <string>

#include "Game/GlobalUnsynced.h"
#include "Net/Protocol/BaseNetProtocol.h"
#include "Net/Protocol/NetProtocol.h"
#include "System/Config/ConfigHandler.h"
#include "System/LoadSave/LoadSaveHandler.h"
#include "System/Log/ILog.h"
#include "System/FileSystem/SimpleParser.h"
#include "System/StringUtil.h"
#include "System/Sync/DumpState.h"

CONFIG(std::string, SimStateTestChecksumLog).defaultValue("").description("[test] write per-frame sync checksums to this file");
CONFIG(int, SimStateTestSaveFrame).defaultValue(-1).description("[test] save the game right after this frame");
CONFIG(std::string, SimStateTestSaveName).defaultValue("simstatetest").description("[test] save name used with SimStateTestSaveFrame");
CONFIG(int, SimStateTestQuitFrame).defaultValue(-1).description("[test] quit right after this frame");
CONFIG(float, SimStateTestGameSpeed).defaultValue(0.0f).description("[test] request this game speed once the simulation runs");
CONFIG(bool, SimStateTestDisableLuaUI).defaultValue(false).description("[test] do not load LuaUI");
CONFIG(std::string, SimStateTestDumpFrames).defaultValue("").description("[test] \"first last period\" frames passed to DumpState");

namespace {
	FILE* checksumLog = nullptr;
	int saveFrame = -1;
	int quitFrame = -1;
	float gameSpeed = 0.0f;
	bool dumpState = false;
	std::string saveName;
}

void SimStateTestHooks::Init()
{
	Kill();

	const std::string logPath = configHandler->GetString("SimStateTestChecksumLog");
	saveFrame = configHandler->GetInt("SimStateTestSaveFrame");
	quitFrame = configHandler->GetInt("SimStateTestQuitFrame");
	saveName = configHandler->GetString("SimStateTestSaveName");
	gameSpeed = configHandler->GetFloat("SimStateTestGameSpeed");

	if (!logPath.empty()) {
		if ((checksumLog = fopen(logPath.c_str(), "w")) == nullptr)
			LOG_L(L_ERROR, "[SimStateTestHooks] could not open checksum log \"%s\"", logPath.c_str());
	}

	const std::vector<std::string> dumpArgs = CSimpleParser::Tokenize(configHandler->GetString("SimStateTestDumpFrames"));

	if ((dumpState = (dumpArgs.size() == 3)))
		DumpState(StringToInt(dumpArgs[0]), StringToInt(dumpArgs[1]), StringToInt(dumpArgs[2]), true, std::nullopt, true);
}

void SimStateTestHooks::Kill()
{
	if (checksumLog != nullptr)
		fclose(checksumLog);

	checksumLog = nullptr;
}

bool SimStateTestHooks::IsLuaUIDisabled()
{
	return configHandler->GetBool("SimStateTestDisableLuaUI");
}

void SimStateTestHooks::PostLoadGame(int frameNum)
{
	LOG("[SimStateTestHooks] loaded game state of frame %d", frameNum);

	// dump the freshly loaded state if requested, so it can be compared with
	// the reference state recorded at the end of the same frame
	if (dumpState)
		DumpState(-1, -1, -1, std::nullopt, std::nullopt, true);
}

void SimStateTestHooks::PostSimFrame(int frameNum, uint32_t syncChecksum)
{
	if (gameSpeed > 0.0f) {
		clientNet->Send(CBaseNetProtocol::Get().SendUserSpeed(gu->myPlayerNum, gameSpeed));
		gameSpeed = 0.0f;
	}

	if (checksumLog != nullptr) {
		fprintf(checksumLog, "%d %08x\n", frameNum, syncChecksum);

		if (frameNum == quitFrame)
			fflush(checksumLog);
	}

	// DumpState() itself only runs from SimFrame with cheats enabled
	if (dumpState)
		DumpState(-1, -1, -1, std::nullopt, std::nullopt, true);

	if (frameNum == saveFrame) {
		LOG("[SimStateTestHooks] saving game at frame %d", frameNum);
		ILoadSaveHandler::CreateSave("Saves/" + saveName + ".ssf", "-y");
	}

	if (frameNum == quitFrame) {
		LOG("[SimStateTestHooks] quitting at frame %d", frameNum);
		Kill();
		gu->globalQuit = true;
	}
}
