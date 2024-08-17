#include "y_args.h"
#include "y_misc.h"
#include "y_data.h"
#include "y_file.h"
#include "y_thread.h"

#include "zstd.h"

#include "definitions.h"
#include "compression.h"
#include "decompression.h"
#include "test.h"

#include <vector>
#include <fstream>
#include <chrono>
#include <cstdio>

#include <map>
#include <cassert>

#pragma comment(lib, ZSTDLIB_PATH)

using namespace ynw;

static void RegisterCommandLineOptions()
{
	using namespace ynw;

	CommandLineOption* inputPathOption = CommandLineOption::Register<StringValueCommandLineOption>('i', "input", " | Path to the input file when using --compress or --decompress or the input directory when using --test.");
	inputPathOption->SetRequired(true);

	CommandLineOption* outputPathOption = CommandLineOption::Register<StringValueCommandLineOption>('o', "output", " | Path to the output file when using --compress or --decompress or the output directory when using --test.");
	outputPathOption->SetRequired(true);

	CommandLineOption* decompressOption = CommandLineOption::Register<CommandLineOption>('x', "decompress", " | Decompress input file in the MSFZ format to a regular PDB output file.");
	decompressOption->SetRequired(true);
	decompressOption->SetExcludedOptions("ct");

	CommandLineOption* compressOption = CommandLineOption::Register<CommandLineOption>('c', "compress", " | Compress input PDB file to a MSFZ format output file.");
	compressOption->SetRequired(true);
	compressOption->SetExcludedOptions("xt");

	StringValueCommandLineOption* strategyOption = CommandLineOption::Register<StringValueCommandLineOption>('s', "strategy", " (NoCompression, SingleFragment, MultiFragment) | Compression strategy to use when using --compress.");
	strategyOption->SetRequired(true);
	strategyOption->SetRequiredOptions("c");
	strategyOption->SetAcceptedValues({ "NoCompression", "SingleFragment", "MultiFragment" });

	IntegerValueCommandLineOption* compressionLevelOption = CommandLineOption::Register<IntegerValueCommandLineOption>('l', "level", " (1-22, default 3) | ZSTD compression level to use when using --compress..");
	compressionLevelOption->SetRequiredOptions("c");
	compressionLevelOption->SetMinValue(1);
	compressionLevelOption->SetMaxValue(22);
	compressionLevelOption->SetDefaultValue(3);

	IntegerValueCommandLineOption* fixedFragmentSizeOption = CommandLineOption::Register<IntegerValueCommandLineOption>('f', "fragment_size", " (default 4096) | Fixed fragment size value to use when using --compress and --strategy=MultiFragment.");
	fixedFragmentSizeOption->SetRequiredOptions("c");
	fixedFragmentSizeOption->SetDefaultValue(0x1000);
	fixedFragmentSizeOption->SetCustomValidationCallback([](const CommandLineOption* /*fragmentSizeOption*/) -> bool
		{
			if (StringValueCommandLineOption* strategyOption = static_cast<StringValueCommandLineOption*>(CommandLineOption::GetOption('s')))
			{
				if (strategyOption->GetValue() == "MultiFragment")
				{
					return true;
				}
			}
			ThrowArgsError("Fixed fragment size can only be used when compression strategy is set to MultiFragment");
			return false;
		});

	IntegerValueCommandLineOption* maxFragmentsPerStreamOption = CommandLineOption::Register<IntegerValueCommandLineOption>('m', "max_frps", " (default 4096) | Maximum number of fragments per stream when using --compress and --strategy=MultiFragment.");
	maxFragmentsPerStreamOption->SetRequiredOptions("c");
	maxFragmentsPerStreamOption->SetDefaultValue(0x1000);
	maxFragmentsPerStreamOption->SetMinValue(2);
	maxFragmentsPerStreamOption->SetCustomValidationCallback([](const CommandLineOption* /*maxFragmentsPerStreamOption*/) -> bool
		{
			if (StringValueCommandLineOption* strategyOption = static_cast<StringValueCommandLineOption*>(CommandLineOption::GetOption('s')))
			{
				if (strategyOption->GetValue() == "MultiFragment")
				{
					return true;
				}
			}
			ThrowArgsError("Max frps option can only be used when compression strategy is set to MultiFragment");
			return false;
		});

	IntegerValueCommandLineOption* blockSizeOption = CommandLineOption::Register<IntegerValueCommandLineOption>('b', "block_size", " (default 4096) | Block size value to use for the output MSF streams when using --decompress.");
	blockSizeOption->SetRequiredOptions("x");
	blockSizeOption->SetDefaultValue(0x1000);
	blockSizeOption->SetCustomValidationCallback([](const CommandLineOption* /*blockSizeOption*/) -> bool
		{
			const std::vector<uint32_t> k_AcceptedBlockSizeValues = { 0x200, 0x400, 0x800, 0x1000, 0x2000 };
			if (IntegerValueCommandLineOption* blockSizeOption = static_cast<IntegerValueCommandLineOption*>(CommandLineOption::GetOption('b')))
			{
				if (std::find(k_AcceptedBlockSizeValues.begin(), k_AcceptedBlockSizeValues.end(), blockSizeOption->GetValue()) != k_AcceptedBlockSizeValues.end())
				{
					return true;
				}
			}
			ThrowArgsError("Block size must be one of { 0x200, 0x400, 0x800, 0x1000, 0x2000 }");
			return false;
		});

	CommandLineOption::Register<IntegerValueCommandLineOption>("thread_num", "(default 75% of processor count) | Number of threads to use for compression or decompression workflows.");

	CommandLineOption* testModeCommandLineOption = CommandLineOption::Register<CommandLineOption>('t', "test", " | Run test batch conversion on directory.");
	testModeCommandLineOption->SetRequired(true);
	testModeCommandLineOption->SetExcludedOptions("xc");
}

bool ParseCommandLineOptions(const int argc, const char** argv, ProgramCommandLineArgs& outArgs)
{
	if (!ynw::ParseCommandLineOptions(argc, argv))
	{
		return false;
	}

	const StringValueCommandLineOption* inputFileOption = CommandLineOption::GetOption<StringValueCommandLineOption>('i');
	assert(inputFileOption->IsPresent());
	outArgs.m_InputFilePath = inputFileOption->GetValue();

	const StringValueCommandLineOption* outputFileOption = CommandLineOption::GetOption<StringValueCommandLineOption>('o');
	if (outputFileOption->IsPresent())
	{
		outArgs.m_OutputFilePath = outputFileOption->GetValue();
	}

	const CommandLineOption* compressionOption = CommandLineOption::GetOption('c');
	const CommandLineOption* decompressionOption = CommandLineOption::GetOption('x');
	if (compressionOption->IsPresent())
	{
		outArgs.m_UsageMode = UsageMode::Compress;

		const StringValueCommandLineOption* strategyOption = CommandLineOption::GetOption<StringValueCommandLineOption>('s');
		assert(strategyOption->IsPresent());
		const std::string& strategy = strategyOption->GetValue();
		if (strategy == "None")
		{
			outArgs.m_CompressionStrategy = CompressionStrategy::NoCompression;
		}
		else if (strategy == "SingleFragment")
		{
			outArgs.m_CompressionStrategy = CompressionStrategy::SingleFragment;
		}
		else if (strategy == "MultiFragment")
		{
			outArgs.m_CompressionStrategy = CompressionStrategy::MultiFragment;

			const IntegerValueCommandLineOption* fragmentSizeOption = CommandLineOption::GetOption<IntegerValueCommandLineOption>('f');
			outArgs.m_FixedFragmentSize = StrictCastTo<uint32_t>(fragmentSizeOption->GetValue());

			const IntegerValueCommandLineOption* maxFragmentsPerStreamOption = CommandLineOption::GetOption<IntegerValueCommandLineOption>('m');
			outArgs.m_MaxFragmentsPerStream = StrictCastTo<uint32_t>(maxFragmentsPerStreamOption->GetValue());
		}
		else
		{
			assert(false);
		}

		const IntegerValueCommandLineOption* levelOption = CommandLineOption::GetOption<IntegerValueCommandLineOption>('l');
		outArgs.m_CompressionLevel = StrictCastTo<uint32_t>(levelOption->GetValue());
}
	else if (decompressionOption->IsPresent())
	{
		outArgs.m_UsageMode = UsageMode::Decompress;

		const IntegerValueCommandLineOption* strategyOption = CommandLineOption::GetOption<IntegerValueCommandLineOption>('b');
		outArgs.m_BlockSize = StrictCastTo<uint32_t>(strategyOption->GetValue());
	}
	else
	{
		outArgs.m_UsageMode = UsageMode::Batch;
	}

	const IntegerValueCommandLineOption* threadNumOption = CommandLineOption::GetOption<IntegerValueCommandLineOption>("thread_num");
	if (threadNumOption->IsPresent())
	{
		ThreadConfig::SetDefaultNumThreads(StrictCastTo<uint32_t>(threadNumOption->GetValue()));
	}

	return true;
}

int main(const int argc, const char** argv)
{
	RegisterCommandLineOptions();

	ProgramCommandLineArgs programArgs;
	if (!ParseCommandLineOptions(argc, argv, programArgs))
	{
		return ynw::PrintArgsUsage("pdbconv");
	}

	TimedScope m_Timer;
	if (programArgs.m_UsageMode == UsageMode::Compress)
	{
		Compression::RunCompression(programArgs);
	}
	else if (programArgs.m_UsageMode == UsageMode::Decompress)
	{
		Decompression::RunDecompression(programArgs);
	}
	else
	{
		IsTestMode() = true;
		Testing::RunBatch(programArgs);
	}

	LogInfo("Execution finished.");

	return 0;
}