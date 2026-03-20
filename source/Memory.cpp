/*	MCMX file compressor

  Copyright (C) 2013, Google Inc.
  Authors: Mathieu Chartier

  LICENSE

    This file is part of the MCMX file compressor.

    MCMX is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MCMX is distributed in the hope that it will be useful,
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

#define USE_MALLOC 1

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
  // 64-byte alignment ensures hash table entries are cache-line aligned,
  // which makes prefetch addresses precise and avoids cross-line splits.
#ifdef _WIN32
  storage = _aligned_malloc(bytes, 64);
  if (storage)
    std::memset(storage, 0, bytes);
#else
  storage = std::aligned_alloc(64, bytes);
  if (storage)
    std::memset(storage, 0, bytes);
#endif
#elif WIN32
  storage = (void *)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
#error UNIMPLEMENTED
#endif
}

void MemMap::release()
{
  if (storage != nullptr)
  {
#if USE_MALLOC
#ifdef _WIN32
    _aligned_free(storage);
#else
    std::free(storage);
#endif
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