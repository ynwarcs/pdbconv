#include "definitions.h"
#include "compression.h"
#include "decompression.h"
#include "y_thread.h"

#include <filesystem>

namespace Testing
{
	// Update manually if it changes, too lazy to have a generic solution...
	constexpr uint32_t k_NumTests = 121;
	ynw::LogProgressTracker* g_CurrentProgressTracker;
	std::string g_OutputFolderPath;

	namespace PDB2MSFZ
	{
		std::string GetOutputFileName(ProgramCommandLineArgs& args);
	}

	namespace MSFZ2PDB
	{
		std::string GetOutputFileName(ProgramCommandLineArgs& args)
		{
			std::string name = std::filesystem::path(args.m_InputFilePath).filename().replace_extension().string();
			name += "_b{" + std::to_string(args.m_BlockSize.value()) + "}";
			name += "_converted.pdb";
			return g_OutputFolderPath + "\\" + name;
		}

		void TestWithArgs(ProgramCommandLineArgs args)
		{
			bool decompressSuccess = false;
			args.m_OutputFilePath = GetOutputFileName(args);
			g_CurrentProgressTracker->UpdateProgress(1);
			{
				SuppressLogInScope();
				decompressSuccess = Decompression::RunDecompression(args);
			}
			
			args.m_InputFilePath = args.m_OutputFilePath;
			args.m_CompressionStrategy = CompressionStrategy::NoCompression;
			args.m_CompressionLevel = 3u;
			args.m_OutputFilePath = PDB2MSFZ::GetOutputFileName(args);
			g_CurrentProgressTracker->UpdateProgress(1);
			{
				SuppressLogInScope();
				if (decompressSuccess)
				{
					Compression::RunCompression(args);
				}
			}
		}

		void TestDifferentBlockSizes(const char* inputPath)
		{
			ProgramCommandLineArgs args = {};
			args.m_InputFilePath = inputPath;
			for (const uint32_t blockSize : {0x200, 0x400, 0x800, 0x1000, 0x2000})
			{
				args.m_BlockSize = blockSize;
				TestWithArgs(args);
			}
		}

		void TestAll(const char* inputPath)
		{
			TestDifferentBlockSizes(inputPath);
		}
	}

	namespace PDB2MSFZ
	{
		std::string GetOutputFileName(ProgramCommandLineArgs& args)
		{
			std::string name = std::filesystem::path(args.m_InputFilePath).filename().replace_extension().string();
			name += "_s{" + std::to_string(static_cast<uint8_t>(args.m_CompressionStrategy.value())) + "}";
			if (args.m_CompressionStrategy == CompressionStrategy::MultiFragment)
			{
				name += "_f{" + std::to_string(args.m_FixedFragmentSize.value()) + "}";
				name += "_m{" + std::to_string(args.m_MaxFragmentsPerStream.value()) + "}";
			}
			name += "_l{" + std::to_string(args.m_CompressionLevel.value()) + "}";
			name += "_msfz.pdb";
			return g_OutputFolderPath + "\\" + name;
		}

		void TestWithArgs(ProgramCommandLineArgs args)
		{
			args.m_OutputFilePath = GetOutputFileName(args);
			g_CurrentProgressTracker->UpdateProgress(1);
			{
				SuppressLogInScope();
				Compression::RunCompression(args);
			}

			// re-decompress and test
			MSFZ2PDB::TestAll(args.m_OutputFilePath.c_str());
		}

		void TestDefaultArgsSelectedStrategy(const char* inputPath, CompressionStrategy strategy)
		{
			ProgramCommandLineArgs args = {};
			args.m_InputFilePath = inputPath;
			args.m_CompressionStrategy = strategy;
			args.m_CompressionLevel = 3;
			TestWithArgs(args);
		}

		void TestDifferentStrategies(const char* inputPath)
		{
			TestDefaultArgsSelectedStrategy(inputPath, CompressionStrategy::NoCompression);
			TestDefaultArgsSelectedStrategy(inputPath, CompressionStrategy::SingleFragment);
		}

		void TestDifferentFragmentSizes(const char* inputPath)
		{
			ProgramCommandLineArgs args = {};
			args.m_InputFilePath = inputPath;
			args.m_CompressionStrategy = CompressionStrategy::MultiFragment;
			args.m_CompressionLevel = 3;
			for (const uint32_t fragmentSize : {0x100, 0x1000, 0x100000})
			{
				for (const uint32_t maxFrps : {0x2, 0x100, 0x3001})
				{
					args.m_FixedFragmentSize = fragmentSize;
					args.m_MaxFragmentsPerStream = maxFrps;
					TestWithArgs(args);
				}
			}
		}

		void TestEverything(const char* inputPath)
		{
			TestDifferentStrategies(inputPath);
			TestDifferentFragmentSizes(inputPath);
		}
	}

	void ProcessFile(const char* inputPath)
	{
		PDB2MSFZ::TestEverything(inputPath);
	}

	void RunBatch(const ProgramCommandLineArgs& args)
	{
		g_OutputFolderPath = args.m_OutputFilePath;

		std::vector<std::string> filesToProcess;
		for (const auto& entry : std::filesystem::directory_iterator(args.m_InputFilePath.c_str()))
		{
			if (entry.is_regular_file() && entry.path().extension() == ".pdb")
			{
				const std::string inputPath = entry.path().string();
				filesToProcess.push_back(inputPath);
			}
		}
		for (const std::string& filePath : filesToProcess)
		{
			const std::string progressMessage = std::string("Processing file ") + filePath;
			ynw::LogProgressTracker progressTracker(progressMessage, k_NumTests);
			g_CurrentProgressTracker = &progressTracker;
			ProcessFile(filePath.c_str());
			g_CurrentProgressTracker = nullptr;
		}
	}
}