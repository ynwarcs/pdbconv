#include "y_file.h"
#include "y_misc.h"
#include "y_data.h"
#include "y_container.h"
#include "y_log.h"
#include "y_thread.h"

#include "definitions.h"
#include "compression.h"

#include "zstd.h"

#include <span>
#include <vector>
#include <algorithm>
#include <numeric>

using namespace ynw;

namespace Compression
{
	struct PDBStreamInfo
	{
		uint32_t m_StreamSize = 0;
		std::vector<uint32_t> m_StreamBlockIndices;
	};

	uint32_t GetFragmentSizeForStream(const uint32_t streamSize, const ProgramCommandLineArgs& args)
	{
		// max frps takes precedence over fixed fragment size
		const CompressionStrategy compressionStrategy = args.m_CompressionStrategy.value();
		if (compressionStrategy == CompressionStrategy::MultiFragment)
		{
			const uint32_t fixedFragmentSize = args.m_FixedFragmentSize.value();
			const uint32_t maxFrps = args.m_MaxFragmentsPerStream.value();
			return std::min(streamSize, std::max(fixedFragmentSize, StrictCastTo<uint32_t>(AlignTo(streamSize, maxFrps)) / maxFrps));
		}
		else
		{
			return streamSize;
		}
	}

	void CalculateOutputRegionSizes(const std::span<const PDBStreamInfo>& streamInfos, 
		const ProgramCommandLineArgs& args,
		uint32_t& outDirectoryNumBytes,
		uint32_t& outChunkDescNumBytes,
		uint32_t& outChunkDataMaxNumBytes)
	{
		const CompressionStrategy compressionStrategy = args.m_CompressionStrategy.value();
		uint32_t numDirectoryBytes = 0;
		uint32_t numChunkDescBytes = 0;
		uint32_t maxNumChunkDataBytes = 0;
		for (const PDBStreamInfo& streamInfo : streamInfos)
		{
			const uint32_t streamSize = streamInfo.m_StreamSize; 
			uint32_t fragmentSize = 0;
			uint32_t numFragments = 0;
			if (streamSize != 0)
			{
				fragmentSize = GetFragmentSizeForStream(streamSize, args);
				numFragments = AlignTo(streamSize, fragmentSize) / fragmentSize;
			}
			numDirectoryBytes += sizeof(uint32_t) + sizeof(MsfzFragment) * numFragments;
			numChunkDescBytes += sizeof(MsfzChunk) * numFragments;

			if (compressionStrategy == CompressionStrategy::NoCompression)
			{
				maxNumChunkDataBytes += streamSize;
			}
			else
			{
				maxNumChunkDataBytes += numFragments * StrictCastTo<uint32_t>(ZSTD_compressBound(fragmentSize));
			}
		}

		outDirectoryNumBytes = numDirectoryBytes;
		outChunkDescNumBytes = numChunkDescBytes;
		outChunkDataMaxNumBytes = maxNumChunkDataBytes;
	}

	void CoalesceDataFromStream(ImmutableStream& pdbFileStream, const PDBStreamInfo& streamInfo, const uint32_t blockSize, ReadOnlyVector<uint8_t>& outStreamData)
	{
		const std::vector<uint32_t>& streamBlockIndices = streamInfo.m_StreamBlockIndices;
		const uint32_t streamSize = streamInfo.m_StreamSize;
		const bool areStreamBlocksContiguous = std::is_sorted(streamBlockIndices.begin(), streamBlockIndices.end()) && streamBlockIndices.back() - streamBlockIndices.front() <= streamBlockIndices.size();
		if (areStreamBlocksContiguous)
		{
			const uint32_t streamOffset = blockSize * streamBlockIndices.front();
			if (!pdbFileStream.CanRead(streamOffset, streamSize))
			{
				ThrowError("Unable to read stream data from the input file. Offset: %u, Size: %u", streamOffset, streamSize);
			}
			outStreamData.AssignNonOwned({ pdbFileStream.PeekAtOffset<uint8_t>(streamOffset), streamSize });
		}
		else
		{
			MutableStreamDynamic coalescedDataStream;
			coalescedDataStream.Reserve(streamSize);
			uint32_t streamSizeLeftover = streamSize;
			for (const uint32_t blockIndex : streamBlockIndices)
			{
				const uint32_t blockOffset = blockSize * blockIndex;
				const uint32_t sizeToRead = std::min(streamSizeLeftover, blockSize);
				if (!pdbFileStream.CanRead(blockOffset, sizeToRead))
				{
					ThrowError("Unable to read stream data from the input file. Offset: %u, Size: %u", blockOffset, sizeToRead);
				}
				coalescedDataStream.WriteBytes(pdbFileStream.PeekAtOffset<uint8_t>(blockOffset), sizeToRead);
				streamSizeLeftover -= sizeToRead;
			}
			std::vector<uint8_t> coalescedDataStreamData;
			coalescedDataStream.MoveTo(coalescedDataStreamData);
			outStreamData.AssignOwned(coalescedDataStreamData);
		}
	}

	void ParseStreamDirectory(ImmutableStream& pdbFileStream, const PDBSuperBlock* pdbSuperblock, std::vector<PDBStreamInfo>& outStreams)
	{
		const uint32_t blockSize = pdbSuperblock->m_BlockSize;
		const uint32_t directorySizeInBytes = pdbSuperblock->m_DirectorySize;

		// read directory indices stream info
		PDBStreamInfo directoryIndicesStreamInfo = {};
		const uint32_t directoryBlockIndicesSize = AlignTo(directorySizeInBytes, blockSize) / blockSize;
		const uint32_t directoryBlockIndicesByteSize = directoryBlockIndicesSize * sizeof(uint32_t);
		const uint32_t directoryBlockIndicesStreamSize = AlignTo(directoryBlockIndicesByteSize, blockSize) / blockSize;
		directoryIndicesStreamInfo.m_StreamSize = directoryBlockIndicesByteSize;
		directoryIndicesStreamInfo.m_StreamBlockIndices.resize(directoryBlockIndicesStreamSize);
		ImmutableStream directoryBlockIndicesStream = pdbFileStream.GetStreamAtOffset(sizeof(PDBSuperBlock), directoryBlockIndicesStreamSize * sizeof(uint32_t));
		directoryBlockIndicesStream.ReadData(directoryIndicesStreamInfo.m_StreamBlockIndices.data());

		// get directory stream indices data
		ReadOnlyVector<uint8_t> directoryIndicesData;
		CoalesceDataFromStream(pdbFileStream, directoryIndicesStreamInfo, blockSize, directoryIndicesData);
		ImmutableStream directoryIndicesStream(directoryIndicesData.GetData(), directoryIndicesData.GetSize());

		// read directory stream info
		PDBStreamInfo directoryStreamInfo = {};
		directoryStreamInfo.m_StreamSize = directorySizeInBytes;
		directoryStreamInfo.m_StreamBlockIndices.resize(directoryBlockIndicesSize);
		memcpy(directoryStreamInfo.m_StreamBlockIndices.data(), directoryIndicesData.GetData(), directoryBlockIndicesSize * sizeof(uint32_t));

		// finally, get the real directory stream data. mental.
		ReadOnlyVector<uint8_t> directoryData;
		CoalesceDataFromStream(pdbFileStream, directoryStreamInfo, blockSize, directoryData);
		ImmutableStream directoryStream(directoryData.GetData(), directoryData.GetSize());

		// setup number of streams
		const uint32_t* numStreamsPtr = directoryStream.PeekAtOffset<uint32_t>(0);
		if (numStreamsPtr == nullptr)
		{
			ThrowError("Unable to read the count of MSF streams from the input file.");
		}
		const uint32_t numStreams = *numStreamsPtr;
		outStreams.resize(numStreams);

		// parse directory data
		ImmutableStream streamSizesStream = directoryStream.GetStreamAtOffset(sizeof(uint32_t), sizeof(uint32_t) * numStreams);
		ImmutableStream blockIndicesStream = directoryStream.GetStreamAtOffset(sizeof(uint32_t) + sizeof(uint32_t) * numStreams);
		for (uint32_t streamIndex = 0; streamIndex < numStreams; ++streamIndex)
		{
			const uint32_t* streamSizePtr = streamSizesStream.Read<uint32_t>();
			PDBStreamInfo& streamInfo = outStreams[streamIndex];
			if (streamSizePtr == nullptr)
			{
				ThrowError("Unable to read size of the stream from the input file. Stream index: %u", streamIndex);
			}
			streamInfo.m_StreamSize = *streamSizePtr;

			if (streamInfo.m_StreamSize == UINT32_MAX || streamInfo.m_StreamSize == 0)
			{
				streamInfo.m_StreamSize = 0;
				continue;
			}

			std::vector<uint32_t>& streamBlockIndices = streamInfo.m_StreamBlockIndices;
			for (uint32_t blockIterator = 0; blockIterator < *streamSizePtr; blockIterator += blockSize)
			{
				const uint32_t* blockIndexPtr = blockIndicesStream.Read<uint32_t>();
				if (blockIndexPtr == nullptr)
				{
					ThrowError("Unable to read block indices from the input file.");
				}
				streamBlockIndices.push_back(*blockIndexPtr);
			}
		}
	}

	void WriteSingleStreamData(ImmutableStream& pdbFileStream,
		const PDBStreamInfo& streamInfo,
		const uint32_t blockSize,
		const uint32_t chunkDataOffset,
		const ProgramCommandLineArgs& args,
		SimpleMutableStreamFixedThreadSafe& outChunkDataStream,
		MsfzStream& outStreamDesc,
		SimpleMutableStreamFixedThreadSafe& outChunkMetadataStream)
	{
		const CompressionStrategy compressionStrategy = args.m_CompressionStrategy.value();
		const uint32_t compressionLevel = args.m_CompressionLevel.value();
		const uint32_t streamDataSize = streamInfo.m_StreamSize;

		if (streamDataSize > 0)
		{
			ReadOnlyVector<uint8_t> streamDataCoalesced;
			CoalesceDataFromStream(pdbFileStream, streamInfo, blockSize, streamDataCoalesced);
			const uint8_t* streamData = streamDataCoalesced.GetData();
			const uint32_t streamDataLength = StrictCastTo<uint32_t>(streamDataCoalesced.GetSize());
			const uint32_t maxFragmentSize = GetFragmentSizeForStream(streamDataLength, args);
			for (uint32_t dataOffset = 0; dataOffset < streamDataLength; dataOffset += maxFragmentSize)
			{
				uint64_t chunkDescOffset = 0;
				MutableStreamFixed chunkDescStream = outChunkMetadataStream.GetRegionSubstreamForWriting(sizeof(MsfzChunk), chunkDescOffset);
				const uint32_t chunkIndex = StrictCastTo<uint32_t>(chunkDescOffset / sizeof(MsfzChunk));

				const uint32_t fragmentSize = std::min(maxFragmentSize, streamDataLength - dataOffset);
				MsfzFragment& fragment = outStreamDesc.m_Fragments.emplace_back();
				fragment.SetChunkIndex(chunkIndex);
				fragment.m_DataSize = fragmentSize;
				fragment.m_DataOffset = 0;

				ReadOnlyVector<uint8_t> streamDataToWrite;
				if (compressionStrategy != CompressionStrategy::NoCompression)
				{
					std::vector<uint8_t> compressedStreamData;
					compressedStreamData.resize(ZSTD_compressBound(fragmentSize));
					const size_t compressedStreamDataLength = ZSTD_compress(
						compressedStreamData.data(),
						compressedStreamData.size(),
						(uint8_t*)streamData + dataOffset,
						fragmentSize,
						compressionLevel
					);

					if (ZSTD_isError(compressedStreamDataLength))
					{
						ThrowError("Error when compressing data: %llx", compressedStreamDataLength);
					}

					compressedStreamData.resize(compressedStreamDataLength);
					streamDataToWrite.AssignOwned(compressedStreamData);
				}
				else
				{
					streamDataToWrite.AssignNonOwned({ streamData, fragmentSize });
				}

				uint64_t chunkDataOffsetForWriting = 0;
				MutableStreamFixed chunkDataSubstreamForWriting = outChunkDataStream.GetRegionSubstreamForWriting(streamDataToWrite.GetSize(), chunkDataOffsetForWriting);
				chunkDataSubstreamForWriting.WriteBytes(streamDataToWrite.GetData(), streamDataToWrite.GetSize());

				MsfzChunk chunkDesc = {};
				chunkDesc.m_DecompressedSize = fragmentSize;
				chunkDesc.m_IsCompressed = compressionStrategy != CompressionStrategy::NoCompression;
				chunkDesc.m_OriginToChunk = 0;
				chunkDesc.m_OffsetToChunkData = StrictCastTo<uint32_t>(chunkDataOffset + chunkDataOffsetForWriting);
				chunkDesc.m_CompressedSize = StrictCastTo<uint32_t>(streamDataToWrite.GetSize());
				chunkDescStream.Write(chunkDesc);
			}
		}
	}

	void CompressAndWriteStreamData(ImmutableStream& pdbFile,
		const std::span<const PDBStreamInfo>& streamInfos,
		const ProgramCommandLineArgs& args,
		const uint32_t blockSize,
		const uint32_t chunkDataOffset,
		MsfzHeader& header,
		MutableStreamDynamic& outDirectoryDataStream,
		SimpleMutableStreamFixedThreadSafe& outChunkMetadataStream,
		SimpleMutableStreamFixedThreadSafe& outChunkDataStream)
	{
		const CompressionStrategy compressionStrategy = args.m_CompressionStrategy.value();

		const uint32_t numStreams = StrictCastTo<uint32_t>(streamInfos.size());;

		MutableStreamDynamic streamDirectoryDataStream;
		std::vector<MsfzStream> streamDescriptors(numStreams);
		{
			LogProgressTracker m_ProgressLog("Converting streams", StrictCastTo<uint32_t>(streamInfos.size()));

			// for progress tracking
			const size_t allStreamsSize = std::accumulate(streamInfos.begin(), streamInfos.end(), static_cast<size_t>(0u),
				[](size_t sumSoFar, const PDBStreamInfo& info) -> size_t { return sumSoFar + info.m_StreamSize; });

			ParallelForRunner streamCompressionRunner(streamInfos);
			streamCompressionRunner.SetScoreFunction([](const PDBStreamInfo& element, uint32_t /*elementIndex*/ ) { return element.m_StreamSize; });
			streamCompressionRunner.Execute([&](const PDBStreamInfo& streamInfo, uint32_t streamIndex)
				{
					MsfzStream& streamDesc = streamDescriptors[streamIndex];
					WriteSingleStreamData(pdbFile, streamInfo, blockSize, chunkDataOffset, args, outChunkDataStream, streamDesc, outChunkMetadataStream);

					m_ProgressLog.UpdateProgress(1, streamInfo.m_StreamSize * 1.0f / allStreamsSize);
				});

			// write stream desc to the directory stream
			for (const MsfzStream& streamDesc : streamDescriptors)
			{
				for (const MsfzFragment& fragmentDesc : streamDesc.m_Fragments)
				{
					streamDirectoryDataStream.Write(fragmentDesc);
				}
				streamDirectoryDataStream.Write<uint32_t>(0u);	// separator
			}
		}

		header.m_NumMSFStreams = numStreams;

		// compress the stream directory data if needed and write related values into the header
		{
			LogScoped("Compressing stream directory data");
			const size_t streamDirectoryDataLength = StrictCastTo<size_t>(streamDirectoryDataStream.GetSize());
			if (compressionStrategy != CompressionStrategy::NoCompression)
			{
				std::vector<uint8_t> compressedStreamDirectoryData(ZSTD_compressBound(streamDirectoryDataLength));
				const size_t compressedStreamDirectoryDataLength = ZSTD_compress(
					compressedStreamDirectoryData.data(),
					compressedStreamDirectoryData.size(),
					streamDirectoryDataStream.GetData(),
					streamDirectoryDataLength,
					3);
				if (ZSTD_isError(compressedStreamDirectoryDataLength))
				{
					ThrowError("Error when compressing data: 0x%llx", compressedStreamDirectoryDataLength);
				}
				compressedStreamDirectoryData.resize(compressedStreamDirectoryDataLength);
				outDirectoryDataStream.WriteSpan<uint8_t>(compressedStreamDirectoryData);
				header.m_IsStreamDirectoryDataCompressed = true;
			}
			else
			{
				outDirectoryDataStream = std::move(streamDirectoryDataStream);
				header.m_IsStreamDirectoryDataCompressed = false;
			}
		}
	}

	void RunCompression(const ProgramCommandLineArgs& args)
	{
		SimpleWinFile pdbFile(args.m_InputFilePath.c_str());
		{
			LogScoped("Opening input file");
			if (!pdbFile.Open(false))
			{
				ThrowError("Unable to open input file.");
			}
		}

		ImmutableStream fileStream(pdbFile.GetData(), pdbFile.GetSize());
		{
			const PDBSuperBlock* pdbSuperblock = fileStream.Peek<PDBSuperBlock>();
			if (pdbSuperblock == nullptr)
			{
				ThrowError("Unable to read PDB superblock from the input file.");
			}
			if (memcmp(pdbSuperblock->m_Signature, g_PdbSignatureBytes, sizeof(g_PdbSignatureBytes)) != 0)
			{
				ThrowError("Input file is not a PDB file.");
			}

			std::vector<PDBStreamInfo> streamInfos;
			{
				LogScoped("Parsing stream directory");
				ParseStreamDirectory(fileStream, pdbSuperblock, streamInfos);
			}

			uint32_t numBytesForDirectoryData = 0;
			uint32_t numBytesForChunkDescriptors = 0;
			uint32_t numBytesForChunkDataMax = 0;		// note: this is the maximum amount of bytes, not the actual amount of byte that chunk data will take up
			CalculateOutputRegionSizes(streamInfos, args, numBytesForDirectoryData, numBytesForChunkDescriptors, numBytesForChunkDataMax);

			SimpleWinFile outputFile(args.m_OutputFilePath.c_str());
			{
				LogScoped("Opening output file");
				if (!outputFile.Open(true))
				{
					ThrowError("Unable to open the output file for writing.");
				}

				const size_t outputFileSize = sizeof(MsfzHeader) + numBytesForDirectoryData + numBytesForChunkDescriptors + numBytesForChunkDataMax;
				if (!outputFile.Resize(outputFileSize))
				{
					ThrowError("Unable to resize the output file. Size = %llu", outputFileSize);
				}
			}

			// we serialize diferrent parts of data as: chunk metadata (descriptors) - chunk data - directory stream data. this is because:
			// 1) chunk metadata length can be easily calculated upfront, this gives a fixed offset in the output file where chunk data will reside
			// and allows us to write directly into the output file rather than using intermediate buffers.
			// 2) both chunk data and directory stream data can have variable length so we can't calculate a fixed offset
			// for at least one of them and must write it into an intermediate buffer. the easy choice is directory stream data, as it's much shorter than chunk data.
			const uint32_t chunkMetadataOffset = sizeof(MsfzHeader);
			const uint32_t chunkDataOffset = chunkMetadataOffset + numBytesForChunkDescriptors;
			// const uint32_t directoryDataOffset = ??? - to calculate after initial writing is done

			MsfzHeader header = {};
			static_assert(sizeof(MsfzHeader::m_Signature) == sizeof(g_MsfzSignatureBytes));
			memcpy(header.m_Signature, g_MsfzSignatureBytes, sizeof(g_MsfzSignatureBytes));

			// chunk metadata info, we calculated this upfront
			header.m_ChunkMetadataOffset = sizeof(MsfzHeader);
			header.m_ChunkMetadataLength = numBytesForChunkDescriptors;
			header.m_NumChunks = header.m_ChunkMetadataLength / sizeof(MsfzChunk);

			// main compression
			MutableStreamFixed outputFileStream(outputFile.GetData(), outputFile.GetSize());
			MutableStreamDynamic directoryDataStream;
			SimpleMutableStreamFixedThreadSafe chunkMetadataStream = outputFileStream.GetStreamAtOffset(header.m_ChunkMetadataOffset);
			SimpleMutableStreamFixedThreadSafe chunkDataStream = outputFileStream.GetStreamAtOffset(chunkDataOffset, numBytesForChunkDataMax);
			CompressAndWriteStreamData(fileStream, streamInfos, args, pdbSuperblock->m_BlockSize, chunkDataOffset, header, directoryDataStream, chunkMetadataStream, chunkDataStream);

			// now we know stream data + directory offsets and size
			const uint32_t streamDataFinalSize = StrictCastTo<uint32_t>(chunkDataStream.GetOffset());
			const uint32_t directoryDataOffset = StrictCastTo<uint32_t>(chunkDataOffset + streamDataFinalSize);
			const uint32_t directoryDataFinalSize = StrictCastTo<uint32_t>(directoryDataStream.GetSize());
			MutableStreamFixed directoryDataStreamFinal = outputFileStream.GetStreamAtOffset(directoryDataOffset, directoryDataFinalSize);
			directoryDataStreamFinal.WriteSpan<uint8_t>({ directoryDataStream.GetData(), directoryDataFinalSize });

			// directory stuff in the header
			header.m_StreamDirectoryDataOffset = directoryDataOffset;
			header.m_StreamDirectoryDataOrigin = 0;
			header.m_StreamDirectoryDataLengthCompressed = StrictCastTo<uint32_t>(directoryDataFinalSize);
			header.m_StreamDirectoryDataLengthDecompressed = StrictCastTo<uint32_t>(numBytesForDirectoryData);

			// write the header
			MutableStreamFixed headerStream = outputFileStream.GetStreamAtOffset(0u, sizeof(MsfzHeader));
			headerStream.Write(header);

			// finally, resize the file to its real length
			const uint64_t realFileLength = sizeof(MsfzHeader) + numBytesForChunkDescriptors + streamDataFinalSize + directoryDataFinalSize;
			outputFile.Resize(realFileLength);

			LogInfo("Input file size = %.2fMB, Output file size = %.2fMB. Compression ratio = %.2f%%\r\n",
				pdbFile.GetSize() * 1.0f / (1 << 20),
				realFileLength * 1.0f / (1 << 20),
				realFileLength * 100.0f / pdbFile.GetSize());
		}
	}
}