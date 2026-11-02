/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// included first due to "WinSock.h has already been included" error on Windows
#include "System/Net/UDPConnection.h"
#include "System/Net/LocalConnection.h"

#include "NetProtocol.h"
#include "Net/SnapshotJoinClient.h"
#include "Game/ClientSetup.h"
#include "Game/GlobalUnsynced.h"
#include "Sim/Misc/GlobalConstants.h"
#include "System/LoadSave/DemoRecorder.h"
// #include "System/Net/LocalConnection.h"
// #include "System/Net/UDPConnection.h"
#include "System/Net/UnpackPacket.h"
#include "System/Platform/Threading.h"
#include "System/Config/ConfigHandler.h"
#include "System/GlobalConfig.h"
#include "System/Log/ILog.h"
#include "System/SafeUtil.h"

CONFIG(int, SourcePort).defaultValue(0);


CNetProtocol* clientNet = nullptr;

CNetProtocol::CNetProtocol() {
	static_assert(sizeof(serverConnMem) >= sizeof(netcode::UDPConnection), "");
	static_assert(sizeof(serverConnMem) >= sizeof(netcode::CLocalConnection), "");
	static_assert(sizeof(demoRecordMem) >= sizeof(CDemoRecorder), "");

	memset(serverConnMem, 0, sizeof(serverConnMem));
	memset(demoRecordMem, 0, sizeof(demoRecordMem));

	demoRecordPtr = new (demoRecordMem) CDemoRecorder();
}

CNetProtocol::~CNetProtocol()
{
	// when the client-server connection is deleted, make sure
	// the server cleans up its corresponding connection to the
	// client
	Send(CBaseNetProtocol::Get().SendQuit(__func__));
	Close(true);

	LOG("[NetProto::%s] %s",__func__, serverConnPtr->Statistics().c_str());

	spring::SafeDestruct(serverConnPtr);
	spring::SafeDestruct(demoRecordPtr);
}


void CNetProtocol::InitClient(std::shared_ptr<ClientSetup> clientSetup, const std::string& clientVersion, const std::string& clientPlatform)
{
	userName = clientSetup->myPlayerName;
	userPasswd = clientSetup->myPasswd;

	serverConnPtr = new (serverConnMem) netcode::UDPConnection(configHandler->GetInt("SourcePort"), clientSetup->hostIP, clientSetup->hostPort);
	serverConnPtr->Unmute();
	serverConnPtr->SendData(CBaseNetProtocol::Get().SendAttemptConnect(userName, userPasswd, clientVersion, clientPlatform, globalConfig.networkLossFactor, false, CSnapshotJoinClient::WantsSnapshotJoin()));
	serverConnPtr->Flush(true);

	LOG("[NetProto::%s] connecting to IP %s on port %i using name %s", __func__, clientSetup->hostIP.c_str(), clientSetup->hostPort, userName.c_str());
}

void CNetProtocol::InitLocalClient()
{
	serverConnPtr = new (serverConnMem) netcode::CLocalConnection();
	serverConnPtr->Flush();

	LOG("[NetProto::%s] connecting to local server", __func__);
}


void CNetProtocol::AttemptReconnect(const std::string& myVersion, const std::string& myPlatform)
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	netcode::UDPConnection conn(*serverConnPtr);

	conn.Unmute();
	conn.SendData(CBaseNetProtocol::Get().SendAttemptConnect(userName, userPasswd, myVersion, myPlatform, globalConfig.networkLossFactor, true));
	conn.Flush(true);

	LOG("[NetProto::%s] reconnecting to server... %ds", __func__, dynamic_cast<decltype(conn)*>(serverConnPtr)->GetReconnectSecs());
}


bool CNetProtocol::NeedsReconnect() {
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	return serverConnPtr->NeedsReconnect();
}

bool CNetProtocol::CheckTimeout(int nsecs, bool initial) const {
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	return serverConnPtr->CheckTimeout(nsecs, initial);
}

bool CNetProtocol::Connected() const
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	return (serverConnPtr->GetDataReceived() > 0);
}

std::string CNetProtocol::ConnectionStr() const
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	return serverConnPtr->GetFullAddress();
}

std::shared_ptr<const netcode::RawPacket> CNetProtocol::Peek(unsigned ahead) const
{
	// the connection may be updated by another thread (bulk transfers, see CSnapshotJoinClient)
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	return serverConnPtr->Peek(ahead);
}

void CNetProtocol::DeleteBufferPacketAt(unsigned index)
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	return serverConnPtr->DeleteBufferPacketAt(index);
}


float CNetProtocol::GetPacketTime(int frameNum) const
{
	// startTime is not yet defined pre-simframe
	if (frameNum < 0)
		return gu->gameTime;

	return (gu->startTime + frameNum * INV_GAME_SPEED);
}


std::shared_ptr<const netcode::RawPacket> CNetProtocol::GetData(int frameNum)
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	std::shared_ptr<const netcode::RawPacket> ret = serverConnPtr->GetData();

	if (ret == nullptr)
		return ret;
	if (ret->data[0] == NETMSG_GAMEDATA)
		return ret;
	// snapshot transfers are not part of the game (the history is recorded separately); requests only as stream markers
	if (ret->data[0] == NETMSG_SNAPSHOT_DATA || ret->data[0] == NETMSG_SNAPSHOT_HASH || ret->data[0] == NETMSG_SNAPSHOT_HISTORY)
		return ret;
	if (ret->data[0] == NETMSG_SNAPSHOT_REQUEST && (ret->length < 3 || ret->data[2] != SNAPSHOT_ROLE_MARKER))
		return ret;

	if (demoRecordPtr->IsValid())
		demoRecordPtr->SaveToDemo(ret->data, ret->length, GetPacketTime(frameNum));

	return ret;
}


void CNetProtocol::Send(const netcode::RawPacket* pkt) { Send(std::shared_ptr<const netcode::RawPacket>(pkt)); }
void CNetProtocol::Send(std::shared_ptr<const netcode::RawPacket> pkt)
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	serverConnPtr->SendData(pkt);
}


void CNetProtocol::SetOutgoingBandwidthLimit(int bytesPerSecond)
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	serverConnPtr->SetOutgoingBandwidthLimit(bytesPerSecond);
}

void CNetProtocol::ResetOutgoingBandwidthLimit()
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	serverConnPtr->ResetOutgoingBandwidthLimit();
}

size_t CNetProtocol::GetOutgoingQueueBytes()
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	return serverConnPtr->GetOutgoingQueueBytes();
}

__FORCE_ALIGN_STACK__
void CNetProtocol::UpdateLoop()
{
	Threading::SetThreadName("heartbeat");

	while (keepUpdating) {
		Update();
		spring_msecs(100).sleep();
	}
}

void CNetProtocol::Update()
{
	// any call to clientNet->Send is unsafe while heartbeat thread exists, i.e. during loading
	std::lock_guard<spring::spinlock> lock(serverConnMutex);

	serverConnPtr->Update();
}

void CNetProtocol::Close(bool flush)
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);

	serverConnPtr->Close(flush);
}


// NOTE: has to use swap rather than assign because of Reset
void CNetProtocol::SetDemoRecorder(CDemoRecorder&& r) { std::swap(*demoRecordPtr, r); }
void CNetProtocol::ResetDemoRecorder() { SetDemoRecorder({}); }

unsigned int CNetProtocol::GetNumWaitingServerPackets() const
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	return (serverConnPtr->GetPacketQueueSize());
}

unsigned int CNetProtocol::GetNumWaitingPingPackets() const
{
	std::lock_guard<spring::spinlock> lock(serverConnMutex);
	return (serverConnPtr->GetNumQueuedPings());
}

