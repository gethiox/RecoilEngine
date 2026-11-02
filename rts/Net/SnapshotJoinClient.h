/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef SNAPSHOT_JOIN_CLIENT_H
#define SNAPSHOT_JOIN_CLIENT_H

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>

class ILoadSaveHandler;
class CGame;

namespace netcode {
	class RawPacket;
}

/**
 * @brief Client side of joining a running game from a snapshot.
 *
 * Source role: when the server assigned this client a role for a request,
 * the client snapshots the game when processing that request's marker in
 * the game stream, then compresses and hashes it in the background and
 * uploads it (provider) or only its digest (verifier).
 *
 * Joining role: the snapshot arrives while connecting (PreGame), is verified
 * against the digest and loaded instead of starting a new game. Afterwards
 * the game stream continues at the marker the snapshot was taken at. The
 * game history before the marker arrives in the background while playing; it
 * is not simulated, only inserted into the local demo (at the marker).
 *
 * See CSnapshotJoinServer and doc/snapshot-join.md.
 */
class CSnapshotJoinClient
{
public:
	using Digest = std::array<uint8_t, 32>;

	/// whether this client asks the server for a snapshot when joining a running
	/// game: the start script's GAME\SnapshotJoin if set (>= 0), else the config
	static bool WantsSnapshotJoin(int scriptValue);

	// --- joining client ---

	/// NETMSG_SNAPSHOT_{DATA,HASH} received while connecting
	void HandlePreGamePacket(std::shared_ptr<const netcode::RawPacket> packet);

	/// the verified snapshot as load handler (once), or nullptr
	ILoadSaveHandler* TakeLoadHandler();

	/// called before reading game packets; starts a game loaded from a snapshot
	void Start(CGame* game);

	/// NETMSG_SNAPSHOT_HISTORY: the game history before the snapshot point (for the demo)
	void HandleHistory(std::shared_ptr<const netcode::RawPacket> packet, CGame* game);

	// --- source client ---

	/// NETMSG_SNAPSHOT_REQUEST in game: role assignment or the snapshot marker
	void HandleRequest(std::shared_ptr<const netcode::RawPacket> packet);
	/// NETMSG_SNAPSHOT_HASH in game: the server aborted a request
	void HandleServerHash(std::shared_ptr<const netcode::RawPacket> packet);

	/// sends the digest of a created snapshot, starts and finishes uploads; called every client update
	void Update();

	/// stops everything (an upload in progress), forgets all state; called when the game ends
	void Reset();

private:
	void CreateSnapshot(uint8_t requestID, uint8_t role, int32_t markerFrame);
	void SendHash(uint8_t requestID, uint8_t status, int32_t frameNum, uint32_t compressedSize, const Digest& digest);

	/// the snapshot's own marker in the game stream (joining client)
	void HandleJoinMarker(int32_t markerFrame);
	void AddHistoryPacket(const uint8_t* data, uint32_t length, CGame* game);

private:
	// joining
	struct Incoming {
		uint8_t requestID = 0;
		std::string data;
	} incoming;

	std::unique_ptr<ILoadSaveHandler> loadHandler;
	uint8_t joinRequestID = 0;
	int32_t joinMarkerFrame = -1;

	enum class JoinState { NONE, LOADING, PLAYING } joinState = JoinState::NONE;

	struct History {
		uint32_t expectedSize = 0;
		uint32_t receivedSize = 0;
		std::string pending;    // received, not yet complete packets
		int32_t frameNum = -1;   // gs->frameNum when the next packet would have been received live
		uint32_t numPackets = 0;
		bool markerSeen = false; // demo insertion point known
		bool complete = false;
	} history;

	// source
	struct Upload {
		uint8_t requestID = 0;
		uint8_t role = 0;
		uint8_t playerNum = 0;
		int32_t frameNum = -1;

		struct Result {
			std::string compressed;
			Digest digest = {};
			bool ok = false;
		};

		std::future<Result> pending;
		Result result;
		bool resultSent = false;

		// the provider uploads in a thread, independent of the frame rate
		std::thread thread;
		std::atomic<bool> stop = {false};
		std::atomic<bool> done = {false};
		uint32_t offset = 0;
	};

	static void UploadLoop(Upload& u);
	void StopUpload();

	std::map<uint8_t, uint8_t> assignedRoles;
	std::unique_ptr<Upload> upload;
};

extern CSnapshotJoinClient snapshotJoinClient;

#endif // SNAPSHOT_JOIN_CLIENT_H
