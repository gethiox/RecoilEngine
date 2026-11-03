/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cstdio>
#include <memory>
#include <sstream>
#include <vector>
#include <zlib.h>

#include "ExternalAI/SkirmishAIHandler.h"
#include "ExternalAI/EngineOutHandler.h"
#include "CregLoadSaveHandler.h"
#include "GameSnapshot.h"
#include "Map/MapDamage.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"
#include "Game/Game.h"
#include "Game/GameSetup.h"
#include "Game/GameVersion.h"
#include "Game/GlobalUnsynced.h"
#include "Game/WaitCommandsAI.h"
#include "Game/SelectedUnitsHandler.h"
#include "Game/Players/PlayerHandler.h"
#include "Game/UI/Groups/GroupHandler.h"
#include "Lua/LuaGaia.h"
#include "Lua/LuaRules.h"
#include "Net/GameServer.h"
#include "Rendering/Textures/ColorMap.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Rendering/Env/Decals/GroundDecalHandler.h"
#include "Sim/Ecs/Helper.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Misc/BuildingMaskMap.h"
#include "Sim/Misc/GroundBlockingObjectMap.h"
#include "Sim/Misc/InterceptHandler.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/SmoothHeightMesh.h"
#include "Sim/Misc/CategoryHandler.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Units/Unit.h"
#include "Sim/Weapons/Weapon.h"
#include "Sim/Path/IPathManager.h"
#include "Sim/Units/CommandAI/BuilderCaches.h"
#include "System/creg/RawSerialize.h"
#include "System/creg/STL_Set.h"
#include "Sim/Misc/Wind.h"
#include "Sim/Misc/YardmapStatusEffectsMap.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Units/CommandAI/CommandDescription.h"
#include "Sim/Units/Scripts/CobEngine.h"
#include "Sim/Units/Scripts/UnitScriptEngine.h"
#include "Sim/Units/Scripts/NullUnitScript.h"
#include "Sim/Weapons/PlasmaRepulser.h"
#include "System/SafeUtil.h"
#include "System/Platform/errorhandler.h"
#include "System/FileSystem/DataDirsAccess.h"
#include "System/FileSystem/FileHandler.h"
#include "System/FileSystem/FileSystem.h"
#include "System/FileSystem/FileQueryFlags.h"
#include "System/FileSystem/GZFileHandler.h"
#include "System/Threading/ThreadPool.h"
#include "System/creg/SerializeLuaState.h"
#include "System/creg/Serializer.h"
#include "System/Exceptions.h"
#include "System/Sync/SyncChecker.h"
#include "System/Log/ILog.h"

#define MAX_STRING_SIZE (1 << 19) // 512kB excluding null-term


CCregLoadSaveHandler::CCregLoadSaveHandler()
{}

CCregLoadSaveHandler::~CCregLoadSaveHandler() = default;

#ifdef USING_CREG
class CGameStateCollector
{
	CR_DECLARE_STRUCT(CGameStateCollector)

public:
	CGameStateCollector() = default;

	void Serialize(creg::ISerializer* s);
};

CR_BIND(CGameStateCollector, )
CR_REG_METADATA(CGameStateCollector, (
	CR_SERIALIZER(Serialize)
))


static uint32_t loadedChecksum = 0;
static uint32_t loadedPrevChecksum = 0;

/// synced state that lives outside of any creg-registered object
static void SerializeSyncedGlobals(creg::ISerializer* s)
{
	auto rngState = gsRNG.GetFullState();
	s->SerializeInt(&rngState.genState, sizeof(rngState.genState));
	s->SerializeInt(&rngState.genStream, sizeof(rngState.genStream));
	s->SerializeInt(&rngState.initSeed, sizeof(rngState.initSeed));
	s->SerializeInt(&rngState.lastSeed, sizeof(rngState.lastSeed));

	// the sync checksum accumulates over up to 4096 frames, a loaded
	// game has to continue with the same value to stay comparable
	uint32_t checksum = 0;
	uint32_t prevChecksum = 0;
#ifdef SYNCCHECK
	checksum = CSyncChecker::GetChecksum();
	prevChecksum = CSyncChecker::GetPrevChecksum();
#endif
	s->SerializeInt(&checksum, sizeof(checksum));
	s->SerializeInt(&prevChecksum, sizeof(prevChecksum));

	// CObject sync ids order listener/dependency lists; objects created after
	// loading must get higher ids than every loaded one, like in the original
	int64_t curSyncID = CObject::GetCurrentSyncID();
	s->SerializeInt(&curSyncID, sizeof(curSyncID));

	// builder bookkeeping kept outside of the units, iterated in hash order
	const auto setType = creg::DeduceType<decltype(CBuilderCaches::reclaimers)>::Get();
	setType->Serialize(s, &CBuilderCaches::reclaimers);
	setType->Serialize(s, &CBuilderCaches::featureReclaimers);
	setType->Serialize(s, &CBuilderCaches::resurrecters);

	smoothGround.Serialize(s);

	// terrain type properties can be changed by synced Lua (Spring.SetTerrainTypeData)
	{
		creg::RawArchive ar(s);

		for (int i = 0; i < CMapInfo::NUM_TERRAIN_TYPES; i++) {
			CMapInfo::TerrainType& tt = const_cast<CMapInfo::TerrainType&>(mapInfo->terrainTypes[i]);

			ar.String(tt.name);
			ar.Value(tt.hardness);
			ar.Value(tt.tankSpeed);
			ar.Value(tt.kbotSpeed);
			ar.Value(tt.hoverSpeed);
			ar.Value(tt.shipSpeed);
			ar.Value(tt.receiveTracks);
		}
	}

	// explosions still deforming the terrain
	mapDamage->Serialize(s);

	if (s->IsWriting())
		return;

	// derived hardness tables; speed changes need no repathing here, the
	// path finder state is loaded as it was
	for (int i = 0; i < CMapInfo::NUM_TERRAIN_TYPES; i++) {
		mapDamage->TerrainTypeHardnessChanged(i);
	}

	CObject::RaiseCurrentSyncID(curSyncID);
	gsRNG.SetFullState(rngState);

	// restored once everything is loaded: constructing objects with synced
	// members (e.g. SyncedFloat3) feeds the checksum, so anything created
	// after this point would leave the loaded game with a different one
	loadedChecksum = checksum;
	loadedPrevChecksum = prevChecksum;
}

void CGameStateCollector::Serialize(creg::ISerializer* s)
{
	s->SerializeObjectInstance(gs, gs->GetClass());
	s->SerializeObjectInstance(gu, gu->GetClass());
	s->SerializeObjectInstance(gameSetup, gameSetup->GetClass());
	s->SerializeObjectInstance(game, game->GetClass());
	s->SerializeObjectInstance(readMap, readMap->GetClass());
	s->SerializeObjectInstance(&quadField, quadField.GetClass());
	s->SerializeObjectInstance(&unitHandler, unitHandler.GetClass());
	s->SerializeObjectInstance(&globalUnitParams, globalUnitParams.GetClass());
	s->SerializeObjectInstance(cobEngine, cobEngine->GetClass());
	s->SerializeObjectInstance(unitScriptEngine, unitScriptEngine->GetClass());
	s->SerializeObjectInstance(&CNullUnitScript::value, CNullUnitScript::value.GetClass());
	s->SerializeObjectInstance(&featureHandler, featureHandler.GetClass());
	s->SerializeObjectInstance(losHandler, losHandler->GetClass());
	s->SerializeObjectInstance(&interceptHandler, interceptHandler.GetClass());
	s->SerializeObjectInstance(CCategoryHandler::Instance(), CCategoryHandler::Instance()->GetClass());
	s->SerializeObjectInstance(&groundBlockingObjectMap, groundBlockingObjectMap.GetClass());
	s->SerializeObjectInstance(&yardmapStatusEffectsMap, yardmapStatusEffectsMap.GetClass());
	s->SerializeObjectInstance(&buildingMaskMap, buildingMaskMap.GetClass());
	s->SerializeObjectInstance(&projectileHandler, projectileHandler.GetClass());
	CPlasmaRepulser::SerializeShieldSegmentCollectionPool(s);
	CColorMap::SerializeColorMaps(s);
	s->SerializeObjectInstance(&waitCommandsAI, waitCommandsAI.GetClass());
	s->SerializeObjectInstance(&envResHandler, envResHandler.GetClass());
	s->SerializeObjectInstance(&moveDefHandler, moveDefHandler.GetClass());
	s->SerializeObjectInstance(&teamHandler, teamHandler.GetClass());
	// player states (active, spectator, team, ...) change through network
	// messages processed by every client at the same frame, i.e. they are
	// synced state queried by game code
	s->SerializeObjectInstance(&playerHandler, playerHandler.GetClass());
	// control groups are client specific
	for (int a = 0; a < teamHandler.ActiveTeams() && !GameSnapshot::IsActive(); a++) {
		s->SerializeObjectInstance(&uiGroupHandlers[a], uiGroupHandlers[a].GetClass());
	}
	s->SerializeObjectInstance(&commandDescriptionCache, commandDescriptionCache.GetClass());
	CSkirmishAIHandler::SerializeSkirmishAIHandler(s);
	s->SerializeObjectInstance(eoh, eoh->GetClass());
	std::unique_ptr<creg::IType> mapType = creg::DeduceType<decltype(CSplitLuaHandle::gameParams)>::Get();
	mapType->Serialize(s, &CSplitLuaHandle::gameParams);

	SerializeSyncedGlobals(s);

	// pathing state refers to units, so it has to come after unitHandler
	bool exactPathing = pathManager->HasExactSerialization();
	s->SerializeInt(&exactPathing, sizeof(exactPathing));

	if (exactPathing != pathManager->HasExactSerialization())
		throw content_error("[CGameStateCollector] saved with a different path finder");

	if (exactPathing)
		pathManager->SerializeState(s);

	// rendering data is client specific; a joining client starts without it
	if (GameSnapshot::IsActive())
		return;

	s->SerializeObjectInstance(CUnitDrawer::modelDrawerData->GetSavedData(), CUnitDrawer::modelDrawerData->GetSavedData()->GetClass());
	s->SerializeObjectInstance(groundDecals, groundDecals->GetClass());
}


class CLuaStateCollector
{
	CR_DECLARE_STRUCT(CLuaStateCollector)

public:
	CLuaStateCollector() = default;
	void Read(const CSplitLuaHandle* handle);
	void Write(CSplitLuaHandle* handle);

	bool valid;
	lua_State* L = nullptr;
	lua_State* L_GC = nullptr;
	std::vector<bool> watchUnitDefs;        // callin masks for Unit*Collision, UnitMoveFailed
	std::vector<bool> watchFeatureDefs;     // callin masks for UnitFeatureCollision
	std::vector<bool> watchProjectileDefs;  // callin masks for Projectile*
	std::vector<bool> watchExplosionDefs;   // callin masks for Explosion
	std::vector<bool> watchAllowTargetDefs; // callin masks for AllowWeapon*Target*
	decltype(CLuaHandle::delayedCallsByFrame) delayedCallsByFrame;

	void Serialize(creg::ISerializer* s);
};

CR_BIND(CLuaStateCollector, )
CR_REG_METADATA(CLuaStateCollector, (
	CR_MEMBER(valid),
	CR_IGNORED(L),
	CR_IGNORED(L_GC),
	CR_MEMBER(watchUnitDefs),
	CR_MEMBER(watchFeatureDefs),
	CR_MEMBER(watchProjectileDefs),
	CR_MEMBER(watchExplosionDefs),
	CR_MEMBER(watchAllowTargetDefs),
	CR_MEMBER(delayedCallsByFrame),
	CR_SERIALIZER(Serialize)
))

void CLuaStateCollector::Read(const CSplitLuaHandle* handle) {
	valid = (handle != nullptr) && handle->syncedLuaHandle.IsValid();
	if (!valid)
		return;

	L = handle->syncedLuaHandle.GetLuaState();
	L_GC = handle->syncedLuaHandle.GetLuaGCState();
	watchUnitDefs = handle->syncedLuaHandle.watchUnitDefs;
	watchFeatureDefs = handle->syncedLuaHandle.watchFeatureDefs;
	watchProjectileDefs = handle->syncedLuaHandle.watchProjectileDefs;
	watchExplosionDefs = handle->syncedLuaHandle.watchExplosionDefs;
	watchAllowTargetDefs = handle->syncedLuaHandle.watchAllowTargetDefs;

	/* This container only holds indexes to the Lua registry, which is
	 * saved alongside the rest of the Lua state since it's fundamentally
	 * just a regular Lua table. So just a shallow copy is sufficient. */
	delayedCallsByFrame = handle->syncedLuaHandle.delayedCallsByFrame;

	// snapshots have to be equal on every client
	if (GameSnapshot::IsActive()) {
		creg::MakeCanonicalLuaState(L, L_GC);
	} else {
		lua_gc(L_GC, LUA_GCCOLLECT, 0);
	}
}

void CLuaStateCollector::Write(CSplitLuaHandle* handle) {
	if ((handle == nullptr) || !handle->syncedLuaHandle.IsValid() || !valid)
		return;

	handle->SwapSyncedHandle(L, L_GC);
	handle->syncedLuaHandle.watchUnitDefs = watchUnitDefs;
	handle->syncedLuaHandle.watchFeatureDefs = watchFeatureDefs;
	handle->syncedLuaHandle.watchProjectileDefs = watchProjectileDefs;
	handle->syncedLuaHandle.watchExplosionDefs = watchExplosionDefs;
	handle->syncedLuaHandle.watchAllowTargetDefs = watchAllowTargetDefs;
	handle->syncedLuaHandle.delayedCallsByFrame = delayedCallsByFrame;
}

void CLuaStateCollector::Serialize(creg::ISerializer* s) {
	if (!valid)
		return;

	creg::SerializeLuaState(s, &L);
	creg::SerializeLuaThread(s, &L_GC);
}

static void WriteString(std::ostream& s, const std::string& str)
{
	if (str.length() > MAX_STRING_SIZE)
		throw content_error("[creg::WriteString] string \"" + str + "\" too long");

	s.write(str.c_str(), str.length() + 1);
}

static void PrintSize(const char* txt, int size)
{
	if (size > (1024 * 1024 * 1024)) {
		LOG("%s %.1f GB", txt, size / (1024.0f * 1024 * 1024));
	} else if (size >  (1024 * 1024)) {
		LOG("%s %.1f MB", txt, size / (1024.0f * 1024));
	} else if (size > 1024) {
		LOG("%s %.1f KB", txt, size / (1024.0f));
	} else {
		LOG("%s %u B",    txt, size);
	}
}
#endif //USING_CREG

static void ReadString(std::istream& s, std::string& str)
{
	std::getline(s, str, '\0');
	if (str.length() > MAX_STRING_SIZE)
		throw content_error("[creg::ReadString] string too long");
}


static void SaveLuaState(CSplitLuaHandle* handle, creg::COutputStreamSerializer& os, std::stringstream& oss)
{
	CLuaStateCollector lsc;
	lsc.Read(handle);
	os.SavePackage(&oss, &lsc, lsc.GetClass());
}


namespace {
	/**
	 * Members of serialized objects that are specific to the local client.
	 * Snapshots store neutral values (so equal synced states give equal
	 * snapshots) and loading a snapshot keeps the local values.
	 */
	struct LocalClientState {
		// CGlobalUnsynced
		float modGameTime = 0.0f;
		float gameTime = 0.0f;
		float startTime = 0.0f;
		int myPlayerNum = 0;
		int myTeam = 0;
		int myAllyTeam = 0;
		int myPlayingTeam = -1;
		int myPlayingAllyTeam = -1;
		bool spectating = false;
		bool spectatingFullView = false;
		bool spectatingFullSelect = false;

		// CGame
		int lastSimFrame = -1;
		float totalGameTime = 0.0f;
		bool hideInterface = false;
		bool showFPS = false;
		bool showClock = false;
		bool showSpeed = false;
		bool noSpectatorChat = false;
		int speedControl = 0;
		int luaGCControl = 0;

		static LocalClientState Capture() {
			LocalClientState l;
			l.modGameTime = gu->modGameTime;
			l.gameTime = gu->gameTime;
			l.startTime = gu->startTime;
			l.myPlayerNum = gu->myPlayerNum;
			l.myTeam = gu->myTeam;
			l.myAllyTeam = gu->myAllyTeam;
			l.myPlayingTeam = gu->myPlayingTeam;
			l.myPlayingAllyTeam = gu->myPlayingAllyTeam;
			l.spectating = gu->spectating;
			l.spectatingFullView = gu->spectatingFullView;
			l.spectatingFullSelect = gu->spectatingFullSelect;

			l.lastSimFrame = game->lastSimFrame;
			l.totalGameTime = game->totalGameTime;
			l.hideInterface = game->hideInterface;
			l.showFPS = game->showFPS;
			l.showClock = game->showClock;
			l.showSpeed = game->showSpeed;
			l.noSpectatorChat = game->noSpectatorChat;
			l.speedControl = game->speedControl;
			l.luaGCControl = game->luaGCControl;
			return l;
		}

		void Apply() const {
			gu->modGameTime = modGameTime;
			gu->gameTime = gameTime;
			gu->startTime = startTime;
			gu->myPlayerNum = myPlayerNum;
			gu->myTeam = myTeam;
			gu->myAllyTeam = myAllyTeam;
			gu->myPlayingTeam = myPlayingTeam;
			gu->myPlayingAllyTeam = myPlayingAllyTeam;
			gu->spectating = spectating;
			gu->spectatingFullView = spectatingFullView;
			gu->spectatingFullSelect = spectatingFullSelect;

			game->lastSimFrame = lastSimFrame;
			game->totalGameTime = totalGameTime;
			game->hideInterface = hideInterface;
			game->showFPS = showFPS;
			game->showClock = showClock;
			game->showSpeed = showSpeed;
			game->noSpectatorChat = noSpectatorChat;
			game->speedControl = speedControl;
			game->luaGCControl = luaGCControl;
		}
	};
}

namespace {
	/**
	 * "Already processed" stamps (CGlobalSynced::GetTempNum, GetMtTempNum) are
	 * only compared for equality with the value of a running query; between
	 * frames none is running, so every stored stamp is equivalent to any value
	 * below the next counter value. Their actual values also depend on unsynced
	 * queries (e.g. from widgets), so snapshots store canonical ones: counters 0,
	 * stamps -1. Loading a snapshot continues the joining client's own counters,
	 * which are above every stamp it may still have in scratch data.
	 */
	class TempNumState {
	public:
		void Capture() {
			counters = gs->GetTempNumCounters();
			stamps.clear();
			repulserStamps.clear();

			ForEachObject([this](CWorldObject* o) { stamps.push_back({o->tempNum, o->mtTempNum}); });
			ForEachRepulser([this](CPlasmaRepulser* r) { repulserStamps.push_back(r->tempNum); });
		}

		void Restore() const {
			gs->SetTempNumCounters(counters);

			size_t i = 0;
			ForEachObject([&](CWorldObject* o) { o->tempNum = stamps[i].first; o->mtTempNum = stamps[i].second; i++; });
			i = 0;
			ForEachRepulser([&](CPlasmaRepulser* r) { r->tempNum = repulserStamps[i++]; });
		}

		static void ApplyCanonical() {
			CGlobalSynced::TempNumCounters c;
			c.tempNum = 0;
			c.mtTempNum.fill(0);
			gs->SetTempNumCounters(c);
			ResetStamps();
		}

		/// after loading a snapshot: canonical stamps, local counters
		void ContinueCounters() const {
			gs->SetTempNumCounters(counters);
			ResetStamps();
		}

	private:
		static void ResetStamps() {
			ForEachObject([](CWorldObject* o) { o->tempNum = -1; o->mtTempNum.fill(-1); });
			ForEachRepulser([](CPlasmaRepulser* r) { r->tempNum = -1; });
		}

		template<typename F> static void ForEachObject(F&& f) {
			for (CUnit* u: unitHandler.GetActiveUnits())
				f(u);
			for (const int id: featureHandler.GetActiveFeatureIDs())
				f(featureHandler.GetFeature(id));
			for (CProjectile* p: projectileHandler.GetActiveProjectiles(true))
				f(p);
		}

		template<typename F> static void ForEachRepulser(F&& f) {
			for (CUnit* u: unitHandler.GetActiveUnits()) {
				for (CWeapon* w: u->weapons) {
					if (CPlasmaRepulser* r = dynamic_cast<CPlasmaRepulser*>(w); r != nullptr)
						f(r);
				}
			}
		}

	private:
		CGlobalSynced::TempNumCounters counters = {};
		std::vector<std::pair<int, std::array<int, ThreadPool::MAX_THREADS>>> stamps;
		std::vector<int> repulserStamps;
	};
}

#ifdef USING_CREG
/// serializes the complete game state (everything after the textual header)
static void WriteGameState(std::stringstream& oss, std::vector<creg::COutputStreamSerializer::LayoutEntry>* layout)
{
	Sim::SaveComponents(oss);

	creg::COutputStreamSerializer os;
	os.SetLayoutRecorder(layout);

	// package boundaries, lets comparisons align the packages independently
	const auto MarkPackage = [&](uint64_t start, const char* name) {
		if (layout != nullptr)
			layout->push_back({start, static_cast<uint64_t>(oss.tellp()), "<package>", name});
	};

	// save lua state first as lua unit scripts depend on it
	const int luaStart = oss.tellp();
	SaveLuaState(luaGaia, os, oss);
	MarkPackage(luaStart, "LuaGaia");
	const int luaRulesStart = oss.tellp();
	SaveLuaState(luaRules, os, oss);
	MarkPackage(luaRulesStart, "LuaRules");
	PrintSize("Lua", ((int)oss.tellp()) - luaStart);

	// save creg state
	const int gameStart = oss.tellp();
	CGameStateCollector gsc;
	os.SavePackage(&oss, &gsc, gsc.GetClass());
	MarkPackage(gameStart, "Game");
	PrintSize("Game", ((int)oss.tellp()) - gameStart);


	// save AI state; only the hosting client has it, not part of snapshots
	const int aiStart = oss.tellp();

	for (const auto& ai: skirmishAIHandler.GetAllSkirmishAIs()) {
		if (GameSnapshot::IsActive())
			break;

		std::stringstream aiData;
		eoh->Save(&aiData, ai.first);

		std::uint64_t aiSize = aiData.tellp();
		creg::WriteUInt(&oss, aiSize);
		if (aiSize > 0)
			oss << aiData.rdbuf();
	}
	PrintSize("AIs", ((int)oss.tellp()) - aiStart);
}
#endif


static void LoadLuaState(CSplitLuaHandle* handle, creg::CInputStreamSerializer& is, std::stringstream& iss)
{
	void* plsc;
	creg::Class* plsccls = nullptr;

	if ((handle != nullptr) && handle->syncedLuaHandle.IsValid())
		creg::CopyLuaContext(handle->syncedLuaHandle.GetLuaState());

	is.LoadPackage(&iss, plsc, plsccls);
	assert(plsc && plsccls == CLuaStateCollector::StaticClass());
	CLuaStateCollector* lsc = static_cast<CLuaStateCollector*>(plsc);

	lsc->Write(handle);

	spring::SafeDelete(lsc);
}


void CCregLoadSaveHandler::SaveGame(const std::string& path)
{
#ifdef USING_CREG
	LOG("[LSH::%s] saving game to \"%s\"", __func__, path.c_str());

	// NB: Selection leaves CObject reference as Unit's listener,
	//     But isn't serialized - leak on load.
	selectedUnitsHandler.ClearSelected();

	try {
		std::stringstream oss;

		// write our own header. SavePackage() will add its own
		WriteString(oss, SpringVersion::GetSync());
		WriteString(oss, gameSetup->setupText);
		WriteString(oss, modName);
		WriteString(oss, mapName);


		WriteGameState(oss, nullptr);

		{
			gzFile file = gzopen(dataDirsAccess.LocateFile(path, FileQueryFlags::WRITE).c_str(), "wb5");

			if (file == nullptr) {
				LOG_L(L_ERROR, "[LSH::%s] could not open save-file", __func__);
				return;
			}

			std::string data = oss.str();
			std::function<void(gzFile, std::string&&)> func = [](gzFile file, std::string&& data) {
				gzwrite(file, data.c_str(), data.size());
				gzflush(file, Z_FINISH);
				gzclose(file);
			};

			// gzFile is just a plain typedef (struct gzFile_s {}* gzFile), can be copied
			// need to keep a reference to the future around or its destructor will block
			ThreadPool::AddExtJob(std::move(std::async(std::launch::async, std::move(func), file, std::move(data))));
		}

		//FIXME add lua state
	} catch (const content_error& ex) {
		LOG_L(L_ERROR, "[LSH::%s] content error \"%s\"", __func__, ex.what());
	} catch (const std::exception& ex) {
		LOG_L(L_ERROR, "[LSH::%s] exception \"%s\"", __func__, ex.what());
	} catch (const char*& exStr) {
		LOG_L(L_ERROR, "[LSH::%s] cstr error \"%s\"", __func__, exStr);
	} catch (const std::string& str) {
		LOG_L(L_ERROR, "[LSH::%s] str error \"%s\"", __func__, str.c_str());
	} catch (...) {
		LOG_L(L_ERROR, "[LSH::%s] unknown error", __func__);
	}
#else //USING_CREG
	LOG_L(L_ERROR, "[LSH::%s] creg is disabled", __func__);
#endif //USING_CREG
}

bool CCregLoadSaveHandler::WriteStateForComparison(const std::string& basePath, bool snapshotMode)
{
#ifdef USING_CREG
	// snapshot mode: exactly what a snapshot contains (to compare snapshots of different clients)
	if (!snapshotMode)
		selectedUnitsHandler.ClearSelected();

	const LocalClientState localState = LocalClientState::Capture();
	std::unique_ptr<GameSnapshot::ScopedActivation> snapshotActivation;

	TempNumState tempNums;
	tempNums.Capture();

	if (snapshotMode) {
		snapshotActivation = std::make_unique<GameSnapshot::ScopedActivation>();
		LocalClientState().Apply();
		TempNumState::ApplyCanonical();
	}

	bool ok = false;

	try {
		std::stringstream oss;
		std::vector<creg::COutputStreamSerializer::LayoutEntry> layout;

		WriteGameState(oss, &layout);

		const std::string data = oss.str();
		FILE* stateFile = fopen((basePath + ".state").c_str(), "wb");
		FILE* layoutFile = fopen((basePath + ".layout").c_str(), "w");

		if (stateFile != nullptr)
			fwrite(data.data(), 1, data.size(), stateFile);

		if (layoutFile != nullptr) {
			for (const auto& e: layout)
				fprintf(layoutFile, "%llu %llu %s %s\n", (unsigned long long) e.start, (unsigned long long) e.end, e.className, e.memberName);
		}

		if (stateFile != nullptr)
			fclose(stateFile);
		if (layoutFile != nullptr)
			fclose(layoutFile);

		ok = (stateFile != nullptr && layoutFile != nullptr);
	} catch (const std::exception& ex) {
		LOG_L(L_ERROR, "[LSH::%s] exception \"%s\"", __func__, ex.what());
	} catch (const std::string& str) {
		LOG_L(L_ERROR, "[LSH::%s] str error \"%s\"", __func__, str.c_str());
	}

	// also after an error: the client must not keep the canonical identity
	localState.Apply();
	tempNums.Restore();
	return ok;
#else
	return false;
#endif
}

bool CCregLoadSaveHandler::WriteSnapshot(std::string& raw)
{
#ifdef USING_CREG
	const LocalClientState localState = LocalClientState::Capture();
	TempNumState tempNums;
	tempNums.Capture();
	bool ok = false;

	try {
		GameSnapshot::ScopedActivation active;
		std::stringstream oss;

		// neutral values for client specific members
		LocalClientState().Apply();
		TempNumState::ApplyCanonical();

		WriteString(oss, GameSnapshot::MAGIC);
		WriteString(oss, SpringVersion::GetSync());
		WriteString(oss, gameSetup->setupText);
		WriteString(oss, gameSetup->modName);
		WriteString(oss, gameSetup->mapName);

		int32_t frameNum = gs->frameNum;
		oss.write(reinterpret_cast<const char*>(&frameNum), sizeof(frameNum));

		WriteGameState(oss, nullptr);
		raw = oss.str();
		ok = true;
	} catch (const content_error& ex) {
		LOG_L(L_ERROR, "[LSH::%s] content error \"%s\"", __func__, ex.what());
	} catch (const std::exception& ex) {
		LOG_L(L_ERROR, "[LSH::%s] exception \"%s\"", __func__, ex.what());
	} catch (const std::string& str) {
		LOG_L(L_ERROR, "[LSH::%s] str error \"%s\"", __func__, str.c_str());
	}

	localState.Apply();
	tempNums.Restore();
	return ok;
#else
	return false;
#endif
}

bool CCregLoadSaveHandler::LoadSnapshot(std::string&& raw, int32_t& frameNum)
{
	GameSnapshot::Header header;
	const size_t dataOffset = GameSnapshot::ReadHeader(raw, header);

	if (dataOffset == 0) {
		LOG_L(L_ERROR, "[LSH::%s] invalid snapshot header", __func__);
		return false;
	}
	if (header.syncVersion != SpringVersion::GetSync()) {
		LOG_L(L_ERROR, "[LSH::%s] snapshot of engine version \"%s\" (local \"%s\")", __func__, header.syncVersion.c_str(), SpringVersion::GetSync().c_str());
		return false;
	}

	scriptText = std::move(header.setupText);
	modName = std::move(header.modName);
	mapName = std::move(header.mapName);
	frameNum = header.frameNum;
	joinMode = true;

	// creg packages refer to absolute stream positions (as written by WriteSnapshot)
	iss.str(std::move(raw));
	iss.clear();
	iss.seekg(dataOffset);
	return true;
}

/// loads the data (map&mod-name,setup-script) needed by PreGame
bool CCregLoadSaveHandler::LoadGameStartInfo(const std::string& path)
{
	if (FileSystem::GetExtensionLowerCase(path) == "srs") {
		// a raw snapshot (see SimStateTestHooks), loaded like a joining client would
		CFileHandler snapshotFile(dataDirsAccess.LocateFile(FindSaveFile(path)), SPRING_VFS_RAW_FIRST);
		std::string raw;
		int32_t frameNum = -1;

		if (!snapshotFile.FileExists() || !snapshotFile.LoadStringData(raw))
			return false;
		if (!LoadSnapshot(std::move(raw), frameNum))
			return false;

		CGameSetup::LoadSavedScript(path, scriptText);
		return true;
	}

	CGZFileHandler saveFile(dataDirsAccess.LocateFile(FindSaveFile(path)), SPRING_VFS_RAW_FIRST);

	std::stringbuf* sbuf = iss.rdbuf();
	std::string saveVersion;
	std::string syncVersion = SpringVersion::GetSync();

	char buf[4096];
	int len;
	while ((len = saveFile.Read(buf, sizeof(buf))) > 0)
		sbuf->sputn(buf, len);

	ReadString(iss, saveVersion);

	// check saved engine version against current build
	// in general these will *not* be binary-compatible
	// (so prefer to terminate loading from PreGame)
	if (saveVersion != syncVersion)
		LOG_L(L_WARNING, "[LSH::%s][release=%d] file \"%s\" saved by engine version \"%s\" incompatible with \"%s\"", __func__, SpringVersion::IsRelease(), path.c_str(), saveVersion.c_str(), syncVersion.c_str());

	// read our own header
	ReadString(iss, scriptText);
	ReadString(iss, modName);
	ReadString(iss, mapName);

	CGameSetup::LoadSavedScript(path, scriptText);
	return (saveVersion == syncVersion);
}

/// this should be called on frame 0 when the game has started
void CCregLoadSaveHandler::LoadGame()
{
#ifdef USING_CREG
	ENTER_SYNCED_CODE();

	// a joining client keeps its identity and unsynced settings
	const LocalClientState localState = LocalClientState::Capture();
	std::unique_ptr<GameSnapshot::ScopedActivation> snapshotActivation;
	TempNumState localTempNums;

	if (joinMode) {
		snapshotActivation = std::make_unique<GameSnapshot::ScopedActivation>();
		localTempNums.Capture();
	}

	{
		Sim::LoadComponents(iss);

		creg::CInputStreamSerializer inputStream;

		// load lua state first, as lua unit scripts depend on it
		LoadLuaState(luaGaia, inputStream, iss);
		LoadLuaState(luaRules, inputStream, iss);

		// load creg state
		void* pGSC = nullptr;
		creg::Class* gsccls = nullptr;

		inputStream.LoadPackage(&iss, pGSC, gsccls);
		assert(pGSC && gsccls == CGameStateCollector::StaticClass());

		// the only job of gsc is to collect gamestate data
		CGameStateCollector* gsc = static_cast<CGameStateCollector*>(pGSC);
		spring::SafeDelete(gsc);
	}

	if (joinMode) {
		localState.Apply();
		localTempNums.ContinueCounters();
	}

#ifdef SYNCCHECK
	// see SerializeSyncedGlobals
	CSyncChecker::SetChecksum(loadedChecksum);
	CSyncChecker::SetPrevChecksum(loadedPrevChecksum);
#endif

	LEAVE_SYNCED_CODE();
#else //USING_CREG
	LOG_L(L_ERROR, "Load failed: creg is disabled");
#endif //USING_CREG
}

/// this should be called on frame 0 when the game has started
void CCregLoadSaveHandler::LoadAIData()
{
#ifdef USING_CREG
	ENTER_SYNCED_CODE();

	// snapshots contain no AI data and the game simply continues (paused or not)
	if (joinMode) {
		iss.str("");
		LEAVE_SYNCED_CODE();
		return;
	}

	// load ai state
	for (const auto& ai: skirmishAIHandler.GetAllSkirmishAIs()) {
		std::uint64_t aiSize;
		creg::ReadUInt(&iss, &aiSize);

		std::vector<char> buffer(aiSize);
		std::stringstream aiData;
		iss.read(buffer.data(), buffer.size());
		aiData.write(buffer.data(), buffer.size());

		eoh->Load(&aiData, ai.first);
	}

	// cleanup
	iss.str("");

	gs->paused = false;
	if (gameServer != nullptr) {
		gameServer->isPaused = false;
		gameServer->syncErrorFrame = 0;
	}

	LEAVE_SYNCED_CODE();
#else //USING_CREG
	LOG_L(L_ERROR, "Load failed: creg is disabled");
#endif //USING_CREG
}
