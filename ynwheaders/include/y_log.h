#pragma once

#include <cstdint>
#include <algorithm>
#include <string>
#include <cstdarg>
#include <chrono>
#include <mutex>

#define LogScoped(message) ynw::LogScopedVar uniqueScopedLog(message)
#define SuppressLogInScope() ynw::SuppressLogScope uniqueSuppressLog

namespace ynw
{
	struct SuppressLogScope
	{
		static inline std::atomic<uint32_t> g__SuppressLog = false;
		SuppressLogScope()
		{
			++g__SuppressLog;
		}

		~SuppressLogScope()
		{
			--g__SuppressLog;
		}
	};

	struct TimedScope
	{
		TimedScope()
		{
			m_StartTime = std::chrono::high_resolution_clock::now();
		}

		virtual ~TimedScope()
		{
			const std::chrono::high_resolution_clock::time_point rightNow = std::chrono::high_resolution_clock::now();
			const std::chrono::duration<double> timeInSeconds = rightNow - m_StartTime;
			if (SuppressLogScope::g__SuppressLog == 0)
			{
				printf(" OK -> %.2f ms.\r\n", timeInSeconds.count() * 1000.f);
			}
		}

	protected:
		std::chrono::high_resolution_clock::time_point m_StartTime;
	};
	
	struct LogScopedVar : public TimedScope
	{
		LogScopedVar(const std::string& message)
			: LogScopedVar(message, true)
		{
		}

	protected:
		LogScopedVar(const std::string& message, bool useNewLine)
			: TimedScope()
			, m_Message(message)
		{
			if (SuppressLogScope::g__SuppressLog == 0)
			{
				printf("%s...\r%s", message.c_str(), useNewLine ? "\n" : "");
			}
		}

		std::string m_Message;
	};

	struct LogProgressTracker : LogScopedVar
	{
		LogProgressTracker(const std::string& message, uint32_t fullProgressValue)
			: LogScopedVar(message, false)
			, m_CurrentProgressValue(0)
			, m_PercentageValue(0)
			, m_FullProgressValue(fullProgressValue)
		{
		}

		void UpdateProgress(uint32_t addValue, float addPercentage = -1.0f)
		{
			m_Mutex.lock();
			m_CurrentProgressValue += addValue;
			if (addPercentage == -1.0f)
			{
				addPercentage = addValue * 1.0f / m_FullProgressValue;
			}
			m_PercentageValue += addPercentage;
			if (SuppressLogScope::g__SuppressLog == 0)
			{
				printf("\r\r%s... %u/%u (%.0f%%)", m_Message.c_str(), m_CurrentProgressValue, m_FullProgressValue, m_PercentageValue * 100.0f);
			}
			m_Mutex.unlock();
		}

		~LogProgressTracker()
		{
			if (SuppressLogScope::g__SuppressLog == 0)
			{
				printf("\n");
			}
		}

	private:
		uint32_t m_FullProgressValue;
		uint32_t m_CurrentProgressValue;
		float m_PercentageValue;
		std::mutex m_Mutex;
	};

	inline __declspec(noreturn) void ThrowError(const char* formatString, ...)
	{
		printf("\r\nFatal error: ");
		va_list argList;
		va_start(argList, formatString);
		vprintf(formatString, argList);
		va_end(argList);
		printf("\r\n");
		exit(-1);
	}

	inline void LogInfo(const char* formatString, ...)
	{
		if (SuppressLogScope::g__SuppressLog == 0)
		{
			va_list argList;
			va_start(argList, formatString);
			vprintf(formatString, argList);
			va_end(argList);
			printf("\r\n");
		}
	}
}