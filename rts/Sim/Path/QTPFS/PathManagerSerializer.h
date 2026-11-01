/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef QTPFS_PATHMANAGER_SERIALIZER_HDR
#define QTPFS_PATHMANAGER_SERIALIZER_HDR

#include "Registry.h"
#include "Path.h"

class CSolidObject;

namespace creg {
	class ISerializer;
	class RawArchive;
}

namespace QTPFS {
	class PathManager;
	struct NodeLayer;
	struct IPath;
	struct UnsyncedIPath;
	struct ExternallyManagedSyncedIPath;
	struct SearchModeIPath;
	struct PathSearch;
	struct UnsyncedPathSearch;
	struct ExternallyManagedPathSearch;
	struct PendingPathRequestDetails;
	struct SyncUpdatedPathsComponent;
	struct RemoveDeadPathsComponent;
	struct NodeLayerSpeedInfoSweep;

	/**
	 * @brief Exact (de)serialization of all QTPFS simulation state.
	 *
	 * Friend of the QTPFS types whose internals it stores; see the .cpp
	 * for the rationale behind what is (and is not) serialized.
	 */
	class PathManagerSerializer {
	public:
		explicit PathManagerSerializer(PathManager& p): pm(p) {}

		void Serialize(creg::ISerializer* s);

	private:
		class EnttArchive;

		void IoNodeLayer(creg::RawArchive& ar, NodeLayer& nl);
		void IoManager(creg::RawArchive& ar);
		void IoRegistry(creg::RawArchive& ar, entt::basic_registry<QTPFS::entity>& reg, const char* regName);

		template<typename... Components>
		void IoRegistryWith(creg::RawArchive& ar, entt::basic_registry<QTPFS::entity>& reg, const char* regName);

		void IoOwner(creg::RawArchive& ar, const CSolidObject*& owner);
		void IoPath(creg::RawArchive& ar, IPath& p);
		void IoSearch(creg::RawArchive& ar, PathSearch& s);

		template<typename T> void Io(creg::RawArchive& ar, T& v);
		void Io(creg::RawArchive& ar, IPath& p);
		void Io(creg::RawArchive& ar, UnsyncedIPath& p);
		void Io(creg::RawArchive& ar, ExternallyManagedSyncedIPath& p);
		void Io(creg::RawArchive& ar, SearchModeIPath& p);
		void Io(creg::RawArchive& ar, PathSearch& s);
		void Io(creg::RawArchive& ar, UnsyncedPathSearch& s);
		void Io(creg::RawArchive& ar, ExternallyManagedPathSearch& s);
		void Io(creg::RawArchive& ar, PendingPathRequestDetails& d);
		void Io(creg::RawArchive& ar, SyncUpdatedPathsComponent& c);
		void Io(creg::RawArchive& ar, RemoveDeadPathsComponent& c);
		void Io(creg::RawArchive& ar, NodeLayerSpeedInfoSweep& c);
		void IoPathNode(creg::RawArchive& ar, IPath::PathNodeData& n);

	private:
		PathManager& pm;
	};
}

#endif
