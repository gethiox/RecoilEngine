/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef MOVE_TYPE_COMPONENTS_H__
#define MOVE_TYPE_COMPONENTS_H__

#include "MoveTypesEvents.h"
#include "System/Ecs/Components/BaseComponents.h"
#include <System/Threading/ThreadPool.h>

#include <cstdint>
#include <stdexcept>

class CUnit;
class CFeature;

namespace MoveTypes {

// For move types that need to be handled single threaded.
ALIAS_COMPONENT(GeneralMoveType, int);

// Special multi-thread ground move type.
ALIAS_COMPONENT(GroundMoveType, int);

// Used by units that have updated the ground collision map and may have trapped units as a result.
// This is used to allow such a situation to be detected immediately. The fall-back checks are too
// slow in practice.
enum UnitTrapCheckType {
    TRAPPER_IS_UNIT,
    TRAPPER_IS_FEATURE
};
struct UnitTrapCheck {
    UnitTrapCheckType type;
    int id;
};

template<class Archive>
void serialize(Archive &ar, UnitTrapCheck &c) { ar(c.type, c.id); }

template<class Archive>
void serialize(Archive &ar, ChangeHeadingEvent &c) { ar(c.unitId, c.deltaHeading, c.changed); }

template<class Archive>
void serialize(Archive &ar, ChangeMainHeadingEvent &c) { ar(c.unitId, c.changed); }

template<class Archive>
void serialize(Archive &ar, UnitMovedEvent &c) {
    // only ever set and reset within a frame
    bool moved = c.moved;
    ar(moved);
    c.moved = moved;
}

/// event lists are filled and consumed within GroundMoveSystem::Update, always empty in between
template<class Archive, class EventList>
void serializeEmptyEventList(Archive &ar, EventList &c) {
    std::uint32_t size = c.value.size();
    ar(size);

    if (size != 0)
        throw std::runtime_error("[MoveTypes::serializeEmptyEventList] pending move type events");
}


struct YardmapTrapCheckSystemComponent {
	static constexpr std::size_t page_size = 1;
    static constexpr std::size_t INITIAL_TRAP_UNIT_LIST_ALLOC_SIZE = 8;

	std::array<std::vector<CUnit*>, ThreadPool::MAX_THREADS> trappedUnitLists;
};

constexpr size_t UNIT_EVENT_VECTOR_RESERVE = 4;

ALIAS_COMPONENT_LIST_RESERVE(FeatureCollisionEvents, std::vector<FeatureCollisionEvent>, UNIT_EVENT_VECTOR_RESERVE);
ALIAS_COMPONENT_LIST_RESERVE(UnitCollisionEvents, std::vector<UnitCollisionEvent>, UNIT_EVENT_VECTOR_RESERVE);
ALIAS_COMPONENT_LIST_RESERVE(FeatureCrushEvents, std::vector<FeatureCrushEvent>, UNIT_EVENT_VECTOR_RESERVE);
ALIAS_COMPONENT_LIST_RESERVE(UnitCrushEvents, std::vector<UnitCrushEvent>, UNIT_EVENT_VECTOR_RESERVE);
ALIAS_COMPONENT_LIST_RESERVE(FeatureMoveEvents, std::vector<FeatureMoveEvent>, UNIT_EVENT_VECTOR_RESERVE);

template<class Archive> void serialize(Archive &ar, FeatureCollisionEvents &c) { serializeEmptyEventList(ar, c); }
template<class Archive> void serialize(Archive &ar, UnitCollisionEvents &c) { serializeEmptyEventList(ar, c); }
template<class Archive> void serialize(Archive &ar, FeatureCrushEvents &c) { serializeEmptyEventList(ar, c); }
template<class Archive> void serialize(Archive &ar, UnitCrushEvents &c) { serializeEmptyEventList(ar, c); }
template<class Archive> void serialize(Archive &ar, FeatureMoveEvents &c) { serializeEmptyEventList(ar, c); }

/**
 * Systems iterate components in storage order, so every storage is saved
 * (and restored in the same order), also those that only hold transient
 * per-frame data.
 */
template<class Archive, class Snapshot>
void serializeComponents(Archive &archive, Snapshot &snapshot) {
    snapshot
        .template get<GeneralMoveType>(archive)
        .template get<GroundMoveType>(archive)
        .template get<UnitTrapCheck>(archive)
        .template get<ChangeHeadingEvent>(archive)
        .template get<ChangeMainHeadingEvent>(archive)
        .template get<UnitMovedEvent>(archive)
        .template get<FeatureCollisionEvents>(archive)
        .template get<UnitCollisionEvents>(archive)
        .template get<FeatureCrushEvents>(archive)
        .template get<UnitCrushEvents>(archive)
        .template get<FeatureMoveEvents>(archive);
}

}

#endif