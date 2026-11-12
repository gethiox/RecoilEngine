/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SnapshotJoinClient.h"

#include <algorithm>
#include <cstring>

#include "Game/Game.h"
#include "Game/SimStateTestHooks.h"
#include "Game/GlobalUnsynced.h"
#include "Net/GameServer.h"
#include "Net/Protocol/BaseNetProtocol.h"
#include "Net/Protocol/NetMessageTypes.h"
#include "Net/Protocol/NetProtocol.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Path/IPathManager.h"
#include "System/Config/ConfigHandler.h"
#include "System/EventHandler.h"
#include "System/LoadSave/CregLoadSaveHandler.h"
#include "System/LoadSave/DemoRecorder.h"
#include "System/LoadSave/GameSnapshot.h"
#include "System/Log/ILog.h"
#include "System/Misc/SpringTime.h"
#include "System/Platform/Threading.h"
#include "System/Net/RawPacket.h"
#include "System/Net/UnpackPacket.h"

CONFIG(bool, SnapshotJoin).defaultValue(false)
	.description("Ask the server for a snapshot of the game when joining a running game, instead of re-simulating it from its start. The start script can decide per game (GAME\\SnapshotJoin).");
CONFIG(int, SnapshotCompressionLevel).defaultValue(1).minimumValue(0).maximumValue(9)
	.description("LZMA2 level used for game snapshots sent to joining clients.");

CSnapshotJoinClient snapshotJoinClient;

// bulk data queued ahead of this client's regular packets (see CConnection::GetOutgoingQueueBytes);
// sent at SnapshotTransferRate, i.e. regular packets wait for at most ~1/64s
static constexpr size_t UPLOAD_QUEUE_LIMIT = 64 * 1024;
static constexpr uint32_t UPLOAD_CHUNK_SIZE = 8 * 1024;
static constexpr uint32_t MAX_SNAPSHOT_SIZE = 256u * 1024u * 1024u; // compressed; a 4-player game is ~15 MB


bool CSnapshotJoinClient::WantsSnapshotJoin(int scriptValue)
{
	// opt-in: re-simulating the game stays the default
	if (scriptValue >= 0)
		return (scriptValue != 0);

	return configHandler->GetBool("SnapshotJoin");
}

void CSnapshotJoinClient::Reset()
{
	// the game ends anyway, so waiting for a running compression is acceptable
	// (a std::async future blocks on destruction, detaching is not an option)
	StopUpload(true);

	for (auto& f: abandoned)
		f.wait();

	abandoned.clear();

	if (joinState == JoinState::PLAYING && !history.complete)
		LOG_L(L_WARNING, "[SnapshotJoin] the game ended before its history was received (%u of %u bytes), the demo is incomplete", history.receivedSize, history.expectedSize);

	assignedRoles.clear();
	ForgetJoin();
}

void CSnapshotJoinClient::ForgetJoin()
{
	incoming = {};
	loadHandler.reset();
	joinRequestID = 0;
	joinMarkerFrame = -1;
	joinState = JoinState::NONE;
	gameIDAnnounced = false;
	history = {};
}

void CSnapshotJoinClient::PreGameEnded()
{
	// a snapshot (or part of one) that no game took: the connection attempt
	// was given up, the next one starts from nothing
	if (loadHandler != nullptr || !incoming.data.empty())
		ForgetJoin();
}

void CSnapshotJoinClient::ConnectionLost()
{
	// the connection is about to be replaced: nothing may use it from the upload thread
	StopUpload(false);
	assignedRoles.clear();
}


//
// joining client
//

void CSnapshotJoinClient::HandlePreGamePacket(std::shared_ptr<const netcode::RawPacket> packet)
{
	// the server commits to the snapshot only once this client confirmed it
	// can use it; until then every failure is still the classic catch-up
	uint8_t ackRequestID = 0;
	bool ackPending = false;

	const auto answer = [&](uint8_t status) {
		const uint8_t noDigest[32] = {0};
		clientNet->Send(CBaseNetProtocol::Get().SendSnapshotHash(ackRequestID, 0xFF, status, -1, 0, 0, noDigest));
		ackPending = false;
	};

	try {
		if (packet->data[0] == NETMSG_SNAPSHOT_DATA) {
			netcode::UnpackPacket pckt(packet, 3);
			uint8_t requestID;
			uint8_t providerNum;
			uint32_t offset;
			uint32_t totalSize;

			pckt >> requestID;
			pckt >> providerNum;
			pckt >> offset;
			pckt >> totalSize;

			const uint32_t headerLen = 3 + 1 + 1 + 4 + 4;
			const uint32_t length = packet->length - headerLen;

			// the server tried again with other sources: a new request starts over
			if (offset == 0 && requestID != incoming.requestID)
				incoming = {};

			if (incoming.data.empty() && offset == 0) {
				incoming.requestID = requestID;
				incoming.data.reserve(std::min(totalSize, MAX_SNAPSHOT_SIZE));
				LOG("[SnapshotJoin] receiving game snapshot (%.1f MB)", totalSize / (1024.0f * 1024.0f));
			}

			if (requestID != incoming.requestID || offset != incoming.data.size() || (offset + length) > totalSize || totalSize > MAX_SNAPSHOT_SIZE) {
				LOG_L(L_ERROR, "[SnapshotJoin] inconsistent snapshot data (request %d, offset %u)", requestID, offset);
				incoming = {};
				return;
			}

			incoming.data.append(reinterpret_cast<const char*>(packet->data + headerLen), length);
			return;
		}

		netcode::UnpackPacket pckt(packet, 1);
		uint8_t requestID;
		uint8_t playerNum;
		uint8_t status;
		int32_t frameNum;
		uint32_t compressedSize;
		uint32_t historySize;
		std::vector<uint8_t> digestBytes(32);

		pckt >> requestID;
		pckt >> playerNum;
		pckt >> status;
		pckt >> frameNum;
		pckt >> compressedSize;
		pckt >> historySize;
		pckt >> digestBytes;

		if (status != SNAPSHOT_STATUS_OK) {
			// also after a snapshot was verified and loaded: the server gave up
			// on the join (e.g. our acknowledgement did not reach it in time),
			// so the game continues from its start like for everyone else
			LOG("[SnapshotJoin] the server could not provide a snapshot, catching up instead");
			ForgetJoin();
			return;
		}

		ackRequestID = requestID;
		ackPending = true;

		const spring_time t0 = spring_gettime();
		std::string raw;
		Digest digest;
		std::copy(digestBytes.begin(), digestBytes.end(), digest.begin());

		if (requestID != incoming.requestID || incoming.data.size() != compressedSize || !GameSnapshot::Decompress(incoming.data, raw)) {
			LOG_L(L_ERROR, "[SnapshotJoin] incomplete or invalid snapshot, catching up instead");
			incoming = {};
			answer(SNAPSHOT_STATUS_FAILED);
			return;
		}

		incoming = {};

		if (GameSnapshot::Hash(raw) != digest) {
			LOG_L(L_ERROR, "[SnapshotJoin] snapshot digest mismatch, catching up instead");
			answer(SNAPSHOT_STATUS_FAILED);
			return;
		}

		auto handler = std::make_unique<CCregLoadSaveHandler>();
		int32_t snapshotFrame = -1;

		if (!handler->LoadSnapshot(std::move(raw), snapshotFrame) || snapshotFrame != frameNum) {
			LOG_L(L_ERROR, "[SnapshotJoin] unusable snapshot (frame %d, expected %d), catching up instead", snapshotFrame, frameNum);
			answer(SNAPSHOT_STATUS_FAILED);
			return;
		}

		answer(SNAPSHOT_STATUS_OK);

		loadHandler = std::move(handler);
		joinRequestID = requestID;
		joinMarkerFrame = frameNum;
		joinState = JoinState::LOADING;
		history = {};
		history.expectedSize = historySize;
		history.complete = (historySize == 0);

		LOG("[SnapshotJoin] verified snapshot of frame %d (digest %s) in %.2fs",
			frameNum, GameSnapshot::DigestToHex(digest).c_str(), (spring_gettime() - t0).toSecsf());
	} catch (const netcode::UnpackPacketException& ex) {
		LOG_L(L_ERROR, "[SnapshotJoin] invalid snapshot packet: %s", ex.what());
		incoming = {};
	} catch (const std::bad_alloc&) {
		LOG_L(L_ERROR, "[SnapshotJoin] out of memory while receiving the snapshot, catching up instead");
		incoming = {};
	} catch (const std::exception& ex) {
		LOG_L(L_ERROR, "[SnapshotJoin] unusable snapshot (%s), catching up instead", ex.what());
		incoming = {};
	}

	if (ackPending)
		answer(SNAPSHOT_STATUS_FAILED);
}

ILoadSaveHandler* CSnapshotJoinClient::TakeLoadHandler()
{
	return loadHandler.release();
}

void CSnapshotJoinClient::Start(CGame* game)
{
	if (joinState != JoinState::LOADING)
		return;

	joinState = JoinState::PLAYING;

	// the game id is part of the loaded state: the demo gets it now (the
	// history, with the original packet, may arrive minutes later) and so do
	// the unsynced handlers, which started fresh (the synced ones reacted to
	// it at game start, their effects are part of the snapshot)
	if (CDemoRecorder* record = clientNet->GetDemoRecorder(); record != nullptr)
		record->SetGameID(game->gameID);

	eventHandler.GameID(game->gameID, sizeof(game->gameID), true);
	gameIDAnnounced = true;

	// the game started long ago; everything synced about that is part of the snapshot
	game->StartPlayingFromSnapshot();
}

void CSnapshotJoinClient::HandleJoinMarker(int32_t markerFrame)
{
	// first packet of the game stream after loading: the snapshot point
	if (markerFrame != joinMarkerFrame || gs->frameNum != joinMarkerFrame) {
		// cannot happen unless the stream or the snapshot is broken
		LOG_L(L_ERROR, "[SnapshotJoin] snapshot marker of frame %d at frame %d (expected %d)", markerFrame, gs->frameNum, joinMarkerFrame);
	} else {
		LOG("[SnapshotJoin] continuing the game from the snapshot of frame %d", joinMarkerFrame);
	}

	// the history before the marker belongs right in front of it
	if (CDemoRecorder* record = clientNet->GetDemoRecorder(); record != nullptr && record->IsValid())
		record->MarkHistoryBeforeLastChunk();

	history.markerSeen = true;
}

void CSnapshotJoinClient::HandleHistory(std::shared_ptr<const netcode::RawPacket> packet, CGame* game)
{
	try {
		netcode::UnpackPacket pckt(packet, 3);
		uint8_t requestID;
		uint32_t offset;
		uint32_t totalSize;

		pckt >> requestID;
		pckt >> offset;
		pckt >> totalSize;

		const uint32_t headerLen = 3 + 1 + 4 + 4;
		const uint32_t length = packet->length - headerLen;

		if (joinState != JoinState::PLAYING || !history.markerSeen || history.complete || requestID != joinRequestID) {
			LOG_L(L_ERROR, "[SnapshotJoin] unexpected game history (request %d)", requestID);
			return;
		}
		if (offset != history.receivedSize || totalSize != history.expectedSize || (offset + length) > totalSize) {
			LOG_L(L_ERROR, "[SnapshotJoin] inconsistent game history (offset %u, received %u of %u bytes); the demo will be incomplete", offset, history.receivedSize, totalSize);
			history.complete = true;
			return;
		}

		history.pending.append(reinterpret_cast<const char*>(packet->data + headerLen), length);
		history.receivedSize += length;

		// split into packets: [uint32 length][packet data]
		size_t pos = 0;

		while ((history.pending.size() - pos) >= sizeof(uint32_t)) {
			uint32_t packetLength;
			memcpy(&packetLength, history.pending.data() + pos, sizeof(packetLength));

			if ((history.pending.size() - pos - sizeof(packetLength)) < packetLength)
				break;

			pos += sizeof(packetLength);
			AddHistoryPacket(reinterpret_cast<const uint8_t*>(history.pending.data() + pos), packetLength, game);
			pos += packetLength;
		}

		history.pending.erase(0, pos);

		if (history.receivedSize < history.expectedSize)
			return;

		history.complete = true;

		if (!history.pending.empty() || history.frameNum != joinMarkerFrame) {
			LOG_L(L_ERROR, "[SnapshotJoin] inconsistent game history (%d frames, expected %d); the demo will be incomplete", history.frameNum, joinMarkerFrame);
			return;
		}

		LOG("[SnapshotJoin] received the game history (%u packets, %d frames, %.1f MB)", history.numPackets, history.frameNum, history.receivedSize / (1024.0f * 1024.0f));
	} catch (const netcode::UnpackPacketException& ex) {
		LOG_L(L_ERROR, "[SnapshotJoin] invalid game history packet: %s", ex.what());
	}
}

void CSnapshotJoinClient::AddHistoryPacket(const uint8_t* data, uint32_t length, CGame* game)
{
	if (length == 0)
		return;

	history.numPackets++;

	// with the timing it would have had when received live (see CNetProtocol::GetData)
	if (CDemoRecorder* record = clientNet->GetDemoRecorder(); record != nullptr && record->IsValid())
		record->SaveHistoryToDemo(data, length, clientNet->GetPacketTime(std::max(history.frameNum, 0)));

	// the synced effects are part of the snapshot, only local ones are applied
	switch (data[0]) {
		case NETMSG_KEYFRAME:
		case NETMSG_NEWFRAME: {
			history.frameNum++;
		} break;

		case NETMSG_GAMEID: {
			if (length < (1 + sizeof(game->gameID)))
				break;

			if (CDemoRecorder* record = clientNet->GetDemoRecorder(); record != nullptr)
				record->SetGameID(&data[1]);

			// normally announced from the loaded state already (see Start)
			if (!gameIDAnnounced) {
				memcpy(game->gameID, &data[1], sizeof(game->gameID));
				eventHandler.GameID(game->gameID, sizeof(game->gameID), true);
				gameIDAnnounced = true;
			}
		} break;

		default: {
		} break;
	}
}


//
// source client
//

void CSnapshotJoinClient::HandleRequest(std::shared_ptr<const netcode::RawPacket> packet)
{
	// replays contain markers (and no assignments), never act on them
	if (gameServer != nullptr && gameServer->GetDemoReader() != nullptr)
		return;

	if (packet->length < 7)
		return;

	const uint8_t requestID = packet->data[1];
	const uint8_t role = packet->data[2];
	int32_t frameNum;
	memcpy(&frameNum, packet->data + 3, sizeof(frameNum));

	if (role == SNAPSHOT_ROLE_MARKER && joinState == JoinState::PLAYING && requestID == joinRequestID && !history.markerSeen) {
		HandleJoinMarker(frameNum);
		return;
	}

	if (role != SNAPSHOT_ROLE_MARKER) {
		assignedRoles[requestID] = role;
		return;
	}

	const auto it = assignedRoles.find(requestID);

	if (it == assignedRoles.end())
		return;

	const uint8_t assignedRole = it->second;
	assignedRoles.erase(it);

	CreateSnapshot(requestID, assignedRole, frameNum);
}

void CSnapshotJoinClient::HandleServerHash(std::shared_ptr<const netcode::RawPacket> packet)
{
	if (packet->length < 4 || packet->data[3] != SNAPSHOT_STATUS_ABORTED)
		return;

	const uint8_t requestID = packet->data[1];
	assignedRoles.erase(requestID);

	if (upload != nullptr && upload->requestID == requestID) {
		LOG("[SnapshotJoin] request %d aborted by the server", requestID);
		StopUpload(false);
	}
}

void CSnapshotJoinClient::SendHash(uint8_t requestID, uint8_t status, int32_t frameNum, uint32_t compressedSize, const Digest& digest)
{
	clientNet->Send(CBaseNetProtocol::Get().SendSnapshotHash(requestID, uint8_t(gu->myPlayerNum), status, frameNum, compressedSize, 0, digest.data()));
}

void CSnapshotJoinClient::CreateSnapshot(uint8_t requestID, uint8_t role, int32_t markerFrame)
{
	const Digest noDigest = {};

	// a compression of an aborted request may still be running
	abandoned.erase(std::remove_if(abandoned.begin(), abandoned.end(), [](const std::future<Upload::Result>& f) {
		return (f.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
	}), abandoned.end());

	// one at a time; the server picks other sources (or tries again later)
	if (upload != nullptr || !abandoned.empty()) {
		SendHash(requestID, SNAPSHOT_STATUS_FAILED, gs->frameNum, 0, noDigest);
		return;
	}
	if (!pathManager->HasExactSerialization()) {
		SendHash(requestID, SNAPSHOT_STATUS_UNSUPPORTED, gs->frameNum, 0, noDigest);
		return;
	}
	// a joining client builds its defs from the archives, edits are not in
	// the snapshot (and gs->defsEdited is synced, so every source refuses)
	if (gs->defsEdited) {
		LOG("[SnapshotJoin] request %d: defs were edited by Lua, no snapshot", requestID);
		SendHash(requestID, SNAPSHOT_STATUS_UNSUPPORTED, gs->frameNum, 0, noDigest);
		return;
	}

	// the only part that stalls this client: the serialization itself
	const spring_time t0 = spring_gettime();
	std::string raw;

	if (!GameSnapshot::Write(raw)) {
		SendHash(requestID, SNAPSHOT_STATUS_FAILED, gs->frameNum, 0, noDigest);
		return;
	}

	LOG("[SnapshotJoin] request %d: snapshot of frame %d (marker frame %d) written in %.2fs (%.1f MB)",
		requestID, gs->frameNum, markerFrame, (spring_gettime() - t0).toSecsf(), raw.size() / (1024.0f * 1024.0f));

	SimStateTestHooks::SnapshotCreated(gs->frameNum, raw);

	upload = std::make_unique<Upload>();
	upload->requestID = requestID;
	upload->role = role;
	upload->frameNum = gs->frameNum;
	upload->playerNum = uint8_t(gu->myPlayerNum);

	// hashing (and for the provider the compression) happen in the background;
	// the digest is of the raw snapshot, so verifiers do not compress at all
	const int level = configHandler->GetInt("SnapshotCompressionLevel");
	const bool compress = (role == SNAPSHOT_ROLE_PROVIDER);

	// the raw snapshot is released as soon as it is hashed and compressed, not
	// when the upload (which holds the task's state) ends
	auto rawPtr = std::make_shared<std::string>(std::move(raw));

	upload->pending = std::async(std::launch::async, [rawPtr, level, compress]() {
		Upload::Result r;

		try {
			r.digest = GameSnapshot::Hash(*rawPtr);
			r.ok = !compress || (GameSnapshot::Compress(*rawPtr, r.compressed, level) && r.compressed.size() <= MAX_SNAPSHOT_SIZE);
		} catch (const std::exception& ex) {
			// e.g. out of memory for the compressed copy
			LOG_L(L_ERROR, "[SnapshotJoin] could not compress the snapshot (%s)", ex.what());
			r.ok = false;
		}

		rawPtr->clear();
		rawPtr->shrink_to_fit();
		return r;
	});
}

void CSnapshotJoinClient::Update()
{
	using namespace std::chrono_literals;

	// drop finished compressions of aborted requests
	abandoned.erase(std::remove_if(abandoned.begin(), abandoned.end(), [](const std::future<Upload::Result>& f) {
		return (f.wait_for(0ms) == std::future_status::ready);
	}), abandoned.end());

	if (upload == nullptr)
		return;

	if (upload->pending.valid()) {
		if (upload->pending.wait_for(0ms) != std::future_status::ready)
			return;

		try {
			upload->result = upload->pending.get();
		} catch (const std::exception& ex) {
			LOG_L(L_ERROR, "[SnapshotJoin] could not compress the snapshot (%s)", ex.what());
			upload->result = {};
		}
	}

	Upload& u = *upload;

	if (!u.resultSent) {
		u.resultSent = true;

		const uint8_t status = u.result.ok ? SNAPSHOT_STATUS_OK : SNAPSHOT_STATUS_FAILED;
		SendHash(u.requestID, status, u.frameNum, uint32_t(u.result.compressed.size()), u.result.digest);

		if (u.role == SNAPSHOT_ROLE_PROVIDER) {
			LOG("[SnapshotJoin] request %d: digest %s, %.1f MB compressed", u.requestID,
				GameSnapshot::DigestToHex(u.result.digest).c_str(), u.result.compressed.size() / (1024.0f * 1024.0f));
		} else {
			LOG("[SnapshotJoin] request %d: digest %s", u.requestID, GameSnapshot::DigestToHex(u.result.digest).c_str());
		}

		if (!u.result.ok || u.role != SNAPSHOT_ROLE_PROVIDER) {
			upload.reset();
			return;
		}

		clientNet->SetOutgoingBandwidthLimit(configHandler->GetInt("SnapshotTransferRate"));
		u.thread = std::thread(&CSnapshotJoinClient::UploadLoop, std::ref(u));
		return;
	}

	if (!u.done)
		return;

	if (u.failed) {
		// the server gives the join to other sources (or up)
		LOG_L(L_ERROR, "[SnapshotJoin] request %d: upload failed", u.requestID);
		SendHash(u.requestID, SNAPSHOT_STATUS_FAILED, u.frameNum, 0, Digest{});
	} else {
		LOG("[SnapshotJoin] request %d: snapshot uploaded", u.requestID);
	}

	StopUpload(true);
}

void CSnapshotJoinClient::UploadLoop(Upload& u)
{
	Threading::SetThreadName("snapshot-upload");

	// an exception must not escape the thread (that would end the process)
	try {
		UploadLoopBody(u);
	} catch (const std::exception& ex) {
		LOG_L(L_ERROR, "[SnapshotJoin] request %d: upload thread failed (%s)", u.requestID, ex.what());
		u.failed = true;
	} catch (...) {
		LOG_L(L_ERROR, "[SnapshotJoin] request %d: upload thread failed", u.requestID);
		u.failed = true;
	}

	u.done = true;
}

void CSnapshotJoinClient::UploadLoopBody(Upload& u)
{
	const std::string& data = u.result.compressed;
	spring_time progressTime = spring_gettime();
	uint32_t progressOffset = u.offset;

	while (!u.stop) {
		// flow control: never queue more than a little ahead of regular game packets
		while (u.offset < data.size() && clientNet->GetOutgoingQueueBytes() < UPLOAD_QUEUE_LIMIT) {
			const uint32_t length = std::min<uint32_t>(UPLOAD_CHUNK_SIZE, data.size() - u.offset);

			clientNet->Send(CBaseNetProtocol::Get().SendSnapshotData(
				u.requestID, u.playerNum, u.offset, uint32_t(data.size()),
				reinterpret_cast<const uint8_t*>(data.data() + u.offset), length
			));

			u.offset += length;
		}

		// the connection only sends (and processes acknowledgements) when updated;
		// do that far more often than once per frame
		clientNet->Update();

		if (u.offset == data.size() && clientNet->GetOutgoingQueueBytes() == 0)
			break;

		if (u.offset != progressOffset) {
			progressOffset = u.offset;
			progressTime = spring_gettime();
		} else if ((spring_gettime() - progressTime) > spring_secs(5)) {
			LOG_L(L_WARNING, "[SnapshotJoin] request %d: upload makes no progress (%u of %u bytes queued, %u bytes in the link's queue)",
				u.requestID, u.offset, unsigned(data.size()), unsigned(clientNet->GetOutgoingQueueBytes()));
			progressTime = spring_gettime();
		}

		spring_msecs(2).sleep(true);
	}
}

void CSnapshotJoinClient::StopUpload(bool waitForCompression)
{
	if (upload == nullptr)
		return;

	if (upload->pending.valid()) {
		// the compression runs for several seconds; an aborted request must
		// not stall this client (and with it the game) for that long
		if (waitForCompression)
			upload->pending.wait();
		else
			abandoned.push_back(std::move(upload->pending));
	}

	if (upload->thread.joinable()) {
		upload->stop = true;
		upload->thread.join();

		clientNet->ResetOutgoingBandwidthLimit();
	}

	upload.reset();
}
