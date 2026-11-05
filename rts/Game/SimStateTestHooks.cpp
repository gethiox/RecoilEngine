/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SimStateTestHooks.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "Game/GlobalUnsynced.h"
#include "Game/SimStateLuaDump.h"
#include "Lua/LuaGaia.h"
#include "Lua/LuaRules.h"
#include "Net/Protocol/BaseNetProtocol.h"
#include "Net/Protocol/NetProtocol.h"
#include "System/Config/ConfigHandler.h"
#include "System/FileSystem/FileSystem.h"
#include "System/LoadSave/CregLoadSaveHandler.h"
#include "System/LoadSave/GameSnapshot.h"
#include "System/LoadSave/LoadSaveHandler.h"
#include "System/Log/ILog.h"
#include "System/Misc/SpringTime.h"
#include "System/FileSystem/SimpleParser.h"
#include "System/StringUtil.h"
#include "System/Sync/DumpState.h"

CONFIG(std::string, SimStateTestChecksumLog).defaultValue("").description("[test] write per-frame sync checksums to this file");
CONFIG(int, SimStateTestSaveFrame).defaultValue(-1).description("[test] save the game right after this frame");
CONFIG(std::string, SimStateTestSaveName).defaultValue("simstatetest").description("[test] save name used with SimStateTestSaveFrame");
CONFIG(bool, SimStateTestSaveSnapshot).defaultValue(false).description("[test] SimStateTestSaveFrame writes a raw game snapshot (Saves/<name>.srs) instead of a saved game");
CONFIG(int, SimStateTestQuitFrame).defaultValue(-1).description("[test] quit right after this frame");
CONFIG(int, SimStateTestCompareFrame).defaultValue(-1).description("[test] write the full game state for comparison after this frame");
CONFIG(bool, SimStateTestCompareSnapshot).defaultValue(false).description("[test] compare-state files contain exactly the snapshot data");
CONFIG(bool, SimStateTestDumpJoinSnapshots).defaultValue(false).description("[test] write every snapshot this client provides for a joining client as comparison state");
CONFIG(float, SimStateTestGameSpeed).defaultValue(0.0f).description("[test] request this game speed once the simulation runs");
CONFIG(int, SimStateTestPauseFrame).defaultValue(-1).description("[test] pause the game right after this frame");
CONFIG(float, SimStateTestPauseSeconds).defaultValue(30.0f).description("[test] unpause the game this many seconds after SimStateTestPauseFrame paused it");
CONFIG(bool, SimStateTestDisableLuaUI).defaultValue(false).description("[test] do not load LuaUI");
CONFIG(std::string, SimStateTestDumpFrames).defaultValue("").description("[test] \"first last period\" frames passed to DumpState");
CONFIG(std::string, SimStateTestDumpRNGFrames).defaultValue("").description("[test] \"first last\" frames passed to DumpRNG (every synced random number drawn)");

namespace {
	FILE* checksumLog = nullptr;
	int saveFrame = -1;
	int quitFrame = -1;
	float gameSpeed = 0.0f;
	int pauseFrame = -1;
	spring_time unpauseTime;
	bool pausedByHook = false;
	bool dumpState = false;
	int compareFrame = -1;

	void WriteCompareState(const std::string& basePath, bool snapshotMode)
	{
		if (CCregLoadSaveHandler::WriteStateForComparison(basePath, snapshotMode))
			LOG("[SimStateTestHooks] wrote comparison state %s", basePath.c_str());

		// human readable view of the synced Lua heaps (after the full GC done above)
		if (luaRules != nullptr)
			SimStateLuaDump::DumpHeap(luaRules->syncedLuaHandle.GetLuaState(), basePath + ".luarules.txt");
		if (luaGaia != nullptr)
			SimStateLuaDump::DumpHeap(luaGaia->syncedLuaHandle.GetLuaState(), basePath + ".luagaia.txt");
	}
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
	compareFrame = configHandler->GetInt("SimStateTestCompareFrame");
	pauseFrame = configHandler->GetInt("SimStateTestPauseFrame");
	pausedByHook = false;

	if (!logPath.empty()) {
		if ((checksumLog = fopen(logPath.c_str(), "w")) == nullptr)
			LOG_L(L_ERROR, "[SimStateTestHooks] could not open checksum log \"%s\"", logPath.c_str());
	}

	const std::vector<std::string> dumpArgs = CSimpleParser::Tokenize(configHandler->GetString("SimStateTestDumpFrames"));

	if ((dumpState = (dumpArgs.size() == 3)))
		DumpState(StringToInt(dumpArgs[0]), StringToInt(dumpArgs[1]), StringToInt(dumpArgs[2]), true, std::nullopt, true);

	const std::vector<std::string> rngArgs = CSimpleParser::Tokenize(configHandler->GetString("SimStateTestDumpRNGFrames"));

	if (rngArgs.size() == 2)
		DumpRNG(StringToInt(rngArgs[0]), StringToInt(rngArgs[1]), true);
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

	// an RNG dump starting at this frame also covers what follows the load
	DumpRNG(-1, -1);

	// dump the freshly loaded state if requested, so it can be compared with
	// the reference state recorded at the end of the same frame
	if (dumpState)
		DumpState(-1, -1, -1, std::nullopt, std::nullopt, true);

	if (frameNum == compareFrame)
		WriteCompareState("compare-" + IntToString(frameNum), configHandler->GetBool("SimStateTestCompareSnapshot"));

	// a client that joined from a snapshot: its state right after loading
	if (configHandler->GetBool("SimStateTestDumpJoinSnapshots"))
		WriteCompareState("loaded-" + IntToString(frameNum), true);
}

void SimStateTestHooks::PostSimFrame(int frameNum, uint32_t syncChecksum)
{
	if (gameSpeed > 0.0f) {
		clientNet->Send(CBaseNetProtocol::Get().SendUserSpeed(gu->myPlayerNum, gameSpeed));
		gameSpeed = 0.0f;
	}

	if (checksumLog != nullptr) {
		// test harnesses follow the log while the game runs
		fprintf(checksumLog, "%d %08x\n", frameNum, syncChecksum);
		fflush(checksumLog);
	}

	// DumpState() itself only runs from SimFrame with cheats enabled
	if (dumpState)
		DumpState(-1, -1, -1, std::nullopt, std::nullopt, true);

	// before saving, so both see the same (garbage-collected) Lua state
	if (frameNum == compareFrame)
		WriteCompareState("compare-" + IntToString(frameNum), configHandler->GetBool("SimStateTestCompareSnapshot"));

	if (frameNum == saveFrame) {
		LOG("[SimStateTestHooks] saving game at frame %d", frameNum);

		if (configHandler->GetBool("SimStateTestSaveSnapshot")) {
			std::string raw;

			if (FileSystem::CreateDirectory("Saves") && GameSnapshot::Write(raw))
				std::ofstream("Saves/" + saveName + ".srs", std::ios::binary).write(raw.data(), raw.size());
		} else {
			ILoadSaveHandler::CreateSave("Saves/" + saveName + ".ssf", "-y");
		}
	}

	if (frameNum == pauseFrame) {
		LOG("[SimStateTestHooks] pausing at frame %d", frameNum);
		clientNet->Send(CBaseNetProtocol::Get().SendPause(gu->myPlayerNum, true));
		unpauseTime = spring_gettime() + spring_msecs(int(configHandler->GetFloat("SimStateTestPauseSeconds") * 1000.0f));
		pausedByHook = true;
	}

	if (frameNum == quitFrame) {
		LOG("[SimStateTestHooks] quitting at frame %d", frameNum);
		Kill();
		gu->globalQuit = true;
	}
}

void SimStateTestHooks::SnapshotCreated(int frameNum, const std::string& snapshot)
{
	if (!configHandler->GetBool("SimStateTestDumpJoinSnapshots"))
		return;

	const std::string basePath = "snapshot-" + IntToString(frameNum);

	// the snapshot as sent, and the same state with its member layout
	std::ofstream(basePath + ".raw", std::ios::binary).write(snapshot.data(), snapshot.size());
	WriteCompareState(basePath, true);
}

void SimStateTestHooks::Update()
{
	if (!pausedByHook || spring_gettime() < unpauseTime)
		return;

	LOG("[SimStateTestHooks] unpausing");
	clientNet->Send(CBaseNetProtocol::Get().SendPause(gu->myPlayerNum, false));
	pausedByHook = false;
}
