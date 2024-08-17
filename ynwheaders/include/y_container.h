#pragma once

#include <vector>
#include <span>
#include <cassert>

namespace ynw
{
	template <typename T>
	class ReadOnlyVector
	{
	public:
		void AssignNonOwned(const std::span<const T>& nonOwnedVector)
		{
			m_OwnsData = false;
			m_NonOwnedData = nonOwnedVector;
		}

		void AssignOwned(std::vector<T>& ownedVector)
		{
			m_OwnsData = true;
			m_OwnedData = std::move(ownedVector);
		}

		operator std::span<const T>() const
		{
			return GetSpan();
		}

		std::span<const T> GetSpan() const
		{
			return { GetData(), GetSize() };
		}

		template <typename U>
		ReadOnlyVector<U> Cast() const
		{
			ReadOnlyVector<U> typedVector;
			if (GetSize() % sizeof(U) != 0)
			{
				return typedVector;
			}
			typedVector.AssignNonOwned(m_OwnsData ? m_OwnedData : m_NonOwnedData);
			return typedVector;
		}

		template <typename U>
		ReadOnlyVector<U> CastAndMove()
		{
			ReadOnlyVector<U> typedVector;
			if (GetSize() % sizeof(U) != 0)
			{
				return typedVector;
			}
			if (m_OwnsData)
			{
				typedVector.AssignNonOwned(m_NonOwnedData);
			}
			else
			{
				typedVector.AssignOwned(m_OwnedData);
				m_OwnsData = false;
			}
		}

		const T* GetData() const
		{
			if (m_OwnsData)
			{
				return m_OwnedData.data();
			}
			else
			{
				return m_NonOwnedData.data();
			}
		}

		size_t GetSize() const
		{
			if (m_OwnsData)
			{
				return m_OwnedData.size();
			}
			else
			{
				return m_NonOwnedData.size();
			}
		}


	private:
		bool m_OwnsData = true;
		std::vector<T> m_OwnedData;
		std::span<const T> m_NonOwnedData;
	};

	class DynamicBitset
	{
	public:
		DynamicBitset() = default;
		~DynamicBitset() = default;

		void Resize(uint32_t size)
		{
			m_Data.resize(AlignTo(size, 8u) / 8u);
			m_ModularSizeInBits = size % 8;
		}

		void SetAll()
		{
			memset(m_Data.data(), ~0, m_Data.size());
		}

		void UnsetAll()
		{
			memset(m_Data.data(), 0, m_Data.size());
		}

		bool Set(uint32_t offset)
		{
			if (offset > GetSize())
			{
				return false;
			}

			const uint32_t vecOffset = offset / 8;
			const uint32_t bitMask = (1 << (offset % 8));
			m_Data[vecOffset] |= bitMask;
			return true;
		}

		bool Unset(uint32_t offset)
		{
			if (offset > GetSize())
			{
				return false;
			}

			const uint32_t vecOffset = offset / 8;
			const uint8_t bitMask = ~(1u << (offset % 8));
			m_Data[vecOffset] &= bitMask;
			return true;
		}

		bool Test(uint32_t offset)
		{
			if (offset > GetSize())
			{
				return false;
			}
			const uint32_t vecOffset = offset / 8;
			const uint32_t bitMask = (1 < (offset % 8));
			return (m_Data[vecOffset] & bitMask);
		}

		const uint8_t* GetData() const { return m_Data.data(); }
		size_t GetSize() const { return m_Data.size() * 8 + m_ModularSizeInBits; }
		const std::span<const uint8_t> GetSpan() const { return m_Data; }
	private:
		std::vector<uint8_t> m_Data;
		uint8_t m_ModularSizeInBits = 0;
	};
}