/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef CR_SET_TYPE_IMPL_H
#define CR_SET_TYPE_IMPL_H

#include "creg_cond.h"
#include "System/UnorderedSet.hpp"

#include <cstdint>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

#ifdef USING_CREG

namespace creg
{
	// set/multiset - this template assumes the type is copyable
	template<typename T>
	class SetType : public IType
	{
	public:
		typedef typename T::iterator iterator;

		SetType() : IType(sizeof(T)) { }
		~SetType() { }

		void Serialize(ISerializer* s, void* instance)
		{
			T& ct = *(T*)instance;
			if (s->IsWriting()) {
				int size = ct.size();
				s->SerializeInt(&size, sizeof(int));
				for (iterator i = ct.begin(); i != ct.end(); ++i) {
					DeduceType<typename T::value_type>::Get()->Serialize(s,(void*) &*i);
				}
			} else {
				ct.clear();
				int size;
				s->SerializeInt(&size, sizeof(int));
				for (int i = 0; i < size; i++) {
					typename T::value_type v;
					DeduceType<typename T::value_type>::Get()->Serialize(s, &v);
					ct.insert(v);
				}
			}
		}
		std::string GetName() const { return "set<" + DeduceType<typename T::value_type>::Get()->GetName() + ">"; }
	};


	/// hash set keeping its exact internal layout, see ExactLayoutHashMapType
	template<typename T>
	class ExactLayoutHashSetType : public IType
	{
	public:
		ExactLayoutHashSetType() : IType(sizeof(T)) { }

		void Serialize(ISerializer* s, void* instance)
		{
			T& ct = *(T*)instance;

			uint64_t numBuckets = ct.layout_bucket_count();
			int32_t maxProbeLength = ct.layout_max_probe_length();
			s->SerializeInt(&numBuckets, sizeof(numBuckets));
			s->SerializeInt(&maxProbeLength, sizeof(maxProbeLength));

			if (numBuckets > (uint64_t(1) << 32) || (numBuckets & (numBuckets - 1)) != 0)
				throw std::runtime_error("[creg::ExactLayoutHashSetType] invalid bucket count");

			std::vector<uint8_t> states(numBuckets);

			if (s->IsWriting()) {
				for (size_t b = 0; b < numBuckets; b++)
					states[b] = ct.layout_state(b);
			}

			s->Serialize(states.data(), static_cast<int>(numBuckets));

			if (s->IsWriting()) {
				for (const auto& v: ct)
					DeduceType<typename T::value_type>::Get()->Serialize(s, (void*) &v);
				return;
			}

			ct.layout_restore(numBuckets, maxProbeLength, states);

			for (size_t b = 0; b < numBuckets; b++) {
				if (states[b] != T::LAYOUT_STATE_FILLED)
					continue;

				typename T::value_type v{};
				DeduceType<typename T::value_type>::Get()->Serialize(s, &v);
				ct.layout_fill(b, std::move(v));
			}
		}

		std::string GetName() const { return "hashset<" + DeduceType<typename T::value_type>::Get()->GetName() + ">"; }
	};

	// Set type
	template<typename T, typename C>
	struct DeduceType<std::set<T, C> > {
		static std::unique_ptr<IType> Get() {
			return std::unique_ptr<IType>(new SetType<std::set<T, C> >());
		}
	};
	// Multiset
	template<typename T>
	struct DeduceType<std::multiset<T> > {
		static std::unique_ptr<IType> Get() {
			return std::unique_ptr<IType>(new SetType<std::multiset<T> >());
		}
	};
	// Hash set
	template<typename T>
	struct DeduceType<spring::unordered_set<T> > {
		using SetT = spring::unordered_set<T>;

		static std::unique_ptr<IType> Get() {
			// values hashed by address cannot keep their layout across processes
			if constexpr (!std::is_pointer_v<T> && requires (SetT& c) { c.layout_bucket_count(); })
				return std::unique_ptr<IType>(new ExactLayoutHashSetType<SetT>());
			else
				return std::unique_ptr<IType>(new SetType<SetT>());
		}
	};
	// Unsynced Hash set
	template<typename T>
	struct DeduceType<spring::unsynced_set<T> > {
		static std::unique_ptr<IType> Get() {
			return std::unique_ptr<IType>(new SetType<spring::unsynced_set<T> >());
		}
	};
}

#endif // USING_CREG

#endif // CR_SET_TYPE_IMPL_H

