#pragma once

#include <string>
#include <optional>
#include <vector>

#include "y_misc.h"

#ifdef _WIN64
#define OUTPUTFOLDER_PLATFORM "x64"
#else
#define OUTPUTFOLDER_PLATFORM "Win32"
#endif

#ifdef _DEBUG
#define OUTPUTFOLDER_TARGET "Debug"
#else
#define OUTPUTFOLDER_TARGET "Release"
#endif

#define ZSTDLIB_PATH "..\\bin\\" OUTPUTFOLDER_PLATFORM "_" OUTPUTFOLDER_TARGET "\\libzstd_static.lib"

// "Microsoft MSFZ Container\x0D\x0AALD"
constexpr uint8_t g_MsfzSignatureBytes[] = 
{
  0x4D, 0x69, 0x63, 0x72, 0x6F, 0x73, 0x6F, 0x66, 0x74, 0x20, 0x4D,
  0x53, 0x46, 0x5A, 0x20, 0x43, 0x6F, 0x6E, 0x74, 0x61, 0x69, 0x6E,
  0x65, 0x72, 0x0D, 0x0A, 0x1A, 0x41, 0x4C, 0x44, 0x00, 0x00 
};

constexpr uint8_t g_PdbSignatureBytes[] = "Microsoft C/C++ MSF 7.00\r\n\x1a\x44\x53";

enum UsageMode : uint8_t
{
	Compress = 0,
	Decompress = 1,
	Batch = 2
};

enum CompressionStrategy : uint8_t
{
	NoCompression,
	SingleFragment,
	MultiFragment
};

struct ProgramCommandLineArgs
{
	std::string m_InputFilePath;
	std::string m_OutputFilePath;
	UsageMode m_UsageMode;

	// compression args
	std::optional<CompressionStrategy> m_CompressionStrategy;
	std::optional<uint32_t> m_CompressionLevel;
	std::optional<uint32_t> m_FixedFragmentSize;
	std::optional<uint32_t> m_MaxFragmentsPerStream;

	// decompression args
	std::optional<uint32_t> m_BlockSize;
};

struct PDBSuperBlock
{
	uint8_t m_Signature[30u];
	uint8_t m_Padding[2u];
	uint32_t m_BlockSize;
	uint32_t m_FreeBlockMapIndex;
	uint32_t m_BlockCount;
	uint32_t m_DirectorySize;
	uint32_t m_Unknown1_32t;
};

struct MsfzHeader
{
	uint8_t m_Signature[0x20];
	uint64_t m_Unknown1_64t;
	uint32_t m_StreamDirectoryDataOffset;
	uint32_t m_StreamDirectoryDataOrigin;
	uint32_t m_ChunkMetadataOffset;
	uint32_t m_ChunkMetadataOrigin;
	uint32_t m_NumMSFStreams;
	uint32_t m_IsStreamDirectoryDataCompressed;
	uint32_t m_StreamDirectoryDataLengthCompressed;
	uint32_t m_StreamDirectoryDataLengthDecompressed;
	uint32_t m_NumChunks;
	uint32_t m_ChunkMetadataLength;
};

struct MsfzChunk
{
	uint32_t m_OffsetToChunkData;
	uint32_t m_OriginToChunk;
	uint32_t m_IsCompressed;
	uint32_t m_CompressedSize;
	uint32_t m_DecompressedSize;
};

struct MsfzFragment
{
	uint32_t m_DataSize;
	uint32_t m_DataOffset;
	uint32_t m_ChunkIndexOrDataOrigin;

	void SetChunkIndex(uint32_t value) { m_ChunkIndexOrDataOrigin = (value) | (1 << 31); }
	uint32_t GetChunkIndex() const { return m_ChunkIndexOrDataOrigin & ~(1 << 31); }
	bool IsLocatedInChunk() const { return m_ChunkIndexOrDataOrigin & (1 << 31); }
};

struct MsfzStream
{
	std::vector<MsfzFragment> m_Fragments;
	uint32_t CalculateSize() const
	{
		size_t sizeValue = 0;
		for (const MsfzFragment& fragmentDesc : m_Fragments)
		{
			sizeValue = ynw::StrictCastTo<uint32_t>(sizeValue + fragmentDesc.m_DataSize);
		}
		return ynw::StrictCastTo<uint32_t>(sizeValue);
	}
};