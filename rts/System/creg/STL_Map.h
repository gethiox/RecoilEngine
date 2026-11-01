/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef CR_MAP_TYPE_IMPL_H
#define CR_MAP_TYPE_IMPL_H

#include "creg_cond.h"
#include "System/UnorderedMap.hpp"

#include <map>

#ifdef USING_CREG

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace creg
{
	// implement map insert() function with an interface common to all map types
	template<typename T>
	struct MapInserter
	{
		typedef typename T::iterator iterator;
		typedef typename T::value_type value_type;
		iterator insert(T& m, value_type& p) {
			// std::map<>::insert returns std::pair<iterator, bool>.
			return m.insert(p).first;
		}
	};
	template<typename TKey, typename TValue>
	struct MapInserter<std::multimap<TKey, TValue> >
	{
		typedef typename std::multimap<TKey, TValue>::iterator iterator;
		typedef typename std::multimap<TKey, TValue>::value_type value_type;
		iterator insert(std::multimap<TKey, TValue>& m, value_type& p) {
			// std::multimap<>::insert returns iterator.
			return m.insert(p);
		}
	};

	// map/multimap, this template assumes that the map key type is copyable
	template<typename T>
	struct MapType : public IType
	{
		typedef typename T::iterator iterator;

		MapType() : IType(sizeof(T)) { }

		~MapType() { }

		void Serialize(ISerializer *s, void *instance)
		{
			T& ct = *(T*)instance;
			if (s->IsWriting()) {
				int size = ct.size();
				s->SerializeInt(&size, sizeof(int));
				for (iterator i = ct.begin(); i != ct.end(); ++i)  {
					DeduceType<typename T::key_type>::Get()->Serialize(s, (void*) &i->first);
					DeduceType<typename T::mapped_type>::Get()->Serialize(s, &i->second);
				}
			} else {
				ct.clear();
				int size;
				s->SerializeInt(&size, sizeof(int));
				for (int a = 0; a < size; a++) {
					typename T::value_type pt;
					// only allow copying of the key type
					DeduceType<typename T::key_type>::Get()->Serialize(s, (void*) &pt.first);
					iterator i = MapInserter<T>().insert(ct, pt);
					DeduceType<typename T::mapped_type>::Get()->Serialize(s, &i->second);
				}
			}
		}
		std::string GetName() const { return "map<" + DeduceType<typename T::key_type>::Get()->GetName() + ", " + DeduceType<typename T::mapped_type>::Get()->GetName() + ">"; }
	};

	/**
	 * Hash map that stores its exact internal layout, so a loaded map
	 * iterates in the very same order as the saved one (an open-addressing
	 * map's order depends on its insertion/erase history, re-inserting the
	 * elements does not reproduce it). Not usable for keys hashed by address.
	 */
	template<typename T>
	struct ExactLayoutHashMapType : public IType
	{
		ExactLayoutHashMapType() : IType(sizeof(T)) { }

		void Serialize(ISerializer* s, void* instance)
		{
			T& ct = *(T*)instance;

			uint64_t numBuckets = ct.layout_bucket_count();
			int32_t maxProbeLength = ct.layout_max_probe_length();
			s->SerializeInt(&numBuckets, sizeof(numBuckets));
			s->SerializeInt(&maxProbeLength, sizeof(maxProbeLength));

			if (numBuckets > (uint64_t(1) << 32) || (numBuckets & (numBuckets - 1)) != 0)
				throw std::runtime_error("[creg::ExactLayoutHashMapType] invalid bucket count");

			std::vector<uint8_t> states(numBuckets);

			if (s->IsWriting()) {
				for (size_t b = 0; b < numBuckets; b++)
					states[b] = ct.layout_state(b);
			}

			s->Serialize(states.data(), static_cast<int>(numBuckets));

			if (s->IsWriting()) {
				// iteration order is bucket order
				for (auto& p: ct) {
					DeduceType<typename T::key_type>::Get()->Serialize(s, (void*) &p.first);
					DeduceType<typename T::mapped_type>::Get()->Serialize(s, &p.second);
				}
				return;
			}

			ct.layout_restore(numBuckets, maxProbeLength, states);

			for (size_t b = 0; b < numBuckets; b++) {
				if (states[b] != T::LAYOUT_STATE_FILLED)
					continue;

				typename T::key_type key{};
				DeduceType<typename T::key_type>::Get()->Serialize(s, &key);
				auto& p = ct.layout_fill(b, std::move(key));
				DeduceType<typename T::mapped_type>::Get()->Serialize(s, &p.second);
			}
		}

		std::string GetName() const { return "hashmap<" + DeduceType<typename T::key_type>::Get()->GetName() + ", " + DeduceType<typename T::mapped_type>::Get()->GetName() + ">"; }
	};

	// Map type
	template<typename TKey, typename TValue>
	struct DeduceType<std::map<TKey, TValue> > {
		static std::unique_ptr<IType> Get() {
			return std::unique_ptr<IType>(new MapType<std::map<TKey, TValue> >());
		}
	};
	// Multimap
	template<typename TKey, typename TValue>
	struct DeduceType<std::multimap<TKey, TValue> > {
		static std::unique_ptr<IType> Get() {
			return std::unique_ptr<IType>(new MapType<std::multimap<TKey, TValue> >());
		}
	};
	// Hash map
	template<typename TKey, typename TValue>
	struct DeduceType<spring::unordered_map<TKey, TValue> > {
		using MapT = spring::unordered_map<TKey, TValue>;

		static std::unique_ptr<IType> Get() {
			// keys hashed by address cannot keep their layout across processes
			if constexpr (!std::is_pointer_v<TKey> && requires (MapT& m) { m.layout_bucket_count(); })
				return std::unique_ptr<IType>(new ExactLayoutHashMapType<MapT>());
			else
				return std::unique_ptr<IType>(new MapType<MapT>());
		}
	};
	template<typename TKey, typename TValue>
	struct DeduceType<spring::unsynced_map<TKey, TValue> > {
		static std::unique_ptr<IType> Get() {
			return std::unique_ptr<IType>(new MapType<spring::unsynced_map<TKey, TValue> >());
		}
	};

	template<typename T>
	struct PairType : public IType
	{
		std::unique_ptr<IType> firstType, secondType;

		PairType()
				: IType(sizeof(T))
				, firstType(DeduceType<typename T::first_type>::Get())
				, secondType(DeduceType<typename T::second_type>::Get()) {}

		~PairType() { }

		void Serialize(ISerializer *s, void *instance)
		{
			T& p = *(T*)instance;
			firstType->Serialize(s, &p.first);
			secondType->Serialize(s, &p.second);
		}
		std::string GetName() const { return "pair<" + firstType->GetName() + "," + secondType->GetName() + ">"; }

	};

	// std::pair
	template<typename TFirst, typename TSecond>
	struct DeduceType<std::pair<TFirst, TSecond> >
	{
		static std::unique_ptr<IType> Get() {
			return std::unique_ptr<IType>(new PairType<std::pair<TFirst, TSecond> >());
		}
	};
}

#endif // USING_CREG

#endif // CR_MAP_TYPE_IMPL_H
