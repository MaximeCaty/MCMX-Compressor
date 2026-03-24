/*	MCM file compressor

  Copyright (C) 2013, Google Inc.
  Authors: Mathieu Chartier

  LICENSE

    This file is part of the MCM file compressor.

    MCM is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MCM is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MCM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cstdlib>
#include <cstring>

#include "Memory.hpp"
#include "Util.hpp"

#define USE_MALLOC 0

#ifdef WIN32
#include <Windows.h>
#else
// TODO: mmap
#endif

MemMap::MemMap() : storage(nullptr), size(0)
{
}

MemMap::~MemMap()
{
  release();
}

void MemMap::resize(size_t bytes)
{
  if (bytes == size)
  {
    std::fill(reinterpret_cast<uint8_t *>(storage), reinterpret_cast<uint8_t *>(storage) + size, 0);
    return;
  }
  release();
  size = bytes;
#if USE_MALLOC
  storage = std::calloc(1, bytes);
#elif WIN32
  // Try large pages first (eliminates TLB misses for 256MB+ hash tables).
  // Requires SeLockMemoryPrivilege. Silently falls back to normal pages.
  storage = nullptr;
  {
    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    {
      TOKEN_PRIVILEGES tp = {};
      tp.PrivilegeCount = 1;
      tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      if (LookupPrivilegeValueA(nullptr, "SeLockMemoryPrivilege", &tp.Privileges[0].Luid))
      {
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        if (GetLastError() == ERROR_SUCCESS)
        {
          SIZE_T large_page_min = GetLargePageMinimum();
          if (large_page_min > 0)
          {
            // Round size up to multiple of large page size
            SIZE_T aligned_size = (size + large_page_min - 1) & ~(large_page_min - 1);
            storage = (void *)VirtualAlloc(nullptr, aligned_size,
                                           MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
          }
        }
      }
      CloseHandle(token);
    }
  }
  if (!storage)
  {
    storage = (void *)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  }
#else
#error UNIMPLEMENTED
#endif
}

void MemMap::release()
{
  if (storage != nullptr)
  {
#if USE_MALLOC
    std::free(storage);
#elif WIN32
    BOOL result = VirtualFree((LPVOID)storage, size, MEM_DECOMMIT);
#else
#error UNIMPLEMENTED
#endif
    storage = nullptr;
  }
}

void MemMap::zero()
{
#ifdef USE_MALLOC
  std::memset(storage, 0, size);
#elif WIN32
  storage = (void *)VirtualAlloc(storage, size, MEM_RESET, PAGE_READWRITE);
#else
  madvise(storage, size, MADV_DONTNEED);
#endif
}