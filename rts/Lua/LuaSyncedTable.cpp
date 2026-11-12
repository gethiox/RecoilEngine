/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaSyncedTable.h"

#include "LuaInclude.h"

#include "LuaHandleSynced.h"
#include "LuaHashString.h"
#include "LuaUtils.h"


static int SyncTableIndex(lua_State* L);
static int SyncTableNewIndex(lua_State* L);
static int SyncTableMetatable(lua_State* L);

/******************************************************************************/

/*
 * Pushes the key on top of dstL (unsynced) onto srcL (synced). Primitive keys
 * are pushed directly: LuaUtils::CopyData would cache them via references in
 * the synced registry, i.e. reading SYNCED.x from unsynced code would modify
 * synced state (the registry's free-list) at client specific times.
 */
static void PushKeyToSyncedState(lua_State* srcL, lua_State* dstL)
{
	switch (lua_type(dstL, -1)) {
		case LUA_TSTRING: {
			size_t len = 0;
			const char* str = lua_tolstring(dstL, -1, &len);
			lua_pushlstring(srcL, str, len);
		} break;
		case LUA_TNUMBER: {
			lua_pushnumber(srcL, lua_tonumber(dstL, -1));
		} break;
		case LUA_TBOOLEAN: {
			lua_pushboolean(srcL, lua_toboolean(dstL, -1));
		} break;
		default: {
			// a table, function or userdata of this state is never a key of
			// the other one; copying it over would create objects (and take
			// registry references) in the synced state
			lua_pushnil(srcL);
		} break;
	}
}

static int SyncTableIndex(lua_State* dstL)
{
	if (lua_isnoneornil(dstL, -1))
		return 0;

	auto slh = CSplitLuaHandle::GetSyncedHandle(dstL);
	if (!slh->IsValid())
		return 0;
	auto srcL = slh->GetLuaState();

	const int srcTop = lua_gettop(srcL);
	const int dstTop = lua_gettop(dstL);

	// copy the index & get value
	lua_pushvalue(srcL, LUA_GLOBALSINDEX);
	PushKeyToSyncedState(srcL, dstL);
	lua_rawget(srcL, -2);

	// copy to destination
	const int valueCopied = LuaUtils::CopyData(dstL, srcL, 1);
	if (lua_istable(dstL, -1)) {
		// disallow writing in SYNCED[...]
		lua_createtable(dstL, 0, 2); {
			LuaPushNamedCFunc(dstL, "__newindex",  SyncTableNewIndex);
			LuaPushNamedCFunc(dstL, "__metatable", SyncTableMetatable);
		}
		lua_setmetatable(dstL, -2);
	}

	assert(valueCopied == 1);
	assert(dstTop + 1 == lua_gettop(dstL));

	lua_settop(srcL, srcTop);
	return valueCopied;
}


static int SyncTableNewIndex(lua_State* L)
{
	luaL_error(L, "Attempt to write to SYNCED table");
	return 0;
}


static int SyncTableMetatable(lua_State* L)
{
	luaL_error(L, "Attempt to access SYNCED metatable");
	return 0;
}


/******************************************************************************/
/******************************************************************************/

/***
 * Proxy table for reading synced global state in unsynced code.
 * 
 * **Generally not recommended.** Instead, listen to the same events as synced
 * and build the table in parallel
 * 
 * Unsynced code can read from the synced global table (`_G`) using the `SYNCED`
 * proxy table. e.g. `_G.foo` can be access from unsynced via `SYNCED.foo`.
 * 
 * This table makes *a copy* of the object on the other side, and only copies
 * numbers, strings, bools and tables (recursively but with the type
 * restriction), in particular this does not allow access to functions.
 * 
 * Note that this makes a copy on each access, so is very slow and will not
 * reflect changes. Cache it, but remember to refresh.
 * 
 * 
 * @global SYNCED table<string, any>
 */
bool LuaSyncedTable::PushEntries(lua_State* L)
{
	LuaPushString(L, "SYNCED");
	lua_newtable(L); { // the proxy table

		lua_createtable(L, 0, 3); { // the metatable
			LuaPushNamedCFunc(L, "__index",     SyncTableIndex);
			LuaPushNamedCFunc(L, "__newindex",  SyncTableNewIndex);
			LuaPushNamedCFunc(L, "__metatable", SyncTableMetatable);
		}

		lua_setmetatable(L, -2);
	}
	lua_rawset(L, -3);

	return true;
}


/******************************************************************************/
/******************************************************************************/
