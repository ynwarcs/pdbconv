#pragma once

#include <bit>
#include "y_log.h"

namespace ynw
{
	inline bool& IsTestMode()
	{
		static bool g_IsTestMode = false;
		return g_IsTestMode;
	}

	template <typename DstType, typename SrcType>
	inline constexpr bool FitsInto(SrcType someNumeric)
	{
		return static_cast<SrcType>(static_cast<DstType>(someNumeric)) == someNumeric;
	}

	template <typename DstType, typename SrcType>
	inline constexpr DstType StrictCastTo(SrcType someNumeric)
	{
		if (FitsInto<DstType>(someNumeric))
		{
			return static_cast<DstType>(someNumeric);
		}

		ThrowError("Range check failure. Value %llu doesn't fit into %u bits.", someNumeric, sizeof(DstType) * 8);
	}

	template <typename Type>
	inline constexpr Type AlignTo(Type someNumeric, Type alignment)
	{
		return ((someNumeric + (alignment - 1)) / alignment) * alignment;
	}

	template <typename SrcType, typename AlignType>
	inline constexpr SrcType AlignTo(SrcType someNumeric, AlignType alignment)
	{
		return AlignTo(someNumeric, StrictCastTo<SrcType>(alignment));
	}

	template <typename Type>
	inline bool IsPowerOf2(Type someNumeric)
	{
		return std::popcount(someNumeric) <= 1;
	}
}