/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef SNAPSHOT_JOIN_SERVER_H
#define SNAPSHOT_JOIN_SERVER_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "System/Misc/SpringTime.h"

class CGameServer;
class GameParticipant;

namespace netcode {
	class RawPacket;
}

/**
 * @brief Server side of joining a running game from a snapshot.
 *
 * Instead of letting a (re)joining client re-simulate the whole game, the
 * server asks up to N synced clients ("sources") for a snapshot of the game
 * state at a common point of the game stream, verifies that their digests
 * agree and relays the snapshot to the joining client. A join then passes
 * through three phases:
 *
 *  1. SNAPSHOT: the joiner is held back (no game packets) while the snapshot
 *     is created, verified and relayed.
 *  2. CATCHUP: the joiner gets the game packets from the snapshot point on,
 *     still held back from live broadcasts until it has all of them.
 *  3. HISTORY: the joiner plays live; the game history before the snapshot
 *     point, which it only needs for its demo, is streamed in the background.
 *
 * The snapshot point is a marker packet (NETMSG_SNAPSHOT_REQUEST with role
 * SNAPSHOT_ROLE_MARKER) broadcast into the game stream: sources snapshot
 * exactly when they process it and the joiner continues exactly after it,
 * which is well defined at any point of the stream (also while paused).
 *
 * Every failure (no sources, timeouts, mismatching digests, disconnects)
 * falls back to the classic catch-up, so a join never fails because of it.
 * See doc/snapshot-join.md for the complete protocol.
 */
class CSnapshotJoinServer
{
public:
	explicit CSnapshotJoinServer(CGameServer* server);
	~CSnapshotJoinServer();

	/**
	 * Decides whether a client connecting to a running game may join via
	 * snapshot; if so the participant is held back (awaitingSnapshot) and
	 * a snapshot is requested.
	 * @return true if the join is now handled by this class
	 */
	bool TryBegin(GameParticipant& joiner, bool clientWantsSnapshot);

	/// NETMSG_SNAPSHOT_{DATA,HASH} sent by a client
	void ProcessPacket(int playerNum, std::shared_ptr<const netcode::RawPacket> packet);

	/// relays data, handles timeouts
	void Update();

	/// a participant disconnected (source or joiner)
	void PlayerLeft(int playerNum);

	bool IsSource(int playerNum) const;

private:
	using Digest = std::array<uint8_t, 32>;

	struct Source {
		int playerNum = -1;
		uint8_t role = 0;
		uint8_t status = 0xFF; // SNAPSHOT_STATUS_* once the digest arrived
		int32_t frameNum = -1;
		uint32_t compressedSize = 0;
		Digest digest = {};
	};

	enum class Phase { SNAPSHOT, CATCHUP, HISTORY };

	struct Join {
		int joinerNum = -1;
		uint8_t requestID = 0;
		int32_t markerFrame = -1;
		Phase phase = Phase::SNAPSHOT;

		/// index of the marker in CGameServer::packetCache
		size_t markerIndex = 0;
		/// next packetCache index sent to the joiner while catching up
		size_t catchupIndex = 0;

		/// history before the marker: [uint32 length][packet] for each packet
		uint32_t historySize = 0;
		uint32_t historyOffset = 0;
		size_t historyIndex = 0;   // next packetCache index serialized into historyBuffer
		std::string historyBuffer; // serialized but not yet sent

		std::vector<Source> sources;
		int providerNum = -1;

		std::string data;          // compressed snapshot received from the provider
		uint32_t dataReceived = 0;
		uint32_t dataTotal = 0;     // announced by the provider
		uint32_t relayOffset = 0;  // bytes forwarded to the joiner

		bool verified = false;
		spring_time startTime;
		spring_time progressTime; // last time a digest or data arrived or data was relayed or sent
		spring_time stallLogTime;
	};

	std::vector<int> SelectSources(const GameParticipant& joiner) const;
	bool HostsNativeAI(int playerNum) const;

	Join* FindJoin(uint8_t requestID);
	void OnData(Join& join, int playerNum, std::shared_ptr<const netcode::RawPacket> packet);
	void OnHash(Join& join, int playerNum, std::shared_ptr<const netcode::RawPacket> packet);
	void Verify(Join& join);
	bool Relay(Join& join);
	bool SendCatchup(Join& join);
	bool SendHistory(Join& join);

	/// snapshot sent and verified: tell the joiner, continue with CATCHUP
	void Succeed(Join& join);
	/// gives up on the snapshot: the joiner catches up the classic way
	void Abort(Join& join, const std::string& reason);
	void AbortSources(const Join& join);
	void Remove(Join& join);

private:
	CGameServer* server;
	std::vector<Join> joins;
	uint8_t nextRequestID = 1;
};

#endif // SNAPSHOT_JOIN_SERVER_H
