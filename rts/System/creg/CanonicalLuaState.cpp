/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SerializeLuaState.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "lib/lua/src/lgc.h"
#include "lib/lua/src/ldo.h"
#include "lib/lua/src/lobject.h"
#include "lib/lua/src/lstate.h"
#include "lib/lua/src/lstring.h"
#include "lib/lua/src/lzio.h"

namespace {

/// the bucket order only affects lookup order, which depends on string creation history
void SortStringTable(global_State* g)
{
	stringtable& strt = g->strt;
	std::vector<GCObject*> bucket;

	const auto Less = [](GCObject* a, GCObject* b) {
		const TString* sa = rawgco2ts(a);
		const TString* sb = rawgco2ts(b);

		if (sa->tsv.len != sb->tsv.len)
			return (sa->tsv.len < sb->tsv.len);

		return (memcmp(getstr(sa), getstr(sb), sa->tsv.len) < 0);
	};

	for (int i = 0; i < strt.size; i++) {
		bucket.clear();

		for (GCObject* o = strt.hash[i]; o != nullptr; o = o->gch.next)
			bucket.push_back(o);

		if (bucket.size() < 2)
			continue;

		std::sort(bucket.begin(), bucket.end(), Less);

		for (size_t j = 0; j < bucket.size(); j++)
			bucket[j]->gch.next = (j + 1 < bucket.size()) ? bucket[j + 1] : nullptr;

		strt.hash[i] = bucket.front();
	}
}

/**
 * Stack, CallInfo array and string table grow on demand and are shrunk by the
 * garbage collector, so their sizes depend on the history (and the collector's
 * timing). Resize them to a function of their current use that the collector
 * does not shrink further (see checkstacksizes and checkSizes in lgc.cpp).
 */
void CanonicalSizes(lua_State* L)
{
	global_State* g = G(L);

	for (GCObject* o = g->rootgc; o != nullptr; o = o->gch.next) {
		if (o->gch.tt != LUA_TTHREAD)
			continue;

		lua_State* th = gco2th(o);

		// overflow handling, never touched by the collector either
		if (th->size_ci > LUAI_MAXCALLS)
			continue;

		// same measure of use as traversestack
		StkId lim = th->top;

		for (CallInfo* ci = th->base_ci; ci <= th->ci; ci++) {
			if (lim < ci->top)
				lim = ci->top;
		}

		const int ciUsed = static_cast<int>(th->ci - th->base_ci);
		const int stackUsed = static_cast<int>(lim - th->stack);
		const int ciSize = std::max(2 * BASIC_CI_SIZE, 2 * (ciUsed + 1));
		// luaD_reallocstack adds 1 + EXTRA_STACK
		const int stackSize = std::max(2 * BASIC_STACK_SIZE + EXTRA_STACK - 1, 2 * stackUsed);

		if (th->size_ci != ciSize)
			luaD_reallocCI(th, ciSize);
		if (th->stacksize != (stackSize + 1 + EXTRA_STACK))
			luaD_reallocstack(th, stackSize);
	}

	int strtSize = MINSTRTABSIZE;

	while (strtSize < static_cast<int>(g->strt.nuse))
		strtSize *= 2;

	if (g->strt.size != strtSize)
		luaS_resize(L, strtSize);

	// scratch buffer (lexer, concatenation), not serialized and empty after loading
	luaZ_freebuffer(L, &g->buff);

	// collector pacing only
	g->gcdept = 0;
}

/**
 * After a full collection every live object has the current white, which
 * alternates with each cycle; use WHITE0 for it.
 */
void CanonicalWhite(global_State* g)
{
	if ((g->currentwhite & WHITEBITS) != bitmask(WHITE1BIT))
		return;

	const auto Swap = [](lu_byte& marked) {
		const lu_byte whites = marked & WHITEBITS;

		if (whites == bitmask(WHITE0BIT) || whites == bitmask(WHITE1BIT))
			marked = (marked & ~WHITEBITS) | (whites ^ WHITEBITS);
	};

	Swap(g->currentwhite);

	for (GCObject* o = g->rootgc; o != nullptr; o = o->gch.next)
		Swap(o->gch.marked);

	for (int i = 0; i < g->strt.size; i++) {
		for (GCObject* o = g->strt.hash[i]; o != nullptr; o = o->gch.next)
			Swap(o->gch.marked);
	}

	// open upvalues are only linked here (and in their thread)
	for (UpVal* uv = g->uvhead.u.l.next; uv != &g->uvhead; uv = uv->u.l.next)
		Swap(uv->marked);
}

}


namespace creg {

void MakeCanonicalLuaState(lua_State* L, lua_State* L_GC)
{
	// a thread outside of any call has nothing on its stack that Lua code
	// could ever access again (C++ code may have leaked values there)
	for (lua_State* th: {L, L_GC}) {
		if (th != nullptr && th->ci == th->base_ci)
			lua_settop(th, 0);
	}

	lua_gc(L_GC, LUA_GCCOLLECT, 0);

	// sizes depend on the amount of live data; the second collection makes
	// the collector's estimates a function of the (now canonical) heap
	CanonicalSizes(L_GC);
	lua_gc(L_GC, LUA_GCCOLLECT, 0);

	CanonicalWhite(G(L_GC));
	SortStringTable(G(L_GC));
}

}
