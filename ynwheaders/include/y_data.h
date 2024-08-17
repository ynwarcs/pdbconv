#pragma once

#include <span>
#include <vector>
#include <cassert>

namespace ynw
{
	class MutableStreamFixed
	{
	public:
		MutableStreamFixed(void* data, uint64_t dataSize)
			: m_Data(reinterpret_cast<uint8_t*>(data))
			, m_Size(dataSize)
		{
		}

		template <typename T>
		bool Write(const T& value)
		{
			return WriteBytes(&value, sizeof(T));
		}

		template <typename T>
		bool WriteSpan(const std::span<const T>& bytesVec)
		{
			return WriteBytes(bytesVec.data(), bytesVec.size() * sizeof(T));
		}

		bool Seek(size_t where)
		{
			if (where > m_Size)
			{
				return false;
			}
			m_Offset = where;
			return true;
		}

		virtual bool WriteBytes(const void* data, size_t dataLengthInBytes)
		{
			if (m_Offset + dataLengthInBytes > m_Size)
			{
				assert(false);
				return false;
			}
			memcpy(m_Data + m_Offset, data, dataLengthInBytes);
			m_Offset += dataLengthInBytes;
			return true;
		}

		MutableStreamFixed GetStreamAtOffset(uint64_t offset, uint64_t size = 0) const
		{
			if (size == 0)
			{
				size = m_Size - offset;
			}

			if (offset > m_Size || offset + size > m_Size)
			{
				return { nullptr, 0 };
			}

			return MutableStreamFixed(GetData() + offset, size);
		}

		virtual void Reset()
		{
			m_Offset = 0;
		}

		uint8_t* GetData() const { return m_Data; }
		uint64_t GetSize() const { return m_Size; }
		uint64_t GetOffset() const { return m_Offset; }

	protected:
		uint8_t* m_Data = nullptr;
		uint64_t m_Size = 0;
		uint64_t m_Offset = 0;
	};

	class MutableStreamDynamic : public MutableStreamFixed
	{
	public:
		MutableStreamDynamic()
			: MutableStreamFixed(nullptr, 0)
		{
		}

		bool WriteBytes(const void* data, size_t dataLengthInBytes) override
		{
			if (m_Offset + dataLengthInBytes > m_OwnedData.size())
			{
				m_OwnedData.resize(m_OwnedData.size() + dataLengthInBytes);
				UpdateData();
			}
			return MutableStreamFixed::WriteBytes(data, dataLengthInBytes);
		}

		void MoveTo(std::vector<uint8_t>& outData)
		{
			outData = std::move(m_OwnedData);
		}

		void Reserve(size_t howManyBytes)
		{
			m_OwnedData.reserve(howManyBytes);
			UpdateData();
		}

		void Reset() override
		{
			MutableStreamFixed::Reset();
			m_OwnedData.clear();
			UpdateData();
		}

		void UpdateData()
		{
			m_Data = m_OwnedData.data();
			m_Size = m_OwnedData.size();
		}

	protected:
		std::vector<uint8_t> m_OwnedData;
	};

	class SimpleMutableStreamFixedThreadSafe : public MutableStreamFixed
	{
	public:
		using MutableStreamFixed::MutableStreamFixed;

		SimpleMutableStreamFixedThreadSafe(const MutableStreamFixed& srcStream)
			: MutableStreamFixed(srcStream)
		{
		}

		MutableStreamFixed GetRegionSubstreamForWriting(const uint64_t regionSize, uint64_t& outRegionOffset)
		{
			m_Mutex.lock();
			if (m_Offset + regionSize > m_Size)
			{
				m_Mutex.unlock();
				return MutableStreamFixed{ nullptr, 0 };
			}

			outRegionOffset = m_Offset;
			m_Offset += regionSize;

			MutableStreamFixed subStream(m_Data + outRegionOffset, regionSize);
			m_Mutex.unlock();
			return subStream;
		}

	private:
		std::mutex m_Mutex;
	};

	class MutableStreamDynamicThreadSafe : public MutableStreamDynamic
	{
	public:
		using MutableStreamDynamic::MutableStreamDynamic;

		MutableStreamFixed GetRegionSubstreamForWriting(const uint64_t regionSize, uint64_t& outRegionOffset)
		{
			m_Mutex.lock();
			if (m_Offset + regionSize > m_Size)
			{
				m_OwnedData.resize(StrictCastTo<size_t>(m_Size - (m_Offset + regionSize)));
				UpdateData();
			}

			outRegionOffset = m_Offset;
			m_Offset += regionSize;

			MutableStreamFixed subStream(m_OwnedData.data() + outRegionOffset, regionSize);
			m_Mutex.unlock();
			return subStream;
		}

	private:
		std::mutex m_Mutex;
	};

	class ImmutableStream
	{
	public:
		ImmutableStream(const void* data, uint64_t numBytes)
			: m_Data(static_cast<const uint8_t*>(data))
			, m_Length(numBytes)
			, m_Offset(0)
		{
		}

		bool CanRead() const { return m_Offset < m_Length; }
		bool CanRead(uint64_t howManyBytes) const { return m_Offset + howManyBytes <= m_Length; }
		bool CanRead(uint64_t fromOffset, uint64_t howManyBytes) const { return fromOffset < m_Length && fromOffset + howManyBytes <= m_Length; }

		ImmutableStream GetStreamAtOffset(uint64_t offset, uint64_t size = 0) const
		{
			if (size == 0 && CanRead(offset, 0))
			{
				return ImmutableStream(m_Data + offset, m_Length - offset);
			}
			else if (size != 0 && CanRead(offset, size))
			{
				return ImmutableStream(m_Data + offset, size);
			}
			else
			{
				return ImmutableStream(nullptr, 0);
			}
		}

		template <typename T>
		bool CopyRead(T& outValue)
		{
			return ReadData(&outValue, sizeof(T));
		}

		bool Seek(uint64_t where)
		{
			if (where > m_Length)
			{
				return false;
			}
			m_Offset = where;
			return true;
		}

		template <typename T>
		const T* Read()
		{
			const T* returnValue = Peek<T>();
			if (returnValue)
			{
				m_Offset += sizeof(T);
			}
			return returnValue;
		}

		template <typename T>
		const T* Peek() const
		{
			if (m_Offset + sizeof(T) > m_Length)
			{
				return nullptr;
			}
			return reinterpret_cast<const T*>(m_Data + m_Offset);
		}

		template <typename T>
		const T* PeekAtOffset(size_t where) const
		{
			if (where > m_Length || where + sizeof(T) > m_Length)
			{
				return nullptr;
			}
			return reinterpret_cast<const T*>(m_Data + where);
		}

		bool ReadData(void* outBytes, size_t numBytes = 0)
		{
			if (numBytes == 0)
			{
				numBytes = StrictCastTo<size_t>(m_Length - m_Offset);
			}

			if (m_Offset + numBytes > m_Length)
			{
				return false;
			}
			memcpy(outBytes, m_Data + m_Offset, numBytes);
			m_Offset += numBytes;
			return true;
		}

	private:
		const uint8_t* m_Data;
		uint64_t m_Length;
		uint64_t m_Offset;
	};
}