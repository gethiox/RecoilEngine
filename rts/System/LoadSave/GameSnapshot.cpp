/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "GameSnapshot.h"

#include <cstring>
#include <zlib.h>

#include "CregLoadSaveHandler.h"
#include "System/Sync/SHA512.hpp"
#include "System/creg/Serializer.h"

namespace {
	int activeCount = 0;

	/// reads a null-terminated string at pos, advancing it; false if unterminated
	bool ReadString(const std::string& raw, size_t& pos, std::string& out)
	{
		const size_t end = raw.find('\0', pos);

		if (end == std::string::npos || (end - pos) > (1 << 20))
			return false;

		out.assign(raw, pos, end - pos);
		pos = end + 1;
		return true;
	}
}

GameSnapshot::ScopedActivation::ScopedActivation() { creg::SetSkipUnsyncedMembers((++activeCount) > 0); }
GameSnapshot::ScopedActivation::~ScopedActivation() { creg::SetSkipUnsyncedMembers((--activeCount) > 0); }

bool GameSnapshot::IsActive() { return (activeCount > 0); }


bool GameSnapshot::Write(std::string& raw)
{
	return CCregLoadSaveHandler::WriteSnapshot(raw);
}

size_t GameSnapshot::ReadHeader(const std::string& raw, Header& header)
{
	size_t pos = 0;
	std::string magic;

	if (!ReadString(raw, pos, magic) || magic != MAGIC)
		return 0;
	if (!ReadString(raw, pos, header.syncVersion))
		return 0;
	if (!ReadString(raw, pos, header.setupText))
		return 0;
	if (!ReadString(raw, pos, header.modName))
		return 0;
	if (!ReadString(raw, pos, header.mapName))
		return 0;
	if ((pos + sizeof(header.frameNum)) > raw.size())
		return 0;

	memcpy(&header.frameNum, raw.data() + pos, sizeof(header.frameNum));
	return (pos + sizeof(header.frameNum));
}


bool GameSnapshot::Compress(const std::string& raw, std::string& compressed, int level)
{
	// format: uint64 raw size, zlib stream
	const uint64_t rawSize = raw.size();
	uLongf bound = compressBound(raw.size());

	compressed.resize(sizeof(rawSize) + bound);
	memcpy(compressed.data(), &rawSize, sizeof(rawSize));

	const int ret = compress2(
		reinterpret_cast<Bytef*>(compressed.data() + sizeof(rawSize)), &bound,
		reinterpret_cast<const Bytef*>(raw.data()), raw.size(),
		level
	);

	if (ret != Z_OK) {
		compressed.clear();
		return false;
	}

	compressed.resize(sizeof(rawSize) + bound);
	return true;
}

bool GameSnapshot::Decompress(const std::string& compressed, std::string& raw, size_t maxRawSize)
{
	uint64_t rawSize = 0;

	if (compressed.size() < sizeof(rawSize))
		return false;

	memcpy(&rawSize, compressed.data(), sizeof(rawSize));

	if (rawSize > maxRawSize)
		return false;

	raw.resize(rawSize);
	uLongf destLen = rawSize;

	const int ret = uncompress(
		reinterpret_cast<Bytef*>(raw.data()), &destLen,
		reinterpret_cast<const Bytef*>(compressed.data() + sizeof(rawSize)), compressed.size() - sizeof(rawSize)
	);

	if (ret != Z_OK || destLen != rawSize) {
		raw.clear();
		return false;
	}

	return true;
}


GameSnapshot::Digest GameSnapshot::Hash(const std::string& raw)
{
	uint8_t full[sha512::SHA_LEN];
	sha512::calc_digest(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), full);

	Digest d;
	memcpy(d.data(), full, d.size());
	return d;
}

std::string GameSnapshot::DigestToHex(const Digest& d)
{
	static constexpr char HEX[] = "0123456789abcdef";
	std::string hex;
	hex.reserve(d.size() * 2);

	for (const uint8_t b: d) {
		hex += HEX[b >> 4];
		hex += HEX[b & 15];
	}

	return hex;
}
