/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _GAME_PARTICIPANT_H
#define _GAME_PARTICIPANT_H

#include <memory>

#include "Game/Players/PlayerBase.h"
#include "Game/Players/PlayerStatistics.h"
#include "System/Net/LoopbackConnection.h"
#include "System/UnorderedMap.hpp"
#include "System/Misc/SpringTime.h"

namespace netcode
{
	class CConnection;
	class RawPacket;
}

class GameParticipant : public PlayerBase
{
public:
	GameParticipant();
	~GameParticipant();

	void SendData(std::shared_ptr<const netcode::RawPacket> packet);
	void Connected(std::shared_ptr<netcode::CConnection> link, bool local);
	void Kill(const std::string& reason, const bool flush = false);

	void CheckForExpiredConnection();

	GameParticipant& operator=(const PlayerBase& base) { PlayerBase::operator=(base); return *this; };

public:
	int id = -1;
	int lastFrameResponse = 0;

	enum State {
		UNCONNECTED,
		CONNECTED,
		INGAME,
		DISCONNECTING,
		DISCONNECTED
	};
	State myState = UNCONNECTED;

	bool isLocal = false;
	bool isReconn = false;
	bool isMidgameJoin = false;
	/// admitted without a password (AllowSpectatorJoin): not trusted as a snapshot source
	bool unauthenticated = false;

	/// joining via a game snapshot which is still being prepared; the client
	/// gets no game packets until it was sent the snapshot (or told to catch up)
	bool awaitingSnapshot = false;
	/// frames before this one are covered by the client's snapshot (no sync responses)
	int syncCheckStartFrame = 0;
	/// joined from a snapshot and still simulating frames the server has
	/// evaluated already: their checksums are compared as they arrive
	/// (CGameServer::lateSyncReference); the number verified so far, -1 if not
	int lateSyncChecks = -1;
	/// frames of that catch-up that had no agreed checksum left to compare with
	int lateSyncMisses = 0;
	/// the last frame this client sent a sync checksum for (lastFrameResponse
	/// also follows keyframe acknowledgements, which run ahead of the simulation while catching up)
	int lastSyncResponseFrame = 0;

	PlayerStatistics lastStats;

	spring_time disconnectDelay;

	struct ClientLinkData {
		ClientLinkData(bool connect = true) {
			if (connect)
				link.reset(new netcode::CLoopbackConnection());
		}

		std::shared_ptr<netcode::CConnection> link;

		int bandwidthUsage = 0;
		int numPacketsSent = 0;
	};

	std::shared_ptr<netcode::CConnection> clientLink;
	spring::unordered_map<uint8_t, ClientLinkData> aiClientLinks;

	#ifdef SYNCCHECK
	spring::unordered_map<int, unsigned int> syncResponse; // syncResponse[frameNum] = checksum
	#endif

private:
	void CloseConnection(bool flush);
};

#endif // _GAME_PARTICIPANT_H
