/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef SIM_STATE_TEST_HOOKS_H
#define SIM_STATE_TEST_HOOKS_H

#include <cstdint>

/**
 * @brief Test-only instrumentation used to verify simulation-state integrity.
 *
 * Every hook is inert unless explicitly enabled through a SimStateTest* config
 * variable (normally passed via an exclusive --config file by the test
 * harness in test/snapshotjoin/), so regular games are never affected.
 *
 * Supported config variables:
 *  - SimStateTestChecksumLog: path of a text file receiving one
 *    "<frame> <checksum>" line per simulated frame
 *  - SimStateTestSaveFrame:   save the game right after this frame was simulated
 *  - SimStateTestSaveName:    name of that save (written to Saves/<name>.ssf)
 *  - SimStateTestQuitFrame:   quit right after this frame was simulated
 *  - SimStateTestDumpFrames:  "<first> <last> <period>", DumpState() range
 *  - SimStateTestGameSpeed:   request this user speed once the sim runs
 *    (requires a matching MaxSpeed modoption in the start script)
 *  - SimStateTestDisableLuaUI: do not load LuaUI (its widgets issue orders at
 *    wall-clock dependent frames, which makes independent runs incomparable)
 */
namespace SimStateTestHooks {
	void Init();
	void Kill();

	bool IsLuaUIDisabled();

	/// called once a saved game state (taken after frame <frameNum>) was fully loaded
	void PostLoadGame(int frameNum);

	/// called by the client right after frame <frameNum> was simulated
	void PostSimFrame(int frameNum, uint32_t syncChecksum);
}

#endif // SIM_STATE_TEST_HOOKS_H
