/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef CREG_LOAD_SAVE_HANDLER_H
#define CREG_LOAD_SAVE_HANDLER_H

#include <cstdint>
#include <string>
#include <sstream>
#include "LoadSaveHandler.h"

class CCregLoadSaveHandler : public ILoadSaveHandler
{
public:
	CCregLoadSaveHandler();
	~CCregLoadSaveHandler();

	bool LoadGameStartInfo(const std::string& path) override;
	void LoadGame() override;
	void LoadAIData() override;
	void SaveGame(const std::string& path) override;

	/**
	 * Test support: write the uncompressed game state plus a map of which
	 * class member produced which bytes to <basePath>.state / .layout, so
	 * two states can be compared member by member.
	 */
	static bool WriteStateForComparison(const std::string& basePath, bool snapshotMode = false);

	/// writes a game snapshot (see GameSnapshot.h), uncompressed
	static bool WriteSnapshot(std::string& raw);

	/**
	 * Prepares loading a snapshot of the game this client is joining. Unlike
	 * a saved game, loading a snapshot keeps the local client's identity
	 * (player, team, spectator state) and unsynced settings.
	 */
	bool LoadSnapshot(std::string&& raw, int32_t& frameNum);
	bool IsSnapshot() const { return joinMode; }

protected:
	std::stringstream iss;
	bool joinMode = false;
};

#endif // CREG_LOAD_SAVE_HANDLER_H
