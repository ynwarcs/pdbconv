#pragma once

#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <cstdarg>

namespace ynw
{
	static void ThrowArgsError(const char* formatString, ...)
	{
		printf("Error when parsing command line arguments: ");
		va_list argList;
		va_start(argList, formatString);
		vprintf(formatString, argList);
		va_end(argList);
		printf("\r\n\r\n");
	}

	class CommandLineOption
	{
	public:
		CommandLineOption(const char* name, const char* description)
			: m_Name(name)
			, m_Description(description)
			, m_ShortLetter('\0')
			, m_IsRequired(false)
			, m_RequiresValue(false)
			, m_IsPresent(false)
		{
		}

		CommandLineOption(const char shortLetter, const char* name, const char* description)
			: CommandLineOption(name, description)
		{
			SetShortLetter(shortLetter);
		}

		using CustomValidationCallback = bool(*)(const CommandLineOption*);

		bool IsPresent() const { return m_IsPresent; }
		void SetRequired(bool isRequired) { m_IsRequired = isRequired; }
		void SetExcludedOptions(const char* excludedOptions) { m_ExcludedOptions = excludedOptions; }
		void SetRequiredOptions(const char* requiredOptions) { m_RequiredOptions = requiredOptions; }
		void SetCustomValidationCallback(CustomValidationCallback&& callback) { m_ValidationCallback = callback; }
		void SetShortLetter(const char shortLetter) { m_ShortLetter = shortLetter;  RegisterShortLetter(shortLetter, m_Name); }

		void Print() const
		{
			if (m_ShortLetter != '\0')
			{
				printf("(-%c) ", m_ShortLetter);
			}
			printf("--%s", m_Name.c_str());
			if (m_RequiresValue)
				printf("={value}");
			printf("%s", m_Description.c_str());
			printf("\r\n");
		}

		virtual bool ParseValue(const char* /*arg*/) { return true; }
		bool Validate() const
		{
			for (const char excludedOpt : m_ExcludedOptions)
			{
				if (CommandLineOption* option = GetOption(excludedOpt))
				{
					if (option->m_IsPresent)
					{
						ThrowArgsError("--%s must not be specified at the same time as --%s", m_Name.c_str(), option->m_Name.c_str());
						return false;
					}
				}
			}

			bool hasRequiredOptionPresent = false;
			for (const char requiredOpt : m_RequiredOptions)
			{
				if (CommandLineOption* option = GetOption(requiredOpt))
				{
					hasRequiredOptionPresent |= option->m_IsPresent;
					if (!option->m_IsPresent)
					{
						return false;
					}
				}
			}

			if (!m_RequiredOptions.empty() && !hasRequiredOptionPresent)
			{
				ThrowArgsError("--%s cannot be specified in this context.", m_Name.c_str());
			}

			if (m_ValidationCallback != nullptr && !m_ValidationCallback(this))
			{
				return false;
			}

			return true;
		}
		bool ValidateRequiredOption() const
		{
			if (!m_IsRequired || m_IsPresent)
			{
				return true;
			}

			// check if any of the excluded options is enabled
			for (const char excludedOpt : m_ExcludedOptions)
			{
				if (CommandLineOption* option = GetOption(excludedOpt))
				{
					if (option->m_IsPresent)
					{
						return true;
					}
				}
			}

			if (m_RequiredOptions.empty())
			{
				if (m_ExcludedOptions.empty())
				{
					ThrowArgsError("--%s is required.", m_Name.c_str());
				}
				else
				{
					std::string errorMessage;
					errorMessage += "One of the following arguments is required: { "; 
					for (const char excludedOpt : m_ExcludedOptions)
					{
						if (CommandLineOption* option = GetOption(excludedOpt))
						{
							errorMessage += "--";
							errorMessage += option->m_Name;
							errorMessage += ", ";
						}
					}
					errorMessage += "--";
					errorMessage += m_Name;
					errorMessage += " }";
					ThrowArgsError(errorMessage.c_str());
				}
				return false;
			}

			for (const char requiredOpt : m_RequiredOptions)
			{
				if (CommandLineOption* option = GetOption(requiredOpt))
				{
					if (option->m_IsPresent)
					{
						ThrowArgsError("--%s is required when --%s is specified.", m_Name.c_str(), option->m_Name.c_str());
						return false;
					}
				}
			}

			return true;
		}

		static CommandLineOption* Parse(const char* arg)
		{
			const size_t argLength = strlen(arg);
			if (argLength < 2 || arg[0] != '-')
			{
				ThrowArgsError("Unexpected argument format: %s", arg);
				return nullptr;
			}

			char optionLetter = arg[1];
			std::string optionName;
			if (optionLetter != '-')
			{
				auto letterMapIt = g_ShortLetterToNameMap.find(optionLetter);
				if (letterMapIt != g_ShortLetterToNameMap.end())
				{
					optionName = letterMapIt->second;
				}
				else
				{
					ThrowArgsError("Unknown command line argument: -%c", optionLetter);
					return nullptr;
				}
			}
			else
			{
				optionName = &arg[2];
				for (uint32_t i = 0; i < optionName.size(); ++i)
				{
					if (!isalnum(optionName[i]) && optionName[i] != '_')
					{
						optionName.resize(i);
						break;
					}
				}
			}

			auto commandLineOptionsIt = g_AllCommandLineOptions.find(optionName);
			if (commandLineOptionsIt == g_AllCommandLineOptions.end())
			{
				ThrowArgsError("Unknown command line argument: --%s", optionName.c_str());
				return nullptr;
			}

			CommandLineOption* option = commandLineOptionsIt->second.get();
			if (option->m_RequiresValue)
			{
				if (!option->ParseValue(arg))
				{
					return nullptr;
				}
			}

			if (!option->Validate())
			{
				return nullptr;
			}

			option->m_IsPresent = true;
			return option;
		}
		template <typename T>
		static T* Register(const char* name, const char* description)
		{
			std::unique_ptr<T> optionPtr = std::make_unique<T>(name, description);
			T* option = optionPtr.get();
			g_AllCommandLineOptions.insert({ name, std::move(optionPtr) });
			return option;
		}
		template <typename T>
		static T* Register(const char shortLetter, const char* name, const char* description)
		{
			std::unique_ptr<T> optionPtr = std::make_unique<T>(shortLetter, name, description);
			T* option = optionPtr.get();
			g_AllCommandLineOptions.insert({ name, std::move(optionPtr) });
			return option;
		}
		template <typename T = CommandLineOption>
		static T* GetOption(const char* name)
		{
			auto commandLineOptionsIt = g_AllCommandLineOptions.find(name);
			if (commandLineOptionsIt != g_AllCommandLineOptions.end())
			{
				return static_cast<T*>(commandLineOptionsIt->second.get());
			}
			return nullptr;
		}
		template <typename T = CommandLineOption>
		static T* GetOption(const char shortLetter)
		{
			auto letterMapIt = g_ShortLetterToNameMap.find(shortLetter);
			if (letterMapIt != g_ShortLetterToNameMap.end())
			{
				return GetOption<T>(letterMapIt->second.c_str());
			}
			return nullptr;
		}
		static bool ValidateRequiredOptions()
		{
			for (auto& commandLineOptionIt : g_AllCommandLineOptions)
			{
				CommandLineOption* option = commandLineOptionIt.second.get();
				if (!option->ValidateRequiredOption())
				{
					return false;
				}
			}
			return true;
		}
		static const std::map<std::string, std::unique_ptr<CommandLineOption>>& GetAllOptions() { return g_AllCommandLineOptions; }

	protected:
		std::string m_Name;
		std::string m_Description;
		std::string m_RequiredOptions;
		std::string m_ExcludedOptions;
		CustomValidationCallback m_ValidationCallback = nullptr;
		char m_ShortLetter;
		bool m_IsRequired;
		bool m_RequiresValue;
		bool m_IsPresent;

		void RegisterShortLetter(const char shortLetter, const std::string& name)
		{
			if (g_ShortLetterToNameMap.find(shortLetter) != g_ShortLetterToNameMap.end())
			{
				ThrowArgsError("Trying to register a command line argument under the short letter %c that has already been used.", shortLetter);
				return;
			}

			g_ShortLetterToNameMap[shortLetter] = name;
		}

		static inline std::map<std::string, std::unique_ptr<CommandLineOption>> g_AllCommandLineOptions;
		static inline std::map<char, std::string> g_ShortLetterToNameMap;
	};

	class StringValueCommandLineOption : public CommandLineOption
	{
	public:
		StringValueCommandLineOption(const char shortLetter, const char* name, const char* description)
			: CommandLineOption(shortLetter, name, description)
		{
			m_RequiresValue = true;
		}
		StringValueCommandLineOption(const char* name, const char* description)
			: CommandLineOption(name, description)
		{
			m_RequiresValue = true;
		}

		void SetAcceptedValues(const std::vector<std::string>& acceptedValues)
		{
			m_AcceptedValues = acceptedValues;
		}

		bool ParseValue(const char* arg) override
		{
			const char* equalsPos = strchr(arg, '=');
			if (equalsPos == nullptr)
			{
				ThrowArgsError("Unexpected argument format: %s", arg);
				return false;
			}
			const char* argValue = equalsPos + 1;
			m_Value = std::string(argValue);
			if (!m_AcceptedValues.empty())
			{
				if (std::find(m_AcceptedValues.begin(), m_AcceptedValues.end(), m_Value) == m_AcceptedValues.end())
				{
					std::string errorMessage;
					errorMessage += "Value (";
					errorMessage += m_Value;
					errorMessage += ") for argument --";
					errorMessage += m_Name;
					errorMessage += "is not among accepted values: { ";
					for (const std::string& acceptedValue : m_AcceptedValues)
					{
						errorMessage += acceptedValue + ", ";
					}
					errorMessage.resize(errorMessage.size() - 2);
					ThrowArgsError(errorMessage.c_str());
					return false;
				}
			}
			return true;
		}

		const std::string& GetValue() const { return m_Value; }

	private:
		std::string m_Value;
		std::vector<std::string> m_AcceptedValues;
	};

	class IntegerValueCommandLineOption : public CommandLineOption
	{
	public:
		IntegerValueCommandLineOption(const char shortLetter, const char* name, const char* description)
			: CommandLineOption(shortLetter, name, description)
			, m_MinValue(0)
			, m_MaxValue(SIZE_MAX)
			, m_Value(0)
		{
			m_RequiresValue = true;
		}
		IntegerValueCommandLineOption(const char* name, const char* description)
			: CommandLineOption(name, description)
			, m_MinValue(0)
			, m_MaxValue(SIZE_MAX)
			, m_Value(0)
		{
			m_RequiresValue = true;
		}

		void SetDefaultValue(size_t defaultValue)
		{
			m_Value = defaultValue;
		}
		void SetMinValue(size_t minValue)
		{
			m_MinValue = minValue;
		}
		void SetMaxValue(size_t maxValue)
		{
			m_MaxValue = maxValue;
		}

		bool ParseValue(const char* arg) override
		{
			const char* equalsPos = strchr(arg, '=');
			if (equalsPos == nullptr)
			{
				ThrowArgsError("Unexpected argument format: %s", arg);
				return false;
			}
			const char* argValue = equalsPos + 1;
			m_Value = std::atoi(argValue);
			if (m_Value < m_MinValue || m_Value > m_MaxValue)
			{
				ThrowArgsError("Value %u for argument --%s is not between min value (%u) and max value (%u)", m_Value, m_Name.c_str(), m_MinValue, m_MaxValue);
				return false;
			}
			return true;
		}

		size_t GetValue() const { return m_Value; }

	private:
		size_t m_Value;
		size_t m_MinValue;
		size_t m_MaxValue;
	};

	static bool ParseCommandLineOptions(const int argc, const char** argv)
	{
		if (argc < 2)
		{
			return false;
		}

		for (int argIndex = 1; argIndex < argc; ++argIndex)
		{
			if (!CommandLineOption::Parse(argv[argIndex]))
			{
				return false;
			}
		}

		return CommandLineOption::ValidateRequiredOptions();
	}

	static int PrintArgsUsage(const char* programName)
	{
		printf("Usage: %s [args]\r\n", programName);
		printf("Arguments:\r\n");

		for (const auto& optionIt : CommandLineOption::GetAllOptions())
		{
			const CommandLineOption* option = optionIt.second.get();
			option->Print();
		}
		return 0;
	}
}