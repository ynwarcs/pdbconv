#include "y_file.h"
#include "y_misc.h"
#include "y_data.h"
#include "y_container.h"
#include "y_log.h"
#include "y_thread.h"

#include "definitions.h"
#include "decompression.h"

#include <zstd.h>
#include <map>
#include <fstream>
#include <numeric>

using namespace ynw;

namespace Decompression
{
	constexpr uint32_t k_PrimaryFreeBlockMapBlockIndex = 1;
	constexpr uint32_t k_AlternateFreeBlockMapBlockIndex = 2;
	constexpr uint32_t k_FirstGeneralUseBlockIndex = 3;

	// limits
	constexpr uint32_t k_MaxNumStreams = 0x10000;
	constexpr uint32_t k_MaxNumBlocks = 1u << 20;

	// This stream class handles "holes" in a contiguous block of data we're supposed to write to
	// i.e. because we have blocks we can't use (FPM blocks), we need to make sure that no data is written there
	class MutableStreamFixedWithHoles : public MutableStreamFixed
	{
	public:
		using MutableStreamFixed::MutableStreamFixed;

		MutableStreamFixedWithHoles(MutableStreamFixed&& sourceStream)
			: MutableStreamFixed(sourceStream)
		{
		}

		void AddHole(const uint64_t beginOffset, const uint64_t endOffset)
		{
			assert(beginOffset < GetSize() && endOffset <= GetSize());
			m_HoleOffsets.emplace_back(beginOffset, endOffset);
		}

		bool WriteBytes(const void* data, size_t dataLengthInBytes) override
		{
			std::vector<std::pair<uint64_t, uint64_t>> writeSpans = AdjustWriteSpans(m_Offset, m_Offset + dataLengthInBytes);
			if (writeSpans.back().second > AdjustOffset(m_Size))
			{
				assert(false);
				return false;
			}

			const uint8_t* srcData = reinterpret_cast<const uint8_t*>(data);
			for (const auto& span : writeSpans)
			{
				const uint64_t numBytesToWrite = span.second - span.first;
				memcpy(m_Data + span.first, srcData, StrictCastTo<size_t>(numBytesToWrite));
				srcData += numBytesToWrite;
			}
			m_Offset += dataLengthInBytes;
			return true;
		}

		MutableStreamFixedWithHoles GetSubStreamAtOffset(uint64_t offset, uint64_t size = 0) const
		{
			if (size == 0)
			{
				size = m_Size - offset;
			}

			const uint64_t realOffset = AdjustOffset(offset);
			const uint64_t realSize = AdjustOffset(offset + size) - realOffset;
			MutableStreamFixedWithHoles resultStream = MutableStreamFixed::GetStreamAtOffset(realOffset, size);
			for (const auto& offsetPair : m_HoleOffsets)
			{
				const uint64_t beginOffsetClamped = std::clamp(offsetPair.first, realOffset, realOffset + realSize);
				const uint64_t endOffsetClamped = std::clamp(offsetPair.second, realOffset, realOffset + realSize);
				if (beginOffsetClamped != endOffsetClamped)
				{
					resultStream.AddHole(beginOffsetClamped - realOffset, endOffsetClamped - realOffset);
				}
			}
			return resultStream;
		}

	private:
		// disallow the normal stream splitting, as it returns a contiguous stream
		using MutableStreamFixed::GetStreamAtOffset;

		std::vector<std::pair<uint64_t, uint64_t>> AdjustWriteSpans(uint64_t beginSpan, uint64_t endSpan) const
		{
			std::vector<std::pair<uint64_t, uint64_t>> resultWriteSpans;
			uint64_t currentBeginSpan = AdjustOffset(beginSpan);
			uint64_t currentEndSpan = AdjustOffset(endSpan);
			for (const auto& offsetPair : m_HoleOffsets)
			{
				if (offsetPair.first >= currentBeginSpan && offsetPair.first < currentEndSpan)
				{
					if (currentBeginSpan != offsetPair.first)
					{
						resultWriteSpans.emplace_back(currentBeginSpan, offsetPair.first);
					}
					currentBeginSpan = offsetPair.second;
				}
			}
			assert(currentBeginSpan <= currentEndSpan);
			resultWriteSpans.emplace_back(currentBeginSpan, currentEndSpan);
			return resultWriteSpans;
		}

		uint64_t AdjustOffset(uint64_t offset) const
		{
			uint64_t resultOffset = offset;
			for (const auto& offsetPair : m_HoleOffsets)
			{
				if (offsetPair.first <= resultOffset)
				{
					resultOffset += offsetPair.second - offsetPair.first;
				}
			}
			return resultOffset;
		}

		std::vector<std::pair<uint64_t, uint64_t>> m_HoleOffsets;
	};

	MutableStreamFixedWithHoles GetStreamFromBlockIndices(const MutableStreamFixed& sourceStream, const std::vector<uint32_t>& blockIndices, const uint32_t blockSize)
	{
		if (blockIndices.size() == 0)
		{
			return MutableStreamFixedWithHoles{ nullptr, 0 };
		}

		const uint32_t firstBlockIndex = blockIndices.front();
		const uint32_t lastBlockIndex = blockIndices.back();
		MutableStreamFixedWithHoles resultStream = sourceStream.GetStreamAtOffset(blockSize * firstBlockIndex, (lastBlockIndex - firstBlockIndex + 1) * blockSize);
		uint32_t prevRelBlockIndex = 0;
		for (uint32_t i = 1; i < blockIndices.size(); ++i)
		{
			const uint32_t relBlockIndex = blockIndices[i] - firstBlockIndex;
			if (relBlockIndex != prevRelBlockIndex + 1)
			{
				resultStream.AddHole((prevRelBlockIndex + 1) * blockSize, relBlockIndex * blockSize);
			}
			prevRelBlockIndex = relBlockIndex;
		}

		return resultStream;
	}

	bool IsBlockReserved(const uint32_t blockIndex, const uint32_t blockSize)
	{
		// Each block of form (k * blockSize + freeBlockMapIndex) is a free block map block... 
		return blockIndex % blockSize == k_PrimaryFreeBlockMapBlockIndex || blockIndex % blockSize == k_AlternateFreeBlockMapBlockIndex;
	}

	void GetStreamDirectoryData(ImmutableStream& msfzFileStream, const MsfzHeader* header, ReadOnlyVector<uint8_t>& outStreamDirectoryData)
	{
		if (const uint8_t* streamDataDirectoryInFile = msfzFileStream.PeekAtOffset<const uint8_t>(header->m_StreamDirectoryDataOffset))
		{
			if (!msfzFileStream.CanRead(header->m_StreamDirectoryDataOffset, header->m_StreamDirectoryDataLengthCompressed))
			{
				ThrowError("Unable to read directory data. The data is out of bounds of the input file.");
			}

			if (header->m_IsStreamDirectoryDataCompressed)
			{
				std::vector<uint8_t> streamDataDirectoryDecompressedBytes;
				streamDataDirectoryDecompressedBytes.resize(header->m_StreamDirectoryDataLengthDecompressed);
				const size_t numDecompressedBytes = ZSTD_decompress(
					streamDataDirectoryDecompressedBytes.data(),
					streamDataDirectoryDecompressedBytes.size(),
					streamDataDirectoryInFile,
					header->m_StreamDirectoryDataLengthCompressed);
				if (ZSTD_isError(numDecompressedBytes))
				{
					ThrowError("Error when decompressing stream directory bytes: %llx", numDecompressedBytes);
				}
				if (numDecompressedBytes < header->m_StreamDirectoryDataLengthDecompressed)
				{
					ThrowError("Error when decompressing stream directory bytes. Decompressed length is not equal to expected length: %u vs %u", numDecompressedBytes, header->m_StreamDirectoryDataLengthDecompressed);
				}
				outStreamDirectoryData.AssignOwned(streamDataDirectoryDecompressedBytes);
			}
			else
			{
				outStreamDirectoryData.AssignNonOwned({ streamDataDirectoryInFile, (size_t)header->m_StreamDirectoryDataLengthCompressed });
			}
		}
	}

	void ParseStreamDirectoryData(const std::span<const uint8_t>& streamDirectoryData, std::vector<MsfzStream>& outStreamDescriptors)
	{
		ImmutableStream streamDirectoryDataStream(streamDirectoryData.data(), streamDirectoryData.size());
		MsfzStream* currentDesc = nullptr;
		while (streamDirectoryDataStream.CanRead())
		{
			if (currentDesc == nullptr)
			{
				currentDesc = &outStreamDescriptors.emplace_back();
			}

			if (const uint32_t* separatorOrFragmentPtr = streamDirectoryDataStream.Peek<uint32_t>())
			{
				if (*separatorOrFragmentPtr == 0)
				{
					// separator;
					streamDirectoryDataStream.Read<uint32_t>();	// to confirm the read
					currentDesc = nullptr;
					continue;
				}
				else
				{
					if (const MsfzFragment* fragmentDesc = streamDirectoryDataStream.Read<MsfzFragment>())
					{
						currentDesc->m_Fragments.push_back(*fragmentDesc);
					}
					else
					{
						ThrowError("Unable to read data from the stream directory.");
					}
				}
			}
			else
			{
				ThrowError("Unable to read data from the stream directory.");
			}
		}
	}

	void GetChunkDescriptorsData(ImmutableStream& msfzFileStream, const MsfzHeader* header, ReadOnlyVector<MsfzChunk>& outChunkDescriptors)
	{
		if (!msfzFileStream.Seek(header->m_ChunkMetadataOffset) || !msfzFileStream.CanRead(header->m_ChunkMetadataLength))
		{
			ThrowError("Invalid data. Chunk metadata offset cannot be seeked to.");
		}

		if (header->m_ChunkMetadataLength % sizeof(MsfzChunk) != 0)
		{
			ThrowError("Invalid chunk metadata length. Must be a multiple of sizeof(MsfzChunk) = %u", sizeof(MsfzChunk));
		}

		if (header->m_NumChunks * sizeof(MsfzChunk) != header->m_ChunkMetadataLength)
		{
			ThrowError("Chunk metadata length and number of chunks mismatch.");
		}

		outChunkDescriptors.AssignNonOwned({ msfzFileStream.Peek<MsfzChunk>(), header->m_NumChunks });
	}

	void AssignBlocksToStreams(const std::span<MsfzStream>& streamDescriptors, 
		const uint32_t blockSize, 
		std::vector<std::vector<uint32_t>>& outBlocksForStreams, 
		std::vector<uint32_t>& outBlocksForDirectory, 
		std::vector<uint32_t>& outBlocksForDirectoryIndices,
		std::vector<uint32_t>& outBlocksForFreeBlockMap,
		uint32_t& outNumBlocks)
	{
		size_t numBlocksForStreams = 0;
		size_t totalNumBytesForDirectory = sizeof(uint32_t);

		auto AssignNextNBlocks = [blockSize](const uint32_t nBlocks, uint32_t& currentBlockIndex, std::vector<uint32_t>& outBlockIndices)
			{
				outBlockIndices.reserve(nBlocks);
				for (uint32_t i = 0; i < nBlocks; ++i)
				{
					uint32_t assignedBlockIndex = currentBlockIndex++;
					while (IsBlockReserved(assignedBlockIndex, blockSize))
					{
						assignedBlockIndex = currentBlockIndex++;
					}
					outBlockIndices.push_back(assignedBlockIndex);
				}
			};

		uint32_t currentBlockIndex = k_FirstGeneralUseBlockIndex;	// start from the first non-reserved block
		{
			// first handle blocks used for regular streams
			for (const MsfzStream& streamDesc : streamDescriptors)
			{
				const uint32_t streamSize = streamDesc.CalculateSize();
				const uint32_t numBlocksRequired = AlignTo(streamSize, blockSize) / blockSize;
				std::vector<uint32_t>& blockIndices = outBlocksForStreams.emplace_back();
				AssignNextNBlocks(numBlocksRequired, currentBlockIndex, blockIndices);

				totalNumBytesForDirectory += sizeof(uint32_t) + sizeof(uint32_t) * numBlocksRequired;
				numBlocksForStreams += numBlocksRequired;
			}
		}

		// now handle directory blocks
		const uint32_t numBlocksForDirectory = StrictCastTo<uint32_t>(AlignTo(totalNumBytesForDirectory, blockSize) / blockSize);
		AssignNextNBlocks(numBlocksForDirectory, currentBlockIndex, outBlocksForDirectory);

		// directory indices
		const uint32_t numBlocksForDirectoryIndices = StrictCastTo<uint32_t>(AlignTo(numBlocksForDirectory * sizeof(uint32_t), blockSize) / blockSize);
		AssignNextNBlocks(numBlocksForDirectoryIndices, currentBlockIndex, outBlocksForDirectoryIndices);

		// fpm
		const uint32_t maxBlockIndex = currentBlockIndex;
		const uint32_t numBlocksForFreeBlockMap = AlignTo(AlignTo(maxBlockIndex, 8u) / 8u, blockSize) / blockSize;
		for (uint32_t i = 0; i < numBlocksForFreeBlockMap; ++i)
		{
			const uint32_t blockIndex = i * blockSize + k_PrimaryFreeBlockMapBlockIndex;
			assert(blockIndex < maxBlockIndex);
			outBlocksForFreeBlockMap.push_back(blockIndex);
		}

		outNumBlocks = maxBlockIndex;
	}

	void WriteSingleStreamDataToPDB(ImmutableStream& msfzFileStream,
		const std::span<const MsfzChunk>& chunkDescriptors,
		const MsfzStream& streamDesc,
		MutableStreamFixed& outputStream)
	{
		uint64_t totalStreamSize = 0;
		for (const MsfzFragment& fragmentDesc : streamDesc.m_Fragments)
		{
			ReadOnlyVector<uint8_t> fragmentData;
			ReadOnlyVector<uint8_t> chunkData;
			if (!fragmentDesc.IsLocatedInChunk())
			{
				// fragment is located in the first page
				if (!msfzFileStream.CanRead(fragmentDesc.m_DataOffset, fragmentDesc.m_DataSize))
				{
					ThrowError("Invalid data. Offset in first page cannot be seeked to.");
				}

				fragmentData.AssignNonOwned({ msfzFileStream.PeekAtOffset<uint8_t>(fragmentDesc.m_DataOffset), fragmentDesc.m_DataSize });
			}
			else
			{
				const uint32_t chunkIndex = fragmentDesc.GetChunkIndex();
				if (chunkIndex >= chunkDescriptors.size())
				{
					ThrowError("Invalid chunk index specified in a fragment descriptor. Index = %u, Number of chunks = %llu", chunkIndex, chunkDescriptors.size());
				}
				const MsfzChunk& chunkDesc = chunkDescriptors[chunkIndex];
				if (fragmentDesc.m_DataOffset > chunkDesc.m_DecompressedSize || fragmentDesc.m_DataOffset + fragmentDesc.m_DataSize > chunkDesc.m_DecompressedSize)
				{
					ThrowError("Invalid data. Fragment goes out of bounds of its corresponding chunk.");
				}
				
				if (!msfzFileStream.CanRead(chunkDesc.m_OffsetToChunkData, chunkDesc.m_CompressedSize))
				{
					ThrowError("Invalid data. Chunk is located outside of bounds of the file.");
				}

				if (chunkDesc.m_IsCompressed)
				{
					std::vector<uint8_t> decompressedChunkData(chunkDesc.m_DecompressedSize);
					const size_t decompressedSizeResult = ZSTD_decompress(decompressedChunkData.data(), decompressedChunkData.size(), msfzFileStream.PeekAtOffset<uint8_t>(chunkDesc.m_OffsetToChunkData), chunkDesc.m_CompressedSize);

					if (ZSTD_isError(decompressedSizeResult))
					{
						ThrowError("Error when decompressing stream data: %s", ZSTD_getErrorName(decompressedSizeResult));
					}
					if (decompressedSizeResult < chunkDesc.m_DecompressedSize)
					{
						ThrowError("Error when decompressing stream data. Decompressed length is not equal to expected length: %u vs %u", decompressedSizeResult, chunkDesc.m_DecompressedSize);
					}
					chunkData.AssignOwned(decompressedChunkData);
				}
				else
				{
					// just shallow assign from the file stream
					chunkData.AssignNonOwned({ msfzFileStream.PeekAtOffset<uint8_t>(chunkDesc.m_OffsetToChunkData), chunkDesc.m_CompressedSize });
				}

				fragmentData.AssignNonOwned({ chunkData.GetData() + fragmentDesc.m_DataOffset, fragmentDesc.m_DataSize });
			}

			totalStreamSize += fragmentData.GetSize();
			outputStream.WriteSpan(fragmentData.GetSpan());
		}
	}

	void WriteStreamsAndDirectoryToPDB(ImmutableStream& msfzFileStream, 
		const std::span<const MsfzChunk>& chunkDescriptors,
		const std::span<const MsfzStream>& streamDescriptors, 
		const uint32_t blockSize,
		const std::vector<uint32_t>& blockIndicesForDirectory,
		const std::vector<std::vector<uint32_t>>& blockIndicesForStreams,
		MutableStreamFixed& outputFileStream,
		uint32_t& outDirectorySizeInBytes)
	{
		const uint32_t numStreams = StrictCastTo<uint32_t>(streamDescriptors.size());
		LogProgressTracker m_ProgressLog("Converting streams", numStreams);

		// for progress tracking
		const uint64_t allStreamsSize = std::accumulate(streamDescriptors.begin(), streamDescriptors.end(), 0ull, [](uint64_t sumSoFar, const MsfzStream& desc) { return sumSoFar + desc.CalculateSize(); });

		ParallelForRunner streamConversionRunner(streamDescriptors);
		streamConversionRunner.SetScoreFunction([](const MsfzStream& element, uint32_t /*elementIndex*/) { return element.CalculateSize(); });
		streamConversionRunner.Execute([&](const MsfzStream& streamDesc, uint32_t streamIndex)
			{
				const std::vector<uint32_t>& blockIndices = blockIndicesForStreams[streamIndex];
				MutableStreamFixedWithHoles streamDataStream = GetStreamFromBlockIndices(outputFileStream, blockIndices, blockSize);
				WriteSingleStreamDataToPDB(msfzFileStream, chunkDescriptors, streamDesc, streamDataStream);

				m_ProgressLog.UpdateProgress(1, streamDescriptors[streamIndex].CalculateSize() * 1.0f / allStreamsSize);
			});


		// split the directory data stream into two streams: one for stream sizes and another for block indices
		MutableStreamFixedWithHoles directoryDataStream = GetStreamFromBlockIndices(outputFileStream, blockIndicesForDirectory, blockSize);
		MutableStreamFixedWithHoles streamSizesStream = directoryDataStream.GetSubStreamAtOffset(sizeof(uint32_t), numStreams * sizeof(uint32_t));
		MutableStreamFixedWithHoles blockIndicesStream = directoryDataStream.GetSubStreamAtOffset(sizeof(uint32_t) + numStreams * sizeof(uint32_t));

		size_t directorySizeInBytes = sizeof(uint32_t);
		directoryDataStream.Write(numStreams);

		// write directory data for this stream: stream size + block indices
		for (uint32_t streamIndex = 0; streamIndex < numStreams; ++streamIndex)
		{
			const std::vector<uint32_t>& blockIndices = blockIndicesForStreams[streamIndex];
			streamSizesStream.Write(streamDescriptors[streamIndex].CalculateSize());
			blockIndicesStream.WriteSpan<uint32_t>(blockIndices);
			directorySizeInBytes += sizeof(uint32_t) + sizeof(uint32_t) * blockIndices.size();
		}

		outDirectorySizeInBytes = StrictCastTo<uint32_t>(directorySizeInBytes);
	}

	bool RunDecompression(const ProgramCommandLineArgs& args)
	{
		SimpleWinFile msfzFile(args.m_InputFilePath.c_str());
		{
			LogScoped("Opening input file");
			if (!msfzFile.Open(false))
			{
				ThrowError("Unable to open input file.");
			}
		}

		ImmutableStream fileStream(msfzFile.GetData(), msfzFile.GetSize());
		if (const MsfzHeader* header = fileStream.Read<MsfzHeader>())
		{
			if (memcmp(header->m_Signature, g_MsfzSignatureBytes, sizeof(g_MsfzSignatureBytes)) != 0)
			{
				ThrowError("Signature mismatch. Expected MSFZ signature at the beginning of the input file.");
			}

			// parse stream directory
			std::vector<MsfzStream> streamDescriptors;
			{
				LogScoped("Parsing stream directory");
				ReadOnlyVector<uint8_t> streamDirectoryData;
				GetStreamDirectoryData(fileStream, header, streamDirectoryData);
				ParseStreamDirectoryData(streamDirectoryData, streamDescriptors);
				if (streamDescriptors.size() != header->m_NumMSFStreams)
				{
					ThrowError("Number of MSF streams in the directory data doesn't match the count specified in the MSFZ header: %u vs %u", streamDescriptors.size(), header->m_NumMSFStreams);
				}
			}

			// get chunk data
			ReadOnlyVector<MsfzChunk> chunkDescriptors;
			{
				LogScoped("Fetching chunk metadata");
				GetChunkDescriptorsData(fileStream, header, chunkDescriptors);
			}

			const uint32_t blockSize = args.m_BlockSize.value();
			std::vector<std::vector<uint32_t>> blockIndicesForStreams;
			std::vector<uint32_t> blockIndicesForDirectory;
			std::vector<uint32_t> blockIndicesForDirectoryIndices;
			std::vector<uint32_t> blockIndicesForFPM;
			uint32_t numBlocksTotal = 0;
			AssignBlocksToStreams(streamDescriptors, blockSize, blockIndicesForStreams, blockIndicesForDirectory, blockIndicesForDirectoryIndices, blockIndicesForFPM, numBlocksTotal);

			if (numBlocksTotal > k_MaxNumBlocks)
			{
				if (!IsTestMode())
				{
					ThrowError("Block size %u requires %u blocks but the maximum is %u.", blockSize, numBlocksTotal, k_MaxNumBlocks);
				}
				else
				{
					return false;
				}
			}
			if (streamDescriptors.size() > k_MaxNumStreams)
			{
				if (!IsTestMode())
				{
					ThrowError("Too many streams: %u, maximum is %u.", streamDescriptors.size(), k_MaxNumStreams);
				}
				else
				{
					return false;
				}
			}

			// open output file for writing
			const size_t totalSizeOfOutputFile = numBlocksTotal * blockSize;
			SimpleWinFile outputFile(args.m_OutputFilePath.c_str());
			{
				if (!outputFile.Open(true))
				{
					ThrowError("Unable to open output file for writing.");
				}
				if (!outputFile.Resize(totalSizeOfOutputFile))
				{
					ThrowError("Error resizing the output file to %llu bytes.", totalSizeOfOutputFile);
				}
			}

			MutableStreamFixed outputFileStream(static_cast<uint8_t*>(outputFile.GetData()), outputFile.GetSize());

			// write streams and directory to PDB
			uint32_t directorySizeInBytes = 0;
			WriteStreamsAndDirectoryToPDB(fileStream, chunkDescriptors, streamDescriptors, blockSize, blockIndicesForDirectory, blockIndicesForStreams, outputFileStream, directorySizeInBytes);

			// write the superblock and directory indices
			{
				LogScoped("Writing directory indices");
				PDBSuperBlock outputSuperblock = {};
				memcpy(outputSuperblock.m_Signature, g_PdbSignatureBytes, sizeof(g_PdbSignatureBytes));
				memset(outputSuperblock.m_Padding, 0, sizeof(outputSuperblock.m_Padding));
				outputSuperblock.m_BlockSize = blockSize;
				outputSuperblock.m_DirectorySize = directorySizeInBytes;
				outputSuperblock.m_FreeBlockMapIndex = k_PrimaryFreeBlockMapBlockIndex;
				outputSuperblock.m_BlockCount = StrictCastTo<uint32_t>(numBlocksTotal);

				// directory block indices
				MutableStreamFixedWithHoles directoryIndicesDataStream = GetStreamFromBlockIndices(outputFileStream, blockIndicesForDirectoryIndices, blockSize);
				directoryIndicesDataStream.WriteSpan<uint32_t>(blockIndicesForDirectory);

				// superblock
				MutableStreamFixed superblockStream = outputFileStream.GetStreamAtOffset(0u);
				superblockStream.Write(outputSuperblock);
				superblockStream.WriteSpan<uint32_t>(blockIndicesForDirectoryIndices);
			}

			// write the free block map
			{
				LogScoped("Writing the free block map");
				DynamicBitset freeBlockMapBitset;
				const uint32_t numBlocksForFreeBlockMap = StrictCastTo<uint32_t>(blockIndicesForFPM.size());
				freeBlockMapBitset.Resize(numBlocksForFreeBlockMap * blockSize * 8);
				freeBlockMapBitset.SetAll();
				for (uint32_t i = 0; i < numBlocksTotal; ++i)
				{
					freeBlockMapBitset.Unset(i);
				}

				// since Feb 2023, stream 0 block has to be marked as free
				const std::vector<uint32_t>& blockIndicesForStreamZero = blockIndicesForStreams[0];
				if (blockIndicesForStreamZero.size() > 0)
				{
					const uint32_t streamZeroFirstBlockIndex = blockIndicesForStreamZero.front();
					const uint32_t streamZeroBlockCount = StrictCastTo<uint32_t>(blockIndicesForStreamZero.size());
					const uint32_t streamZeroLastBlockIndex = streamZeroFirstBlockIndex + streamZeroBlockCount;
					for (uint32_t i = streamZeroFirstBlockIndex; i < streamZeroLastBlockIndex; ++i)
					{
						freeBlockMapBitset.Set(i);
					}
				}

				MutableStreamFixedWithHoles freeBlockMapDataStream = GetStreamFromBlockIndices(outputFileStream, blockIndicesForFPM, blockSize);
				freeBlockMapDataStream.WriteSpan<uint8_t>(freeBlockMapBitset.GetSpan());
			}

			LogInfo("Input file size = %.2fMB, Output file size = %.2fMB. Decompression ratio = %.2f%%\r\n",
				msfzFile.GetSize() * 1.0f / (1 << 20),
				totalSizeOfOutputFile * 1.0f / (1 << 20),
				msfzFile.GetSize() * 100.0f / totalSizeOfOutputFile);
		}

		return true;
	}
}