#pragma once

#include <span>
#include <thread>
#include <atomic>
#include <algorithm>
#include <vector>
#include <functional>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace ynw
{
	struct ThreadConfig
	{
		static void SetDefaultNumThreads(uint32_t numThreads) { g_DefaultNumThreads = numThreads; }
		static uint32_t GetDefaultNumThreads()
		{
			if (g_DefaultNumThreads != 0)
			{
				return g_DefaultNumThreads;
			}
			else
			{
				constexpr float k_ThreadUsageRatio = 0.75;	// use 3/4 of available cores
#ifdef _WIN32
				SYSTEM_INFO systemInfo = {};
				GetSystemInfo(&systemInfo);
				return static_cast<uint32_t>(systemInfo.dwNumberOfProcessors * k_ThreadUsageRatio);
#else
				static_assert(!"Default thread detection not implemented for non-windows platforms.");
#endif
			}
		}

	private:
		static inline uint32_t g_DefaultNumThreads = 0;
	};

	template <typename ElementType>
	struct ParallelForRunner
	{
	public:
		using ActionFnSig = std::function<void(const ElementType& element, uint32_t elementIndex)>;
		using ScoreFnSig = std::function<uint32_t(const ElementType& element, uint32_t elementIndex)>;

		ParallelForRunner(const std::span<ElementType>& elements)
			: m_Elements(elements)
		{
			m_NumThreads = ThreadConfig::GetDefaultNumThreads();
		}

		void SetScoreFunction(ScoreFnSig&& scoreFn) { m_ScoreFunction = scoreFn; }
		void SetNumThreads(uint32_t numThreads) { m_NumThreads = numThreads; }

		void Execute(ActionFnSig&& actionFn)
		{
			std::vector<uint32_t> indexQueue;
			indexQueue.reserve(m_Elements.size());
			for (uint32_t i = 0; i < m_Elements.size(); ++i)
			{
				indexQueue.push_back(i);
			}

			if (m_ScoreFunction)
			{
				std::sort(indexQueue.begin(), indexQueue.end(), [this](const uint32_t lhs, const uint32_t rhs)
					{
						return m_ScoreFunction(m_Elements[lhs], lhs) > m_ScoreFunction(m_Elements[rhs], rhs);
					});
			}

			std::atomic<size_t> currentWorkingIndex;
			auto workerThreadFn = [&currentWorkingIndex, &indexQueue, &actionFn, this]()
				{
					while (true)
					{
						const size_t workingIndex = currentWorkingIndex++;
						if (workingIndex >= m_Elements.size())
						{
							return;
						}

						actionFn(m_Elements[indexQueue[workingIndex]], indexQueue[workingIndex]);
					}
				};

			std::vector<std::thread> workerThreads;
			workerThreads.reserve(m_NumThreads);
			for (uint32_t i = 0; i < m_NumThreads; ++i)
			{
				workerThreads.emplace_back(workerThreadFn);
			}
			for (uint32_t i = 0; i < m_NumThreads; ++i)
			{
				workerThreads[i].join();
			}
		}

	private:
		std::span<ElementType> m_Elements;
		ScoreFnSig m_ScoreFunction;
		uint32_t m_NumThreads;
	};
}