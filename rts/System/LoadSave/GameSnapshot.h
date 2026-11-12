/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef GAME_SNAPSHOT_H
#define GAME_SNAPSHOT_H

#include <array>
#include <cstdint>
#include <string>

/**
 * @brief In-memory game state snapshots used to join a running game.
 *
 * A snapshot is taken by an already synced client right after it simulated a
 * given frame and contains the complete synced state (the same data as a
 * creg saved game). A joining client loads it instead of re-simulating the
 * game from its start, see doc/snapshot-join.md.
 *
 * Snapshots exclude client specific (unsynced) data, so that equal synced
 * states produce equal snapshots; this allows verifying a snapshot against
 * the hashes of other clients.
 */
namespace GameSnapshot {
	using Digest = std::array<uint8_t, 32>;

	struct Header {
		std::string syncVersion;
		std::string setupText;
		std::string modName;
		std::string mapName;
		int32_t frameNum = -1;
	};

	static constexpr const char* MAGIC = "RECOIL-GAME-SNAPSHOT-1";

	/// maximum accepted size of an uncompressed snapshot
	static constexpr size_t MAX_RAW_SIZE = size_t(2) << 30;

	/// serialize the current state; only valid between two simulation frames
	bool Write(std::string& raw);

	/// parses the header of a raw snapshot; data starts at the returned offset (0 on error)
	size_t ReadHeader(const std::string& raw, Header& header);

	bool Compress(const std::string& raw, std::string& compressed, int level);
	bool Decompress(const std::string& compressed, std::string& raw, size_t maxRawSize = MAX_RAW_SIZE);

	Digest Hash(const std::string& raw);
	std::string DigestToHex(const Digest& d);

	/// true while a snapshot is written or loaded (excludes unsynced data)
	bool IsActive();

	struct ScopedActivation {
		ScopedActivation();
		~ScopedActivation();
		ScopedActivation(const ScopedActivation&) = delete;
		ScopedActivation& operator=(const ScopedActivation&) = delete;
	};
}

#endif
