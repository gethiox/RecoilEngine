/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/**
 * Exact (de)serialization of the QTPFS path manager.
 *
 * The goal is that a game loaded from a saved state continues *bit-exactly*
 * like the game it was saved from, which rules out rebuilding any of the
 * pathing state on load:
 *
 *  - node layers are updated incrementally from damage queues, so their
 *    tesselation depends on history and may lag behind the terrain
 *  - path entities are processed in the packed order of their ECS storages
 *    (including the owning group <PathSearch, ProcessPath>), so storages are
 *    restored element by element in their original order
 *  - regular searches run as a background task spanning frame boundaries;
 *    the writer waits for that task, the staged results are stored and are
 *    applied by SyncUpdatedPathsSystem at the start of the next frame just
 *    like in the original game
 *
 * Only data needed by the simulation is stored; scratch buffers (search
 * thread data, update thread data, debug traces) are reset.
 */

#include "PathManager.h"
#include "PathManagerSerializer.h"

#include <algorithm>
#include <tuple>
#include <type_traits>

#include "Components/Path.h"
#include "Components/PathSpeedModInfo.h"
#include "Components/RemoveDeadPaths.h"
#include "Components/SyncUpdatedPaths.h"
#include "Registry.h"
#include "Sim/Objects/SolidObject.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "System/creg/RawSerialize.h"
#include "System/Exceptions.h"
#include "System/Log/ILog.h"
#include "System/Threading/ThreadPool.h"

namespace QTPFS {

using Registry = entt::basic_registry<QTPFS::entity>;
using EntityType = entt::entt_traits<QTPFS::entity>::entity_type;

/// every component type that may live in a QTPFS registry; storages are
/// restored in this order (PathSearch must precede ProcessPath, see header)
#define QTPFS_SERIALIZED_COMPONENTS \
	IPath, UnsyncedIPath, ExternallyManagedSyncedIPath, SearchModeIPath, \
	PathSearch, UnsyncedPathSearch, ExternallyManagedPathSearch, \
	ProcessPath, PathSearchRef, PathIsDirty, PathIsTemp, PathIsToBeUpdated, \
	PathUpdatedCounterIncrease, PathRequeueSearch, PathDelayedDelete, \
	PendingPathRequestDetails, SharedPathChain, PartialSharedPathChain, \
	ExternalPathEntityId, InternalPathEntityId, \
	NodeLayerSpeedInfoSweep, PathSpeedModInfoSystemComponent, \
	RemoveDeadPathsComponent, SyncUpdatedPathsComponent

static constexpr uint32_t MARKER_LAYERS   = 0x51545031; // QTP1
static constexpr uint32_t MARKER_REGISTRY = 0x51545032; // QTP2
static constexpr uint32_t MARKER_MANAGER  = 0x51545033; // QTP3
static constexpr uint32_t MARKER_END      = 0x51545034; // QTP4


/// glue between entt's snapshot interface and creg::RawArchive
class PathManagerSerializer::EnttArchive {
public:
	EnttArchive(creg::RawArchive& a, PathManagerSerializer& p): ar(a), pms(p) {}

	template<typename T> void operator()(T& v) { pms.Io(ar, v); }
	template<typename T> void operator()(const T& v) { pms.Io(ar, const_cast<T&>(v)); }

private:
	creg::RawArchive& ar;
	PathManagerSerializer& pms;
};


template<typename T> void PathManagerSerializer::Io(creg::RawArchive& ar, T& v)
{
	// entities, alias components and plain system components; only types
	// without padding may be stored as raw memory (padding bytes are not
	// initialized and would make otherwise identical states differ)
	static_assert(std::is_trivially_copyable_v<T>, "QTPFS component needs a dedicated serializer");
	static_assert(std::is_empty_v<T> || sizeof(T) <= 8 || std::is_same_v<T, PathSpeedModInfoSystemComponent>,
		"larger QTPFS component, check for padding and add a dedicated serializer");
	ar.Value(v);
}

void PathManagerSerializer::Io(creg::RawArchive& ar, RemoveDeadPathsComponent& c)
{
	ar.Value(c.refreshRate);
	ar.Value(c.refreshOffset);
	ar.Value(c.deleteAll);
}

void PathManagerSerializer::Io(creg::RawArchive& ar, NodeLayerSpeedInfoSweep& c)
{
	ar.Value(c.updateMaxNodes);
	ar.Value(c.updateCurMaxSpeed);
	ar.Value(c.updateCurSumSpeed);
	ar.Value(c.updateNumLeafNodes);
	ar.Value(c.layerNum);
	ar.Value(c.updateInProgress);
}

void PathManagerSerializer::IoPathNode(creg::RawArchive& ar, IPath::PathNodeData& n)
{
	ar.Value(n.nodeId);
	ar.Value(n.nodeNumber);
	ar.Value(n.netPoint);
	ar.Value(n.pathPointIndex);
	ar.Value(n.xmin);
	ar.Value(n.zmin);
	ar.Value(n.xmax);
	ar.Value(n.zmax);
	ar.Value(n.badNode);
}

namespace {
	/**
	 * Stands in for path owners that were destroyed while their path still
	 * existed ("units can get wiped without triggering a delete", see
	 * PathManager.cpp). The path manager only reads objectUsable from such
	 * owners, so the joining client behaves like the source, which still holds
	 * the destroyed unit's pointer. An unsynced object: it must not consume a
	 * synced sync-id on the joining client.
	 */
	class DeadPathOwner: public CSolidObject {
	public:
		DeadPathOwner(): CSolidObject(false) { objectUsable = false; }

		void ForcedMove(const float3& newPos) override {}
		CMatrix44f GetTransformMatrix(bool synced = false, bool fullread = false) const override { return CMatrix44f(); }
	};

	const CSolidObject* GetDeadPathOwner()
	{
		static DeadPathOwner* owner = nullptr;

		if (owner == nullptr)
			owner = new DeadPathOwner();

		return owner;
	}

	constexpr int NO_OWNER = -1;
	constexpr int DEAD_OWNER = -2;
}

void PathManagerSerializer::IoOwner(creg::RawArchive& ar, const CSolidObject*& owner)
{
	int unitID = NO_OWNER;

	if (ar.IsWriting() && owner != nullptr) {
		// the pointer is only dereferenced if the unit is known to be alive;
		// path requests always come from units
		if (liveOwners.find(owner) != liveOwners.end()) {
			unitID = static_cast<const CUnit*>(owner)->id;
		} else {
			unitID = DEAD_OWNER;
		}
	}

	ar.Value(unitID);

	if (ar.IsWriting())
		return;

	owner = nullptr;

	if (unitID == DEAD_OWNER) {
		owner = GetDeadPathOwner();
		return;
	}

	if (unitID < 0)
		return;

	// units are deserialized before the path manager, so the (pointer
	// stable) object already exists even if its data is read later
	if ((owner = unitHandler.GetUnit(unitID)) == nullptr)
		throw content_error("[QTPFS::PathManagerSerializer] path owned by unknown unit");
}

void PathManagerSerializer::IoPath(creg::RawArchive& ar, IPath& p)
{
	ar.Value(p.pathID);
	ar.Value(p.pathType);
	ar.Value(p.nextPointIndex);
	ar.Value(p.repathAtPointIndex);
	ar.Value(p.numPathUpdates);
	ar.Value(p.firstNodeIdOfCleanPath);
	ar.Value(p.hash);
	ar.Value(p.virtualHash);
	ar.Value(p.radius);
	ar.Value(p.synced);
	ar.Value(p.haveFullPath);
	ar.Value(p.havePartialPath);
	ar.Value(p.boundingBoxOverride);
	ar.Value(p.isRawPath);
	ar.Value(p.repathTriggered);
	ar.PodContainer(p.points);
	ar.Container(p.nodes, [this](creg::RawArchive& a, IPath::PathNodeData& n) { IoPathNode(a, n); });
	ar.Value(p.boundingBoxMins);
	ar.Value(p.boundingBoxMaxs);
	ar.Value(p.goalPosition);
	IoOwner(ar, p.owner);
	// searchTime is a wall-clock debug value, not stored
}

void PathManagerSerializer::Io(creg::RawArchive& ar, IPath& p) { IoPath(ar, p); }
void PathManagerSerializer::Io(creg::RawArchive& ar, UnsyncedIPath& p) { IoPath(ar, p); }
void PathManagerSerializer::Io(creg::RawArchive& ar, ExternallyManagedSyncedIPath& p) { IoPath(ar, p); }
void PathManagerSerializer::Io(creg::RawArchive& ar, SearchModeIPath& p) { IoPath(ar, p); }

void PathManagerSerializer::IoSearch(creg::RawArchive& ar, PathSearch& s)
{
	ar.Value(s.searchID);
	ar.Value(s.searchTeam);
	ar.Value(s.searchType);
	ar.Value(s.searchState);
	ar.Value(s.pathSearchHash);
	ar.Value(s.pathPartialSearchHash);
	IoOwner(ar, s.pathOwner);
	ar.Value(s.pathType);

	bool hasNodeLayer = (s.nodeLayer != nullptr);
	ar.Value(hasNodeLayer);

	for (auto& dsd: s.directionalSearchData) {
		ar.Value(dsd.srcPoint);
		ar.Value(dsd.tgtPoint);
	}

	ar.Array(s.netPoints, std::size(s.netPoints));
	ar.Value(s.goalPos);
	ar.Value(s.searchLimitMins);
	ar.Value(s.searchLimitMaxs);
	ar.Array(s.gDists, std::size(s.gDists));
	ar.Array(s.hDists, std::size(s.hDists));
	ar.Array(s.gCosts, std::size(s.gCosts));
	ar.Array(s.hCosts, std::size(s.hCosts));
	ar.Value(s.hCostMult);
	ar.Value(s.goalDistance);
	ar.Value(s.adjustedGoalDistance);
	ar.Value(s.fwdStepIndex);
	ar.Value(s.bwdStepIndex);
	ar.Value(s.nodeSearchLimit);
	ar.Value(s.fwdNodesSearched);
	ar.Value(s.bwdNodesSearched);
	ar.Value(s.haveFullPath);
	ar.Value(s.havePartPath);
	ar.Value(s.badGoal);
	ar.Value(s.rawPathCheck);
	ar.Value(s.synced);
	ar.Value(s.pathRequestWaiting);
	ar.Value(s.doPartialSearch);
	ar.Value(s.tryPathRepair);
	ar.Value(s.rejectPartialSearch);
	ar.Value(s.allowPartialSearch);
	ar.Value(s.expectIncompletePartialSearch);
	ar.Value(s.searchEarlyDrop);
	ar.Value(s.initialized);
	ar.Value(s.partialReverseTrace);
	ar.Value(s.doPathRepair);
	ar.Value(s.fwdPathConnected);
	ar.Value(s.bwdPathConnected);
	ar.Value(s.useFwdPathOnly);
	ar.Value(s.fullSharedPathHead);
	ar.Value(s.partSharedPathHead);

	if (ar.IsWriting())
		return;

	// execution scratch state; searches are either completed (results are
	// staged in flags and SearchModeIPath) or not yet initialized
	if (hasNodeLayer) {
		if (s.pathType < 0 || s.pathType >= static_cast<int>(pm.nodeLayers.size()))
			throw content_error("[QTPFS::PathManagerSerializer] invalid search path type");

		s.nodeLayer = &pm.nodeLayers[s.pathType];
	} else {
		s.nodeLayer = nullptr;
	}

	s.searchThreadData = nullptr;
	s.searchExec = nullptr;
	s.searchIter = {};
	s.curSearchNode = nullptr;
	s.nextSearchNode = nullptr;

	for (auto& dsd: s.directionalSearchData) {
		dsd.openNodes = nullptr;
		dsd.srcSearchNode = nullptr;
		dsd.tgtSearchNode = nullptr;
		dsd.minSearchNode = nullptr;
		dsd.repairPathRealSrcSearchNode = nullptr;
	}
}

void PathManagerSerializer::Io(creg::RawArchive& ar, PathSearch& s) { IoSearch(ar, s); }
void PathManagerSerializer::Io(creg::RawArchive& ar, UnsyncedPathSearch& s) { IoSearch(ar, s); }
void PathManagerSerializer::Io(creg::RawArchive& ar, ExternallyManagedPathSearch& s) { IoSearch(ar, s); }

void PathManagerSerializer::Io(creg::RawArchive& ar, PendingPathRequestDetails& d)
{
	IoOwner(ar, d.object);
	ar.Value(d.startPos);
	ar.Value(d.endPos);
	ar.Value(d.radius);
	ar.Value(d.pathType);
	ar.Value(d.synced);
	ar.Value(d.externalRequest);
	ar.Value(d.allowRawSearch);
}

void PathManagerSerializer::Io(creg::RawArchive& ar, SyncUpdatedPathsComponent& c)
{
	// the background task was completed before writing, see Serialize()
	if (ar.IsReading())
		c.backgroundTask.reset();
}


template<typename... Components>
static void CheckKnownStorages(Registry& reg, const char* regName)
{
	const entt::id_type known[] = {entt::type_hash<QTPFS::entity>::value(), entt::type_hash<Components>::value()...};

	for (auto&& [id, storage]: reg.storage()) {
		if (storage.empty())
			continue;
		if (std::find(std::begin(known), std::end(known), id) != std::end(known))
			continue;

		// a new component type was added to QTPFS without teaching the
		// serializer about it; refuse rather than produce a wrong state
		throw content_error(std::string("[QTPFS::PathManagerSerializer] unhandled component storage in ") + regName + ": " + std::string(storage.info().name()));
	}
}

template<typename... Components>
void PathManagerSerializer::IoRegistryWith(creg::RawArchive& ar, Registry& reg, const char* regName)
{
	EnttArchive archive(ar, *this);

	if (ar.IsWriting()) {
		CheckKnownStorages<Components...>(reg, regName);

		entt::basic_snapshot snapshot{reg};
		snapshot.template get<QTPFS::entity>(archive);
		(snapshot.template get<Components>(archive), ...);
		return;
	}

	// clear() keeps the registry's own entities as free entries, which would
	// shift the ids allocated after the load: empty the entity storage too
	reg.clear();
	reg.storage<QTPFS::entity>().clear();

	entt::basic_snapshot_loader loader{reg};
	loader.template get<QTPFS::entity>(archive);
	(loader.template get<Components>(archive), ...);
}

void PathManagerSerializer::IoRegistry(creg::RawArchive& ar, Registry& reg, const char* regName)
{
	ar.Marker(MARKER_REGISTRY);
	IoRegistryWith<QTPFS_SERIALIZED_COMPONENTS>(ar, reg, regName);
}


void PathManagerSerializer::IoNodeLayer(creg::RawArchive& ar, NodeLayer& nl)
{
	ar.Value(nl.layerNumber);
	ar.Value(nl.numLeafNodes);
	ar.Value(nl.updateCounter);
	ar.Value(nl.numOpenNodes);
	ar.Value(nl.numClosedNodes);
	ar.Value(nl.maxNodesAlloced);
	ar.Value(nl.numRootNodes);
	ar.Value(nl.xRootNodes);
	ar.Value(nl.zRootNodes);
	ar.Value(nl.rootNodeSize);
	ar.Value(nl.rootMask);
	ar.Value(nl.xsize);
	ar.Value(nl.zsize);
	ar.Value(nl.maxRelSpeedMod);
	ar.Value(nl.avgRelSpeedMod);
	ar.Value(nl.useShortestPath);

	if (nl.maxNodesAlloced < 0 || nl.maxNodesAlloced > static_cast<int32_t>(POOL_TOTAL_SIZE))
		throw content_error("[QTPFS::PathManagerSerializer] invalid node count");

	ar.PodContainer(nl.nodeIndcs, POOL_TOTAL_SIZE);
	ar.FixedPodContainer(nl.curSpeedMods);
	ar.FixedPodContainer(nl.curSpeedBins);

	if (ar.Count(nl.mapSquareStatusCache.size()) != nl.mapSquareStatusCache.size())
		throw content_error("[QTPFS::PathManagerSerializer] square status cache size mismatch");

	// MapSquareData is a plain byte wrapper with member functions
	using MapSquareData = NodeLayer::MapSquareData;
	static_assert(std::is_standard_layout_v<MapSquareData> && sizeof(MapSquareData) == sizeof(SpeedBinType));

	for (auto& sector: nl.mapSquareStatusCache)
		ar.Array(reinterpret_cast<SpeedBinType*>(sector.data()), sector.size());

	// nodes; everything up to the high-water mark, free ones included since
	// re-allocation does not necessarily reset every field
	for (uint32_t i = 0; i < static_cast<uint32_t>(nl.maxNodesAlloced); i++) {
		auto& chunk = nl.poolNodes[i / NodeLayer::POOL_CHUNK_SIZE];

		if (ar.IsReading() && chunk.empty())
			chunk.resize(NodeLayer::POOL_CHUNK_SIZE);

		INode& n = chunk[i % NodeLayer::POOL_CHUNK_SIZE];

		ar.Value(n.nodeNumber);
		ar.Value(n.index);
		ar.Value(n.points);
		ar.Value(n.moveCostAvg);
		ar.Value(n.childBaseIndex);

		const uint32_t numNeighbours = ar.Count(n.neighbours.size(), POOL_TOTAL_SIZE);

		if (ar.IsReading()) {
			n.neighbours.clear();
			n.neighbours.resize(numNeighbours);
		}

		ar.Array(n.neighbours.data(), numNeighbours);
	}

	if (ar.IsReading()) {
		// scratch vectors, refilled on every update
		nl.selectedNodes.clear();
		nl.openNodes.clear();
	}
}

void PathManagerSerializer::IoManager(creg::RawArchive& ar)
{
	ar.Marker(MARKER_MANAGER);

	auto& tracker = pm.nodeLayersMapDamageTrack;
	ar.Value(tracker.width);
	ar.Value(tracker.height);
	ar.Value(tracker.cellSize);

	if (ar.Count(tracker.mapChangeTrackers.size()) != tracker.mapChangeTrackers.size())
		throw content_error("[QTPFS::PathManagerSerializer] move-def count mismatch");

	for (auto& mct: tracker.mapChangeTrackers) {
		// vector<bool> is bit-packed, serialize it element-wise
		const uint32_t n = ar.Count(mct.damageMap.size());

		if (n != mct.damageMap.size())
			throw content_error("[QTPFS::PathManagerSerializer] damage map size mismatch");

		for (uint32_t i = 0; i < n; i++) {
			bool b = mct.damageMap[i];
			ar.Value(b);
			mct.damageMap[i] = b;
		}

		ar.Container(mct.damageQueue, [](creg::RawArchive& a, int& v) { a.Value(v); });
	}

	if (ar.Count(pm.pathCache.dirtyPaths.size()) != pm.pathCache.dirtyPaths.size())
		throw content_error("[QTPFS::PathManagerSerializer] dirty-path layer count mismatch");

	for (auto& layerDirtyPaths: pm.pathCache.dirtyPaths) {
		ar.Container(layerDirtyPaths, [](creg::RawArchive& a, PathCache::DirtyPathDetail& d) {
			a.Value(d.pathEntity);
			a.Value(d.autoRepathTrigger);
			a.Value(d.nodesAreCleanFromNodeId);
			a.Value(d.clearSharing);
			a.Value(d.clearPath);
		});
	}

	// shared path maps are only used for lookups, store them sorted by key
	const auto IoSharedMap = [&ar](auto& map) {
		using Map = std::remove_reference_t<decltype(map)>;
		std::vector<std::pair<typename Map::key_type, typename Map::mapped_type>> entries;

		if (ar.IsWriting()) {
			entries.assign(map.begin(), map.end());
			std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		}

		ar.Container(entries, [](creg::RawArchive& a, auto& e) { a.Value(e.first); a.Value(e.second); });

		if (ar.IsReading()) {
			map.clear();

			for (const auto& e: entries)
				map[e.first] = e.second;
		}
	};

	IoSharedMap(pm.sharedPaths);
	IoSharedMap(pm.partialSharedPaths);

	ar.Value(pm.deadPathsToUpdatePerFrame);
	ar.Value(pm.recalcDeadPathUpdateRateOnFrame);
	ar.Value(pm.rootSize);
	ar.Value(pm.searchStateOffset);
	ar.Value(pm.numPathRequests);
	ar.Value(pm.refreshDirtyPathRateFrame);
	ar.Value(pm.updateDirtyPathRate);
	ar.Value(pm.updateDirtyPathRemainder);
	ar.Value(pm.pfsCheckSum);
	ar.Value(pm.extReserveEntity);
	ar.Value(pm.systemEntity);

	// the system-globals helper only stores which entity holds the system components
	EnttArchive archive(ar, *this);
	systemGlobals.serialize(archive);
}

void PathManagerSerializer::Serialize(creg::ISerializer* s)
{
	creg::RawArchive ar(s);

	liveOwners.clear();

	if (ar.IsWriting()) {
		for (unsigned int i = 0; i < unitHandler.MaxUnits(); i++) {
			if (const CUnit* unit = unitHandler.GetUnit(i); unit != nullptr)
				liveOwners.insert(unit);
		}

		// staged results of in-flight searches have to be complete before
		// they can be stored; this does not change the outcome, the next
		// frame would wait for the very same task
		if (systemGlobals.IsSystemActive<SyncUpdatedPathsComponent>()) {
			auto& comp = systemGlobals.GetSystemComponent<SyncUpdatedPathsComponent>();

			if (comp.backgroundTask) {
				wait_for_mt_background(comp.backgroundTask);
				comp.backgroundTask.reset();
			}
		}
	}

	ar.Marker(MARKER_LAYERS);

	if (ar.Count(pm.nodeLayers.size()) != pm.nodeLayers.size())
		throw content_error("[QTPFS::PathManagerSerializer] node layer count mismatch");

	for (NodeLayer& nl: pm.nodeLayers)
		IoNodeLayer(ar, nl);

	IoManager(ar);
	IoRegistry(ar, registry, "registry");
	IoRegistry(ar, externalRegistry, "externalRegistry");

	ar.Marker(MARKER_END);

	if (ar.IsWriting())
		return;

	// debug-only traces refer to searches of the original process
	for (auto& trace: pm.pathTraces)
		delete trace.second;

	pm.pathTraces.clear();

	// search scratch buffers are sized by the node high-water mark
	int maxAllocedNodes = 0;

	for (const NodeLayer& nl: pm.nodeLayers)
		maxAllocedNodes = std::max(nl.GetMaxNodesAlloced(), maxAllocedNodes);

	pm.searchThreadData.clear();

	for (int threads = ThreadPool::GetNumThreads(); threads-- > 0; ) {
		pm.searchThreadData.emplace_back(SearchThreadData(maxAllocedNodes, threads));
	}
}

}
