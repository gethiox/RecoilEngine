/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SnapshotJoinServer.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <random>

#include "GameServer.h"
#include "GameParticipant.h"
#include "GameSkirmishAI.h"
#include "Game/GameSetup.h"
#include "Net/Protocol/BaseNetProtocol.h"
#include "Net/Protocol/NetMessageTypes.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/Net/Connection.h"
#include "System/Net/UnpackPacket.h"
#include "System/SpringFormat.h"
#include "System/StringUtil.h"

CONFIG(bool, AllowSnapshotJoin).defaultValue(true)
	.description("Server: let (re)joining clients load a snapshot of the running game provided by other clients instead of re-simulating it.");
CONFIG(int, SnapshotJoinSources).defaultValue(3).minimumValue(1).maximumValue(8)
	.description("Server: number of clients asked for a snapshot (one provides it, the others verify its digest).");
CONFIG(int, SnapshotJoinMinSources).defaultValue(1).minimumValue(1).maximumValue(8)
	.description("Server: minimum number of clients that have to provide or confirm a snapshot, otherwise the joining client re-simulates the game.");
CONFIG(int, SnapshotJoinTimeout).defaultValue(60).minimumValue(5)
	.description("Server: seconds a snapshot join may make no progress (no digest or data received or relayed) before falling back to re-simulating the game.");
CONFIG(int, SnapshotTransferRate).defaultValue(4 * 1024 * 1024).minimumValue(64 * 1024)
	.description("Bytes per second used for transferring snapshots and the game history to a joining client.");

// Bulk data is queued on a link only up to these limits (see CConnection::
// GetOutgoingQueueBytes), the link sends it at SnapshotTransferRate. While the
// joiner waits for the snapshot nothing else is sent to it; the history is
// sent while it plays, so live packets must never wait long behind it.
static constexpr size_t RELAY_QUEUE_LIMIT = 512 * 1024;
static constexpr size_t HISTORY_QUEUE_LIMIT = 64 * 1024;
static constexpr uint32_t RELAY_CHUNK_SIZE = 8 * 1024;
static constexpr uint32_t MAX_SNAPSHOT_SIZE = 1024u * 1024u * 1024u;
// sources further behind would make the joiner wait for them to reach the marker
static constexpr int MAX_SOURCE_LAG = 30 * GAME_SPEED;


CSnapshotJoinServer::CSnapshotJoinServer(CGameServer* s): server(s) {}
CSnapshotJoinServer::~CSnapshotJoinServer() = default;


bool CSnapshotJoinServer::HostsNativeAI(int playerNum) const
{
	// the state of a native AI only exists on its host, a snapshot cannot provide it
	const auto& setupAIs = server->myGameSetup->GetAIStartingDataCont();

	for (size_t i = 0; i < server->skirmishAIs.size(); i++) {
		const auto& [valid, ai] = server->skirmishAIs[i];

		if (!valid || ai.hostPlayer != playerNum)
			continue;

		// AIs not from the setup script were created at runtime: unknown kind
		if (i >= setupAIs.size() || !setupAIs[i].isLuaAI)
			return true;
	}

	return false;
}

std::vector<int> CSnapshotJoinServer::SelectSources(const GameParticipant& joiner) const
{
	std::vector<int> specs;
	std::vector<int> players;

	for (const GameParticipant& p: server->players) {
		if (p.id == joiner.id || p.isFromDemo || p.clientLink == nullptr)
			continue;
		if (p.myState != GameParticipant::INGAME || p.isReconn || p.awaitingSnapshot || p.desynced)
			continue;
		if ((server->serverFrameNum - p.lastFrameResponse) > MAX_SOURCE_LAG)
			continue;

		(p.spectator ? specs : players).push_back(p.id);
	}

	// creating a snapshot briefly stalls its source; spectators do not affect the game
	// speed, so prefer them and never stall players for a joining spectator
	std::mt19937 rng(std::random_device{}());
	std::shuffle(specs.begin(), specs.end(), rng);
	std::shuffle(players.begin(), players.end(), rng);

	std::vector<int> sources = specs;

	if (!joiner.spectator)
		sources.insert(sources.end(), players.begin(), players.end());

	sources.resize(std::min(sources.size(), size_t(configHandler->GetInt("SnapshotJoinSources"))));
	return sources;
}


bool CSnapshotJoinServer::TryBegin(GameParticipant& joiner, bool clientWantsSnapshot)
{
	if (!clientWantsSnapshot || !configHandler->GetBool("AllowSnapshotJoin"))
		return false;
	if (!server->gameHasStarted || server->demoReader != nullptr || joiner.isLocal)
		return false;

	if (HostsNativeAI(joiner.id)) {
		server->Message(spring::format("[SnapshotJoin] %s hosts a native AI, catching up instead", joiner.name.c_str()), false);
		return false;
	}

	const std::vector<int> sourceNums = SelectSources(joiner);

	if (sourceNums.empty() || int(sourceNums.size()) < configHandler->GetInt("SnapshotJoinMinSources")) {
		server->Message(spring::format("[SnapshotJoin] %d suitable source(s) for %s, catching up instead", int(sourceNums.size()), joiner.name.c_str()), false);
		return false;
	}

	Join join;
	join.joinerNum = joiner.id;
	join.requestID = nextRequestID++;
	join.providerNum = sourceNums.front();
	join.startTime = spring_gettime();
	join.progressTime = join.startTime;

	// never reuse the id of a running join
	while (join.requestID == 0 || FindJoin(join.requestID) != nullptr)
		join.requestID = nextRequestID++;

	for (const int num: sourceNums) {
		Source src;
		src.playerNum = num;
		src.role = (num == join.providerNum) ? SNAPSHOT_ROLE_PROVIDER : SNAPSHOT_ROLE_VERIFIER;
		join.sources.push_back(src);

		// assign roles first; the marker below follows on the same ordered link
		server->players[num].SendData(CBaseNetProtocol::Get().SendSnapshotRequest(join.requestID, src.role, -1));
	}

	// hold the joiner back from here on
	joiner.awaitingSnapshot = true;

	// the snapshot point; part of the game stream (and history), the joiner gets it while catching up
	join.markerFrame = server->serverFrameNum;
	join.markerIndex = server->packetCache.size();
	server->Broadcast(CBaseNetProtocol::Get().SendSnapshotRequest(join.requestID, SNAPSHOT_ROLE_MARKER, join.markerFrame));
	assert(server->packetCache.size() == (join.markerIndex + 1));

	joiner.clientLink->SetOutgoingBandwidthLimit(configHandler->GetInt("SnapshotTransferRate"));

	server->Message(spring::format(
		"[SnapshotJoin] %s joins from a snapshot of frame %d (request %d, %d source(s), provider %s)",
		joiner.name.c_str(), join.markerFrame, join.requestID, int(join.sources.size()), server->players[join.providerNum].name.c_str()
	), false);

	joins.push_back(std::move(join));
	return true;
}


CSnapshotJoinServer::Join* CSnapshotJoinServer::FindJoin(uint8_t requestID)
{
	const auto it = std::find_if(joins.begin(), joins.end(), [&](const Join& j) { return (j.requestID == requestID); });
	return (it != joins.end()) ? &(*it) : nullptr;
}

bool CSnapshotJoinServer::IsSource(int playerNum) const
{
	for (const Join& j: joins) {
		if (j.phase != Phase::SNAPSHOT)
			continue;

		for (const Source& s: j.sources) {
			if (s.playerNum == playerNum)
				return true;
		}
	}

	return false;
}


void CSnapshotJoinServer::ProcessPacket(int playerNum, std::shared_ptr<const netcode::RawPacket> packet)
{
	const uint8_t msgID = packet->data[0];
	const size_t headerSize = (msgID == NETMSG_SNAPSHOT_DATA) ? 3 : 1;

	if (packet->length < (headerSize + 2))
		return;

	const uint8_t requestID = packet->data[headerSize];
	Join* join = FindJoin(requestID);

	// e.g. late data of a finished or aborted join
	if (join == nullptr || join->phase != Phase::SNAPSHOT)
		return;

	const auto it = std::find_if(join->sources.begin(), join->sources.end(), [&](const Source& s) { return (s.playerNum == playerNum); });

	if (it == join->sources.end()) {
		LOG_L(L_WARNING, "[SnapshotJoin] ignoring snapshot packet from non-source player %d", playerNum);
		return;
	}

	try {
		if (msgID == NETMSG_SNAPSHOT_DATA)
			OnData(*join, playerNum, packet);
		else
			OnHash(*join, playerNum, packet);
	} catch (const netcode::UnpackPacketException& ex) {
		LOG_L(L_ERROR, "[SnapshotJoin] invalid snapshot packet from player %d: %s", playerNum, ex.what());
		Abort(*join, "invalid snapshot packet");
	}
}

void CSnapshotJoinServer::OnData(Join& join, int playerNum, std::shared_ptr<const netcode::RawPacket> packet)
{
	if (playerNum != join.providerNum)
		return;

	netcode::UnpackPacket pckt(packet, 3);
	uint8_t requestID;
	uint8_t senderNum;
	uint32_t offset;
	uint32_t totalSize;

	pckt >> requestID;
	pckt >> senderNum;
	pckt >> offset;
	pckt >> totalSize;

	const uint32_t headerLen = 3 + 1 + 1 + 4 + 4;
	const uint32_t length = packet->length - headerLen;

	// the server's input rate limiting may process a packet twice (see CGameServer::ServerReadNet)
	if (senderNum == playerNum && totalSize == join.dataTotal && (offset + length) <= join.dataReceived)
		return;

	if (senderNum != playerNum || totalSize > MAX_SNAPSHOT_SIZE || offset != join.dataReceived || (offset + length) > totalSize) {
		Abort(join, "inconsistent snapshot data");
		return;
	}

	if (join.data.empty()) {
		join.data.reserve(totalSize);
		join.dataTotal = totalSize;
	}

	if (totalSize != join.dataTotal) {
		Abort(join, "inconsistent snapshot size");
		return;
	}

	join.data.append(reinterpret_cast<const char*>(packet->data + headerLen), length);
	join.dataReceived += length;
	join.progressTime = spring_gettime();

	// progress, roughly every 8 MB
	if ((join.dataReceived >> 23) != ((join.dataReceived - length) >> 23) || join.dataReceived == totalSize)
		LOG("[SnapshotJoin] request %d: received %u of %u bytes", join.requestID, join.dataReceived, totalSize);
}

void CSnapshotJoinServer::OnHash(Join& join, int playerNum, std::shared_ptr<const netcode::RawPacket> packet)
{
	netcode::UnpackPacket pckt(packet, 1);
	uint8_t requestID;
	uint8_t senderNum;
	uint32_t historySize;
	std::vector<uint8_t> digest(32);

	Source& src = *std::find_if(join.sources.begin(), join.sources.end(), [&](const Source& s) { return (s.playerNum == playerNum); });

	pckt >> requestID;
	pckt >> senderNum;
	pckt >> src.status;
	pckt >> src.frameNum;
	pckt >> src.compressedSize;
	pckt >> historySize; // only set by the server
	pckt >> digest;

	std::copy(digest.begin(), digest.end(), src.digest.begin());
	join.progressTime = spring_gettime();

	if (senderNum != playerNum)
		src.status = SNAPSHOT_STATUS_FAILED;

	LOG("[SnapshotJoin] request %d: %s %s reports status %d (frame %d, %u bytes, digest %02x%02x%02x%02x...)",
		join.requestID, (src.role == SNAPSHOT_ROLE_PROVIDER) ? "provider" : "verifier", server->players[playerNum].name.c_str(),
		int(src.status), src.frameNum, src.compressedSize, src.digest[0], src.digest[1], src.digest[2], src.digest[3]);

	if (src.status != SNAPSHOT_STATUS_OK) {
		if (src.role == SNAPSHOT_ROLE_PROVIDER)
			Abort(join, spring::format("provider failed (status %d)", int(src.status)));

		return;
	}

	if (src.frameNum != join.markerFrame) {
		// all clients have to snapshot exactly at the marker
		Abort(join, spring::format("source %s snapshotted frame %d instead of %d", server->players[playerNum].name.c_str(), src.frameNum, join.markerFrame));
		return;
	}
}

void CSnapshotJoinServer::Verify(Join& join)
{
	if (join.verified)
		return;

	const Source& provider = *std::find_if(join.sources.begin(), join.sources.end(), [&](const Source& s) { return (s.playerNum == join.providerNum); });

	// wait for the provider's digest and all of its data
	if (provider.status != SNAPSHOT_STATUS_OK || join.dataReceived != provider.compressedSize)
		return;

	// every verifier has to have answered (sources that disconnected were removed)
	for (const Source& s: join.sources) {
		if (s.status == 0xFF)
			return;
	}

	int numConfirmations = 0;

	for (const Source& s: join.sources) {
		if (s.playerNum == join.providerNum || s.status != SNAPSHOT_STATUS_OK)
			continue;

		if (s.digest != provider.digest) {
			Abort(join, spring::format("digest of %s differs from the provider's", server->players[s.playerNum].name.c_str()));
			return;
		}

		numConfirmations++;
	}

	join.verified = true;
	server->Message(spring::format("[SnapshotJoin] request %d: snapshot of %u bytes confirmed by %d other source(s)", join.requestID, join.dataReceived, numConfirmations), false);
}

bool CSnapshotJoinServer::Relay(Join& join)
{
	GameParticipant& joiner = server->players[join.joinerNum];

	// stream data as it arrives, before verification; the joiner discards it on abort
	while (join.relayOffset < join.dataReceived && joiner.clientLink->GetOutgoingQueueBytes() < RELAY_QUEUE_LIMIT) {
		const uint32_t length = std::min(RELAY_CHUNK_SIZE, join.dataReceived - join.relayOffset);

		joiner.clientLink->SendData(CBaseNetProtocol::Get().SendSnapshotData(
			join.requestID, join.providerNum, join.relayOffset, join.dataTotal,
			reinterpret_cast<const uint8_t*>(join.data.data() + join.relayOffset), length
		));

		join.relayOffset += length;
		join.progressTime = spring_gettime();
	}

	return (join.verified && join.relayOffset == join.dataReceived);
}

bool CSnapshotJoinServer::SendCatchup(Join& join)
{
	GameParticipant& joiner = server->players[join.joinerNum];
	const auto& cache = server->packetCache;

	// the joiner gets no broadcasts until it has every cached packet, so nothing
	// can be missed or reordered; packets cached meanwhile are sent here as well
	while (join.catchupIndex < cache.size() && joiner.clientLink->GetOutgoingQueueBytes() < RELAY_QUEUE_LIMIT) {
		joiner.clientLink->SendData(cache[join.catchupIndex++]);
	}

	if (join.catchupIndex < cache.size())
		return false;

	joiner.awaitingSnapshot = false;
	return true;
}

bool CSnapshotJoinServer::SendHistory(Join& join)
{
	GameParticipant& joiner = server->players[join.joinerNum];
	const auto& cache = server->packetCache;

	// background transfer: live packets always pass a short queue
	while (join.historyOffset < join.historySize && joiner.clientLink->GetOutgoingQueueBytes() < HISTORY_QUEUE_LIMIT) {
		// serialize the next packets: [uint32 length][packet data]
		while (join.historyBuffer.size() < RELAY_CHUNK_SIZE && join.historyIndex < join.markerIndex) {
			const netcode::RawPacket& packet = *cache[join.historyIndex++];
			const uint32_t length = packet.length;

			join.historyBuffer.append(reinterpret_cast<const char*>(&length), sizeof(length));
			join.historyBuffer.append(reinterpret_cast<const char*>(packet.data), packet.length);
		}

		const uint32_t length = std::min(uint32_t(join.historyBuffer.size()), RELAY_CHUNK_SIZE);

		if (length == 0 || (join.historyOffset + length) > join.historySize) {
			// the cache before the marker changed; cannot happen
			LOG_L(L_ERROR, "[SnapshotJoin] request %d: inconsistent game history (%u of %u bytes)", join.requestID, join.historyOffset, join.historySize);
			return true;
		}

		joiner.clientLink->SendData(CBaseNetProtocol::Get().SendSnapshotHistory(
			join.requestID, join.historyOffset, join.historySize,
			reinterpret_cast<const uint8_t*>(join.historyBuffer.data()), length
		));

		join.historyBuffer.erase(0, length);
		join.historyOffset += length;
	}

	return (join.historyOffset == join.historySize);
}


void CSnapshotJoinServer::Update()
{
	const spring_time timeout = spring_secs(configHandler->GetInt("SnapshotJoinTimeout"));

	for (size_t i = 0; i < joins.size(); ) {
		Join& join = joins[i];
		const size_t numJoins = joins.size();

		switch (join.phase) {
			case Phase::SNAPSHOT: {
				if ((spring_gettime() - join.progressTime) > timeout) {
					const Source& p = *std::find_if(join.sources.begin(), join.sources.end(), [&](const Source& s) { return (s.playerNum == join.providerNum); });
					Abort(join, spring::format("timeout: received %u of %u bytes (provider status %d, announced %u), relayed %u", join.dataReceived, join.dataTotal, int(p.status), p.compressedSize, join.relayOffset));
					break;
				}

				Verify(join);

				if (joins.size() == numJoins && Relay(join))
					Succeed(join);
			} break;

			case Phase::CATCHUP: {
				if (!SendCatchup(join))
					break;

				join.phase = Phase::HISTORY;

				if (join.historySize > 0) {
					server->Message(spring::format("[SnapshotJoin] request %d: %s caught up after %.1fs, sending %u bytes of game history in the background",
						join.requestID, server->players[join.joinerNum].name.c_str(), (spring_gettime() - join.startTime).toSecsf(), join.historySize), false);
				}
			} [[fallthrough]];

			case Phase::HISTORY: {
				if (!SendHistory(join))
					break;

				if (join.historySize > 0)
					server->Message(spring::format("[SnapshotJoin] request %d: completed after %.1fs", join.requestID, (spring_gettime() - join.startTime).toSecsf()), false);

				Remove(join);
			} break;
		}

		// Abort and Remove erase the join
		i += (joins.size() == numJoins);
	}
}

void CSnapshotJoinServer::PlayerLeft(int playerNum)
{
	for (size_t i = 0; i < joins.size(); ) {
		Join& join = joins[i];
		const size_t numJoins = joins.size();

		if (join.joinerNum == playerNum) {
			if (join.phase == Phase::SNAPSHOT)
				AbortSources(join);

			Remove(join);
		} else if (join.phase != Phase::SNAPSHOT) {
			// sources are no longer needed
		} else if (join.providerNum == playerNum) {
			Abort(join, "providing client left");
		} else {
			const auto pred = [&](const Source& s) { return (s.playerNum == playerNum); };
			join.sources.erase(std::remove_if(join.sources.begin(), join.sources.end(), pred), join.sources.end());
		}

		i += (joins.size() == numJoins);
	}
}


void CSnapshotJoinServer::Succeed(Join& join)
{
	GameParticipant& joiner = server->players[join.joinerNum];
	const Source& provider = *std::find_if(join.sources.begin(), join.sources.end(), [&](const Source& s) { return (s.playerNum == join.providerNum); });

	uint64_t historySize = 0;

	for (size_t i = 0; i < join.markerIndex; i++)
		historySize += sizeof(uint32_t) + server->packetCache[i]->length;

	// cannot happen, the snapshot of such a game would be far larger
	if (historySize > std::numeric_limits<uint32_t>::max()) {
		Abort(join, "game history too large");
		return;
	}

	server->Message(spring::format("[SnapshotJoin] request %d: snapshot sent to %s after %.1fs", join.requestID, joiner.name.c_str(), (spring_gettime() - join.startTime).toSecsf()), false);

	joiner.clientLink->SendData(CBaseNetProtocol::Get().SendSnapshotHash(
		join.requestID, uint8_t(join.providerNum), SNAPSHOT_STATUS_OK, join.markerFrame, uint32_t(join.data.size()), uint32_t(historySize), provider.digest.data()
	));
	joiner.clientLink->SendData(CBaseNetProtocol::Get().SendSetPlayerNum(uint8_t(join.joinerNum)));
	joiner.clientLink->SendData(CBaseNetProtocol::Get().SendCurrentFrameProgress(server->serverFrameNum));
	joiner.syncCheckStartFrame = join.markerFrame + 1;

	// the game stream from the marker on, then live packets; the history before it in the background
	join.phase = Phase::CATCHUP;
	join.catchupIndex = join.markerIndex;
	join.historySize = uint32_t(historySize);

	// not needed anymore
	join.data = {};
	join.sources.clear();
}

void CSnapshotJoinServer::Abort(Join& join, const std::string& reason)
{
	assert(join.phase == Phase::SNAPSHOT);

	GameParticipant& joiner = server->players[join.joinerNum];
	const uint8_t noDigest[32] = {0};

	server->Message(spring::format("[SnapshotJoin] request %d for %s aborted (%s), catching up instead", join.requestID, joiner.name.c_str(), reason.c_str()), false);

	// tell the sources to stop and the joiner to catch up
	AbortSources(join);

	if (joiner.clientLink == nullptr) {
		Remove(join);
		return;
	}

	joiner.clientLink->SendData(CBaseNetProtocol::Get().SendSnapshotHash(join.requestID, uint8_t(join.joinerNum), SNAPSHOT_STATUS_ABORTED, -1, 0, 0, noDigest));
	joiner.clientLink->SendData(CBaseNetProtocol::Get().SendSetPlayerNum(uint8_t(join.joinerNum)));
	joiner.clientLink->SendData(CBaseNetProtocol::Get().SendCurrentFrameProgress(server->serverFrameNum));
	joiner.syncCheckStartFrame = 0;

	// the classic catch-up: the complete game stream, then live packets
	join.phase = Phase::CATCHUP;
	join.catchupIndex = 0;
	join.historySize = 0;
	join.data = {};
	join.sources.clear();
}

void CSnapshotJoinServer::AbortSources(const Join& join)
{
	const uint8_t noDigest[32] = {0};

	for (const Source& s: join.sources) {
		server->players[s.playerNum].SendData(CBaseNetProtocol::Get().SendSnapshotHash(join.requestID, uint8_t(s.playerNum), SNAPSHOT_STATUS_ABORTED, -1, 0, 0, noDigest));
	}
}

void CSnapshotJoinServer::Remove(Join& join)
{
	GameParticipant& joiner = server->players[join.joinerNum];
	const uint8_t requestID = join.requestID;

	if (joiner.clientLink != nullptr)
		joiner.clientLink->ResetOutgoingBandwidthLimit();

	joiner.awaitingSnapshot = false;
	joins.erase(std::find_if(joins.begin(), joins.end(), [&](const Join& j) { return (j.requestID == requestID); }));
}
