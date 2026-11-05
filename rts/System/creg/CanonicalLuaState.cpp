/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SerializeLuaState.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

#include "lib/lua/src/lgc.h"
#include "lib/lua/src/ldo.h"
#include "lib/lua/src/lobject.h"
#include "lib/lua/src/lstate.h"
#include "lib/lua/src/lstring.h"
#include "lib/lua/src/ltable.h"
#include "lib/lua/src/ltm.h"
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

/// total order on table keys that depends on content (and creation ids) only
int KeyRank(const TValue* k)
{
	switch (ttype(k)) {
		case LUA_TNUMBER: return 0;
		case LUA_TBOOLEAN: return 1;
		case LUA_TSTRING: return 2;
		case LUA_TLIGHTUSERDATA: return 3;
		default: return 4; // collectable, hashed by creation id (see lobject.h CommonHeader)
	}
}

bool KeyLess(const TValue* a, const TValue* b)
{
	const int ra = KeyRank(a);
	const int rb = KeyRank(b);

	if (ra != rb)
		return (ra < rb);

	switch (ra) {
		case 0: return (nvalue(a) < nvalue(b));
		case 1: return (bvalue(a) < bvalue(b));
		case 2: {
			const TString* sa = rawtsvalue(a);
			const TString* sb = rawtsvalue(b);

			if (sa->tsv.len != sb->tsv.len)
				return (sa->tsv.len < sb->tsv.len);

			return (memcmp(getstr(sa), getstr(sb), sa->tsv.len) < 0);
		}
		case 3: return (pvalue(a) < pvalue(b));
		default: {
			if (ttype(a) != ttype(b))
				return (ttype(a) < ttype(b));

			return (gcvalue(a)->gch.hashid < gcvalue(b)->gch.hashid);
		}
	}
}

/**
 * The node layout of a table (and so its iteration order and its bytes in a
 * saved state) is a function of its insertion and removal history. For
 * tables that synced code mutates in a synced order the history is the same
 * on every client. Two kinds of tables have a history that depends on the
 * collector's (wall-clock driven) timing instead:
 *  - weak tables: entries vanish whenever the collector happens to run;
 *  - tables keyed by collectable objects (coroutines, tables, functions):
 *    their keys hash by creation id and usually live in weak tables (BAR's
 *    unit-script thread sets), or are only ever iterated unsafely (synced
 *    "next" warns about such keys since their order never was reproducible).
 * Light userdata keys hash by address and cannot be reproduced at all.
 * Rebuilding these tables from their (identical) contents in a canonical
 * order gives identical layouts; nothing synced code can rely on changes.
 */
bool HasHistoryDependentLayout(global_State* g, const Table* t)
{
	const int size = sizenode(t);

	// empty hash part (also the shared dummy node)
	if (t->lsizenode == 0 && ttisnil(gval(gnode(t, 0))) && ttisnil(gkey(gnode(t, 0))))
		return false;

	if (gfasttm(g, t->metatable, TM_MODE) != NULL)
		return true;

	for (int i = 0; i < size; i++) {
		const Node* n = gnode(t, i);

		if (ttisnil(gval(n)))
			continue;

		const TValue* k = key2tval(n);

		if ((iscollectable(k) && ttype(k) != LUA_TSTRING) || ttype(k) == LUA_TLIGHTUSERDATA)
			return true;
	}

	return false;
}

// the same sizing as ltable.cpp's rehash (which is static), without an extra key
constexpr int CANON_MAXBITS = 26;
constexpr int CANON_MAXASIZE = (1 << CANON_MAXBITS);

int ArrayIndex(const TValue* key)
{
	if (ttisnumber(key)) {
		const lua_Number n = nvalue(key);
		int k;
		lua_number2int(k, n);

		if (cast_num(k) == n)
			return k;
	}

	return -1;
}

int CountInt(const TValue* key, int* nums)
{
	const int k = ArrayIndex(key);

	if (0 < k && k <= CANON_MAXASIZE) {
		nums[ceillog2(k)]++;
		return 1;
	}

	return 0;
}

int ComputeSizes(int* nums, int* narray)
{
	int a = 0;
	int na = 0;
	int n = 0;
	int twotoi = 1;

	for (int i = 0; twotoi / 2 < *narray; i++, twotoi *= 2) {
		if (nums[i] > 0) {
			a += nums[i];

			if (a > twotoi / 2) {
				n = twotoi;
				na = a;
			}
		}

		if (a == *narray)
			break;
	}

	*narray = n;
	return na;
}

/**
 * The sizes of a table's parts are only recomputed when an insertion finds
 * no free node, from the entries alive at that moment; for a weak table that
 * moment and that count depend on the collector's timing. Resize to what a
 * rehash of the current contents gives.
 */
void CanonicalResize(lua_State* L, Table* t)
{
	int nums[CANON_MAXBITS + 1] = {0};
	int nasize = 0;
	int totaluse = 0;

	// numusearray
	{
		int i = 1;

		for (int lg = 0, ttlg = 1; lg <= CANON_MAXBITS; lg++, ttlg *= 2) {
			int lc = 0;
			int lim = ttlg;

			if (lim > t->sizearray) {
				lim = t->sizearray;

				if (i > lim)
					break;
			}

			for (; i <= lim; i++) {
				if (!ttisnil(&t->array[i - 1]))
					lc++;
			}

			nums[lg] += lc;
			nasize += lc;
		}

		totaluse = nasize;
	}

	// numusehash
	for (int i = sizenode(t); i-- > 0; ) {
		const Node* n = gnode(t, i);

		if (ttisnil(gval(n)))
			continue;

		nasize += CountInt(key2tval(n), nums);
		totaluse++;
	}

	const int na = ComputeSizes(nums, &nasize);

	luaH_resize(L, t, nasize, totaluse - na);
}

void RebuildTable(lua_State* L, Table* t)
{
	CanonicalResize(L, t);

	// an empty hash part is the shared (read-only) dummy node now
	if (t->lsizenode == 0 && ttisnil(gkey(gnode(t, 0))) && ttisnil(gval(gnode(t, 0))))
		return;

	const int size = sizenode(t);
	std::vector<std::pair<TValue, TValue>> entries;
	entries.reserve(size);

	for (int i = 0; i < size; i++) {
		const Node* n = gnode(t, i);

		// removed entries (nil value, possibly a dead key) are dropped
		if (ttisnil(gval(n)))
			continue;

		entries.emplace_back();
		setobj(L, &entries.back().first, key2tval(n));
		setobj(L, &entries.back().second, gval(n));
	}

	std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return KeyLess(&a.first, &b.first); });

	// same as setnodevector, keeping the node array
	for (int i = 0; i < size; i++) {
		Node* n = gnode(t, i);

		gnext(n) = NULL;
		setnilvalue(gkey(n));
		setnilvalue(gval(n));
	}

	t->lastfree = gnode(t, size);

	// at most <size> entries, so no rehash: the layout now only depends on the sorted keys
	for (const auto& e: entries) {
		TValue* v = luaH_set(L, t, &e.first);
		setobj2t(L, v, &e.second);
	}

	assert(sizenode(t) == size);
}

void RebuildHistoryDependentTables(lua_State* L)
{
	global_State* g = G(L);

	for (GCObject* o = g->rootgc; o != nullptr; o = o->gch.next) {
		if (o->gch.tt != LUA_TTABLE)
			continue;

		Table* t = gco2h(o);

		if (HasHistoryDependentLayout(g, t))
			RebuildTable(L, t);
	}
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

	// after a collection: weak entries are gone, nothing is in a gray list
	RebuildHistoryDependentTables(L_GC);

	// sizes depend on the amount of live data; the second collection makes
	// the collector's estimates a function of the (now canonical) heap
	CanonicalSizes(L_GC);
	lua_gc(L_GC, LUA_GCCOLLECT, 0);

	CanonicalWhite(G(L_GC));
	SortStringTable(G(L_GC));
}

}
