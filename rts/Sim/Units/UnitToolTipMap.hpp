/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef UNIT_TOOLTIP_MAP_H
#define UNIT_TOOLTIP_MAP_H

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "System/UnorderedMap.hpp"
#include "System/creg/RawSerialize.h"

struct UnitToolTipMap {
public:
	void Clear() {
		tooltips.clear();
		tooltips.reserve(256);
	}

	void Set(int id, std::string&& tip) { tooltips[id] = std::move(tip); }
	const std::string& Get(int id) { return tooltips[id]; }

	/// synced Lua can set and read these; stored in id order (the map's order is not reproducible)
	void Serialize(creg::ISerializer* s) {
		creg::RawArchive ar(s);
		std::vector<std::pair<int, std::string>> items;

		if (ar.IsWriting()) {
			items.assign(tooltips.begin(), tooltips.end());
			std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return (a.first < b.first); });
		}

		ar.Container(items, [](creg::RawArchive& a, std::pair<int, std::string>& e) { a.Value(e.first); a.String(e.second); });

		if (ar.IsWriting())
			return;

		Clear();

		for (auto& e: items)
			tooltips[e.first] = std::move(e.second);
	}

private:
	spring::unordered_map<int, std::string> tooltips;
};

extern UnitToolTipMap unitToolTipMap;

#endif

