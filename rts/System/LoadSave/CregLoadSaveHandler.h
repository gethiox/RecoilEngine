/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef CREG_LOAD_SAVE_HANDLER_H
#define CREG_LOAD_SAVE_HANDLER_H

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
	static bool WriteStateForComparison(const std::string& basePath);

protected:
	std::stringstream iss;
};

#endif // CREG_LOAD_SAVE_HANDLER_H
