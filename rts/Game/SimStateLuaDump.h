/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef SIM_STATE_LUA_DUMP_H
#define SIM_STATE_LUA_DUMP_H

#include <string>

struct lua_State;

namespace SimStateLuaDump {
	/// test support: canonical text dump of the Lua heap reachable from L
	bool DumpHeap(lua_State* L, const std::string& path);
}

#endif
