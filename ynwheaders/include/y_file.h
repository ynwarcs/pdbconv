#pragma once

#ifdef _WIN32
#include <windows.h>
#else
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <string>

namespace ynw
{
#ifdef _WIN32
	class SimpleFile
	{
	public:
		SimpleFile(const char* path)
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
				m_FileMapping = nullptr;
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
			if (m_ViewOfFile != nullptr)
			{
				UnmapViewOfFile(m_ViewOfFile);
				m_ViewOfFile = nullptr;
			}

			if (m_FileMapping != nullptr)
			{
				CloseHandle(m_FileMapping);
				m_FileMapping = nullptr;
			}

		}

		void* GetData() const { return m_ViewOfFile; }
		uint64_t GetSize() const { return m_Size; }

		~SimpleFile()
		{
			Unmap();
			if (m_Handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(m_Handle);
			}
		}

	private:
		std::string m_Path;
		bool m_IsWritable = false;
		HANDLE m_Handle = INVALID_HANDLE_VALUE;
		HANDLE m_FileMapping = NULL;
		LPVOID m_ViewOfFile = NULL;
		uint64_t m_Size = 0;
	};
#else
	class SimpleFile
	{
	public:
		SimpleFile(const char* path)
			: m_Path(path)
		{
		}

		bool Resize(uint64_t newSize)
		{
			Unmap();
			if (ftruncate(m_Fd, newSize) == 0)
			{
				m_Size = newSize;
				return Map();
			}
			return false;
		}

		bool Open(bool forWrite, bool overwriteExisting = true)
		{
			int flags = forWrite ? O_RDWR : O_RDONLY;
			if (forWrite)
			{
				flags |= O_CREAT;
				if (overwriteExisting)
				{
					flags |= O_TRUNC;
				}
				else
				{
					flags |= O_EXCL;
				}
			}

			m_Fd = open(m_Path.c_str(), flags, 0644);
			m_IsWritable = forWrite;

			if (m_Fd != -1)
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

			if (m_Size == 0)
			{
				struct stat st;
				if (fstat(m_Fd, &st) == 0)
				{
					m_Size = st.st_size;
				}
				else
				{
					return false;
				}
			}

			if (m_Size == 0)
			{
				return true;
			}

			m_ViewOfFile = mmap(nullptr, m_Size, m_IsWritable ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED, m_Fd, 0);

			if (m_ViewOfFile == MAP_FAILED)
			{
				m_ViewOfFile = nullptr;
				return false;
			}

			return true;
		}

		void Unmap()
		{
			if (m_ViewOfFile != nullptr)
			{
				munmap(m_ViewOfFile, m_Size);
				m_ViewOfFile = nullptr;
			}
		}

		void* GetData() const { return m_ViewOfFile; }
		uint64_t GetSize() const { return m_Size; }

		~SimpleFile()
		{
			Unmap();
			if (m_Fd != -1)
			{
				close(m_Fd);
			}
		}

	private:
		std::string m_Path;
		bool m_IsWritable = false;
		int m_Fd = -1;
		void* m_ViewOfFile = nullptr;
		uint64_t m_Size = 0;
	};
#endif
}
