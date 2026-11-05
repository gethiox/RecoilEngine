/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef CR_LUA_TYPES_H
#define CR_LUA_TYPES_H

#include "creg_cond.h"
#include "LuaInclude.h"

namespace creg {
	void SerializeLuaState(creg::ISerializer* s, lua_State** L);
	void SerializeLuaThread(creg::ISerializer* s, lua_State** L);
	void RegisterCFunction(const char* name, lua_CFunction f);
	void AutoRegisterCFunctions(const std::string& handle, lua_State* L);
	void UnregisterAllCFunctions();
	void CopyLuaContext(lua_State* L);

	/**
	 * Brings a Lua state that is not running any code into a canonical form
	 * without changing its behaviour, so equal states serialize to equal bytes
	 * on every client (used for game snapshots):
	 *  - values left on idle thread stacks (inaccessible to Lua code) are
	 *    dropped, then everything unreachable is collected
	 *  - sizes that depend on history and on the collector's timing (thread
	 *    stacks, string table, scratch buffer) and the collector's colors and
	 *    pacing are normalized
	 *  - string table buckets are ordered by content
	 * Includes the full garbage collections done before saving.
	 */
	void MakeCanonicalLuaState(lua_State* L, lua_State* L_GC);
}

#endif //CR_BASIC_TYPES_H
