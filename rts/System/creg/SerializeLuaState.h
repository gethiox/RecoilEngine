/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef CR_LUA_TYPES_H
#define CR_LUA_TYPES_H

#include <vector>
#include <utility>
#include <cstddef>
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
	ptrdiff_t MakeCanonicalLuaState(lua_State* L, lua_State* L_GC);

	/**
	 * The canonical form of a table whose layout depends on the collector's
	 * timing (weak tables) or on addresses (light userdata keys): the sizes
	 * a rehash of its contents gives and those contents in a canonical
	 * order. Snapshots store such tables in this form; the loading client
	 * builds the table from it.
	 */
	struct CanonicalTable {
		int nasize = 0;
		int nhsize = 0;
		std::vector<std::pair<TValue, TValue>> entries;
	};

	bool TableHasHistoryDependentLayout(const Table* t);
	void ComputeCanonicalTable(const Table* t, CanonicalTable& out);
	/// the allocation counters as they will be once the stored (canonical) tables are loaded
	void AdjustLuaAccounting(lua_State* L, ptrdiff_t delta);
	/// forgets canonical tables of a load that did not finish (called before every load)
	void ClearPendingLuaTables();
}

#endif //CR_BASIC_TYPES_H
