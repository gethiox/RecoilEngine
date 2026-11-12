/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/**
 * Test support: canonical text dump of a (synced) Lua heap.
 *
 * Tables are printed with their exact internal layout (array part, node
 * slots in storage order, including empty and dead slots) because the
 * layout decides iteration order and future rehashing. Objects are
 * identified by their creation sequence number (hashid), which is part of
 * the saved state, so two dumps of equivalent states are textually equal.
 */

#include "SimStateLuaDump.h"

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "lib/lua/include/LuaInclude.h"
#include "lib/lua/src/lobject.h"
#include "lib/lua/src/lstate.h"
#include "lib/lua/src/ltable.h"
#include "System/StringUtil.h"

namespace {
	class LuaHeapDumper {
	public:
		explicit LuaHeapDumper(FILE* f): out(f) {}

		void DumpRoots(lua_State* L) {
			global_State* g = G(L);

			fprintf(out, "nexthashid %u\n", static_cast<unsigned>(g->nexthashid));
			Enqueue(&g->l_registry, "registry");
			Enqueue(gt(L), "_G");

			for (int i = 0; i < NUM_TAGS; i++) {
				if (g->mt[i] != nullptr)
					EnqueueObj(obj2gco(g->mt[i]), "basic_mt[" + IntToString(i) + "]");
			}

			while (!queue.empty()) {
				std::vector<std::pair<GCObject*, std::string>> batch;
				batch.swap(queue);

				for (const auto& [o, path]: batch)
					DumpObject(o, path);
			}
		}

	private:
		std::string Repr(const TValue* v) {
			char buf[128];

			switch (ttype(v)) {
				case LUA_TNIL: return "nil";
				case LUA_TBOOLEAN: return bvalue(v) ? "true" : "false";
				case LUA_TNUMBER: {
					// exact bit pattern, numbers are floats in Spring's Lua
					float f = nvalue(v);
					uint32_t bits;
					memcpy(&bits, &f, sizeof(bits));
					snprintf(buf, sizeof(buf), "%.9g<%08x>", f, bits);
					return buf;
				}
				case LUA_TSTRING: {
					std::string s(svalue(v), tsvalue(v)->len);
					if (s.size() > 64)
						s = s.substr(0, 64) + "...";
					for (char& c: s) {
						if (c == '\n' || c == '\r' || c == '\0')
							c = '?';
					}
					return "\"" + s + "\"";
				}
				case LUA_TLIGHTUSERDATA: return "lightuserdata";
				case LUA_TDEADKEY: return "DEADKEY";
				default: {
					const GCObject* o = gcvalue(v);
					snprintf(buf, sizeof(buf), "%s#%u", lua_typename(nullptr, ttype(v)), static_cast<unsigned>(o->gch.hashid));
					return buf;
				}
			}
		}

		void Enqueue(const TValue* v, const std::string& path) {
			if (iscollectable(v) && ttype(v) != LUA_TSTRING)
				EnqueueObj(gcvalue(v), path);
		}

		void EnqueueObj(GCObject* o, const std::string& path) {
			if (o == nullptr || !visited.insert(o).second)
				return;

			queue.emplace_back(o, path);
		}

		void DumpObject(GCObject* o, const std::string& path) {
			switch (o->gch.tt) {
				case LUA_TTABLE: DumpTable(gco2h(o), path); break;
				case LUA_TFUNCTION: DumpFunction(gco2cl(o), path); break;
				case LUA_TUSERDATA: {
					Udata* u = rawgco2u(o);
					fprintf(out, "userdata#%u %s len=%u\n", static_cast<unsigned>(o->gch.hashid), path.c_str(), static_cast<unsigned>(u->uv.len));

					if (u->uv.metatable != nullptr)
						EnqueueObj(obj2gco(u->uv.metatable), path + ".<mt>");
					if (u->uv.env != nullptr)
						EnqueueObj(obj2gco(u->uv.env), path + ".<env>");
				} break;
				case LUA_TTHREAD: {
					lua_State* th = gco2th(o);
					fprintf(out, "thread#%u %s status=%d stack=%d\n", static_cast<unsigned>(o->gch.hashid), path.c_str(), th->status, static_cast<int>(th->top - th->stack));

					for (StkId p = th->stack; p < th->top; p++)
						Enqueue(p, path + ".<stack>");

					Enqueue(gt(th), path + ".<globals>");
				} break;
				default: {
					fprintf(out, "object#%u %s type=%d\n", static_cast<unsigned>(o->gch.hashid), path.c_str(), o->gch.tt);
				} break;
			}
		}

		void DumpTable(Table* t, const std::string& path) {
			fprintf(out, "table#%u %s sizearray=%d lsizenode=%d flags=%u lastfree=%d\n",
				static_cast<unsigned>(t->hashid), path.c_str(), t->sizearray, t->lsizenode, t->flags,
				(t->lastfree != nullptr && t->node != nullptr) ? static_cast<int>(t->lastfree - t->node) : -1);

			if (t->metatable != nullptr) {
				fprintf(out, "  mt %s\n", Repr2(obj2gco(t->metatable)).c_str());
				EnqueueObj(obj2gco(t->metatable), path + ".<mt>");
			}

			for (int i = 0; i < t->sizearray; i++) {
				const TValue* v = &t->array[i];
				if (ttisnil(v))
					continue;

				fprintf(out, "  a[%d] = %s\n", i + 1, Repr(v).c_str());
				Enqueue(v, path + "[" + IntToString(i + 1) + "]");
			}

			if (t->node == nullptr || t->lsizenode == 0 && ttisnil(gkey(t->node)) && ttisnil(gval(t->node)))
				return;

			const int sizenode = 1 << t->lsizenode;

			for (int i = 0; i < sizenode; i++) {
				const Node* n = &t->node[i];
				const TValue* k = key2tval(n);
				const TValue* v = gval(n);

				if (ttisnil(k) && ttisnil(v))
					continue;

				const int next = (gnext(n) != nullptr) ? static_cast<int>(gnext(n) - t->node) : -1;
				const std::string ks = Repr(k);

				fprintf(out, "  n[%d] %s = %s next=%d\n", i, ks.c_str(), Repr(v).c_str(), next);

				if (ttisnil(v))
					continue;

				Enqueue(k, path + ".<key " + ks + ">");
				Enqueue(v, path + "[" + ks + "]");
			}
		}

		std::string Repr2(GCObject* o) {
			TValue tv;
			tv.value.gc = o;
			tv.tt = o->gch.tt;
			return Repr(&tv);
		}

		void DumpFunction(Closure* cl, const std::string& path) {
			if (cl->c.isC) {
				fprintf(out, "cfunction#%u %s nup=%d\n", static_cast<unsigned>(cl->c.hashid), path.c_str(), cl->c.nupvalues);

				for (int i = 0; i < cl->c.nupvalues; i++) {
					fprintf(out, "  up[%d] = %s\n", i, Repr(&cl->c.upvalue[i]).c_str());
					Enqueue(&cl->c.upvalue[i], path + ".<up" + IntToString(i) + ">");
				}
			} else {
				const Proto* p = cl->l.p;
				fprintf(out, "function#%u %s %s:%d nup=%d\n", static_cast<unsigned>(cl->l.hashid), path.c_str(),
					(p->source != nullptr) ? getstr(p->source) : "?", p->linedefined, cl->l.nupvalues);

				for (int i = 0; i < cl->l.nupvalues; i++) {
					const TValue* v = cl->l.upvals[i]->v;
					fprintf(out, "  up[%d] = %s\n", i, Repr(v).c_str());
					Enqueue(v, path + ".<up" + IntToString(i) + ">");
				}
			}

			if (cl->c.env != nullptr)
				EnqueueObj(obj2gco(cl->c.env), path + ".<env>");
		}

	private:
		FILE* out;
		std::unordered_set<const GCObject*> visited;
		std::vector<std::pair<GCObject*, std::string>> queue;
	};
}

bool SimStateLuaDump::DumpHeap(lua_State* L, const std::string& path)
{
	if (L == nullptr)
		return false;

	FILE* f = fopen(path.c_str(), "w");

	if (f == nullptr)
		return false;

	LuaHeapDumper(f).DumpRoots(L);
	fclose(f);
	return true;
}
