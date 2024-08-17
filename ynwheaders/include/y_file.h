#pragma once

#ifdef _WIN32
#include <Windows.h>
#endif

#include <string>

namespace ynw
{
#ifdef _WIN32
	class SimpleWinFile
	{
	public:
		SimpleWinFile(const char* path)
			: m_Path(path)
		{
		}

		bool Resize(uint64_t newSize)
		{
			Unmap();
			LARGE_INTEGER newSizeL;
			newSizeL.QuadPart = static_cast<LONGLONG>(newSize);
			if (SetFilePointerEx(m_Handle, newSizeL, NULL, FILE_BEGIN) && SetEndOfFile(m_Handle))
			{
				newSizeL.QuadPart = 0;
				if (SetFilePointerEx(m_Handle, newSizeL, NULL, FILE_BEGIN))
				{
					m_Size = newSize;
					return Map();
				}
			}
			return false;	// if we fail, there's a chance the data etc is unmapped. rip.
		}

		bool Open(bool forWrite, bool overwriteExisting = true)
		{
			m_Handle = CreateFileA(m_Path.c_str(),
				forWrite ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
				forWrite ? FILE_SHARE_WRITE : FILE_SHARE_READ,
				nullptr,
				forWrite ? (overwriteExisting ? CREATE_ALWAYS : CREATE_NEW) : OPEN_EXISTING,
				forWrite ? 0 : FILE_ATTRIBUTE_READONLY,
				nullptr);

			m_IsWritable = forWrite;
			if (m_Handle != INVALID_HANDLE_VALUE)
			{
				if (m_IsWritable)
				{
					return true;
				}
				else
				{
					return Map();
				}
			}
			return false;
		}

		bool Map()
		{
			Unmap();

			m_FileMapping = CreateFileMappingW(m_Handle, nullptr, m_IsWritable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
			if (m_FileMapping == nullptr)
			{
				return false;
			}

			m_ViewOfFile = MapViewOfFile(m_FileMapping, m_IsWritable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ, 0, 0, 0);

			if (m_ViewOfFile == nullptr)
			{
				CloseHandle(m_FileMapping);
				m_FileMapping = INVALID_HANDLE_VALUE;
				return false;
			}

			LARGE_INTEGER fileSize;
			if (!GetFileSizeEx(m_Handle, &fileSize))
			{
				Unmap();
				return false;
			}

			m_Size = static_cast<uint64_t>(fileSize.QuadPart);

			return true;
		}

		void Unmap()
		{
			if (m_ViewOfFile != INVALID_HANDLE_VALUE)
			{
				UnmapViewOfFile(m_ViewOfFile);
				m_ViewOfFile = INVALID_HANDLE_VALUE;
			}

			if (m_FileMapping != INVALID_HANDLE_VALUE)
			{
				CloseHandle(m_FileMapping);
				m_FileMapping = INVALID_HANDLE_VALUE;
			}

		}

		void* GetData() const { return m_ViewOfFile; }
		uint64_t GetSize() const { return m_Size; }

		~SimpleWinFile()
		{
			Unmap();
			CloseHandle(m_Handle);
		}

	private:
		std::string m_Path;
		bool m_IsWritable = false;
		HANDLE m_Handle = INVALID_HANDLE_VALUE;
		HANDLE m_FileMapping = INVALID_HANDLE_VALUE;
		HANDLE m_ViewOfFile = INVALID_HANDLE_VALUE;
		uint64_t m_Size = 0;
	};
#endif
}