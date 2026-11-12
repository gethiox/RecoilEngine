/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef CREG_RAW_SERIALIZE_H
#define CREG_RAW_SERIALIZE_H

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "ISerializer.h"
#include "System/Exceptions.h"

namespace creg {
	/**
	 * @brief Thin helper for hand-written (de)serialization through a creg::ISerializer.
	 *
	 * Used by subsystems whose state is not described by creg metadata (pools,
	 * index-linked structures, ECS registries). Values are stored in native
	 * representation, which is fine since saved games and snapshots are only
	 * ever exchanged between identical engine builds.
	 *
	 * Every container is prefixed with its element count. Readers validate
	 * counts against an optional upper bound and throw content_error on
	 * mismatches instead of silently producing an inconsistent state.
	 */
	class RawArchive {
	public:
		explicit RawArchive(ISerializer* s): ser(s) {}

		bool IsWriting() const { return ser->IsWriting(); }
		bool IsReading() const { return !ser->IsWriting(); }

		template<typename T> void Value(T& v) {
			static_assert(std::is_trivially_copyable_v<T>, "use a dedicated overload for non-trivial types");
			ser->Serialize(&v, sizeof(T));
		}

		template<typename T> void Array(T* data, size_t count) {
			static_assert(std::is_trivially_copyable_v<T>, "use a dedicated overload for non-trivial types");
			ser->Serialize(data, static_cast<int>(count * sizeof(T)));
		}

		/// element count; when reading, throws if it exceeds maxCount
		uint32_t Count(size_t count, size_t maxCount = UINT32_MAX) {
			uint32_t n = static_cast<uint32_t>(count);
			Value(n);

			if (n > maxCount)
				throw content_error("[creg::RawArchive] element count out of range");

			return n;
		}

		/// a container of trivially copyable elements that supports size(), resize() and data()
		template<typename C> void PodContainer(C& c, size_t maxCount = UINT32_MAX) {
			const uint32_t n = Count(c.size(), maxCount);

			if (IsReading())
				c.resize(n);

			Array(c.data(), n);
		}

		/// like PodContainer, but the (already initialized) size must not change
		template<typename C> void FixedPodContainer(C& c) {
			if (Count(c.size()) != c.size())
				throw content_error("[creg::RawArchive] container size mismatch");

			Array(c.data(), c.size());
		}

		/// a container of arbitrary elements, each handled by f(archive, element)
		template<typename C, typename F> void Container(C& c, F&& f, size_t maxCount = UINT32_MAX) {
			const uint32_t n = Count(c.size(), maxCount);

			if (IsReading()) {
				c.clear();
				c.resize(n);
			}

			for (auto& e: c)
				f(*this, e);
		}

		void String(std::string& str) {
			const uint32_t n = Count(str.size(), 1 << 24);

			if (IsReading())
				str.resize(n);

			ser->Serialize(str.data(), static_cast<int>(n));
		}

		/// writes or verifies a marker; protects against reading misaligned data
		void Marker(uint32_t marker) {
			uint32_t v = marker;
			Value(v);

			if (v != marker)
				throw content_error("[creg::RawArchive] stream corrupted (marker mismatch)");
		}

		ISerializer* GetSerializer() { return ser; }

	private:
		ISerializer* ser;
	};
}

#endif // CREG_RAW_SERIALIZE_H
