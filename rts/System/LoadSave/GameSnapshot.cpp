/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "GameSnapshot.h"

#include <cstring>

#include <Alloc.h>
#include <Lzma2Dec.h>
#include <Lzma2Enc.h>

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


// LZMA2 instead of zlib: most of a snapshot is path finder and height map data
// that repeats at distances far beyond zlib's 32 kB window (a typical
// snapshot is about 4 times smaller, at 2-3 times the compression time)
static constexpr uint32_t LZMA2_DICT_SIZE = 16 << 20;
static constexpr uint64_t LZMA2_BLOCK_SIZE = 32 << 20;
static constexpr int LZMA2_THREADS = 2; // ~450 MB while compressing

bool GameSnapshot::Compress(const std::string& raw, std::string& compressed, int level)
{
	// format: uint64 raw size, uint8 LZMA2 properties, LZMA2 stream
	const uint64_t rawSize = raw.size();

	CLzma2EncProps props;
	Lzma2EncProps_Init(&props);
	props.lzmaProps.level = level;
	props.lzmaProps.dictSize = LZMA2_DICT_SIZE;
	props.blockSize = LZMA2_BLOCK_SIZE;
	props.numBlockThreads_Max = LZMA2_THREADS;
	props.numTotalThreads = LZMA2_THREADS;
	Lzma2EncProps_Normalize(&props);

	CLzma2EncHandle enc = Lzma2Enc_Create(&g_Alloc, &g_BigAlloc);

	if (enc == nullptr)
		return false;

	size_t outSize = raw.size() + raw.size() / 64 + (1 << 16);
	compressed.resize(sizeof(rawSize) + 1 + outSize);
	memcpy(compressed.data(), &rawSize, sizeof(rawSize));

	SRes res = Lzma2Enc_SetProps(enc, &props);

	if (res == SZ_OK) {
		Lzma2Enc_SetDataSize(enc, rawSize);
		compressed[sizeof(rawSize)] = static_cast<char>(Lzma2Enc_WriteProperties(enc));

		res = Lzma2Enc_Encode2(enc, nullptr,
			reinterpret_cast<Byte*>(compressed.data() + sizeof(rawSize) + 1), &outSize,
			nullptr, reinterpret_cast<const Byte*>(raw.data()), raw.size(),
			nullptr
		);
	}

	Lzma2Enc_Destroy(enc);

	if (res != SZ_OK) {
		compressed.clear();
		return false;
	}

	compressed.resize(sizeof(rawSize) + 1 + outSize);
	return true;
}

bool GameSnapshot::Decompress(const std::string& compressed, std::string& raw, size_t maxRawSize)
{
	uint64_t rawSize = 0;

	if (compressed.size() < (sizeof(rawSize) + 1))
		return false;

	memcpy(&rawSize, compressed.data(), sizeof(rawSize));

	// the size is the sender's claim: bound the allocation by it and by what
	// LZMA2 could possibly have produced from this much input
	if (rawSize > maxRawSize || rawSize > uint64_t(compressed.size()) * 128)
		return false;

	raw.resize(rawSize);

	SizeT destLen = rawSize;
	SizeT srcLen = compressed.size() - sizeof(rawSize) - 1;
	ELzmaStatus status;

	const SRes res = Lzma2Decode(
		reinterpret_cast<Byte*>(raw.data()), &destLen,
		reinterpret_cast<const Byte*>(compressed.data() + sizeof(rawSize) + 1), &srcLen,
		static_cast<Byte>(compressed[sizeof(rawSize)]), LZMA_FINISH_END, &status, &g_Alloc
	);

	if (res != SZ_OK || destLen != rawSize || srcLen != (compressed.size() - sizeof(rawSize) - 1)) {
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
