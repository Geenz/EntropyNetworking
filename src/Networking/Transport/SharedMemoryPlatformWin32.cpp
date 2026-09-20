/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file SharedMemoryPlatformWin32.cpp
 * @brief Windows implementation of shared memory platform operations
 *
 * Implements SharedMemory::PlatformOps for Windows using:
 * - CreateFileMapping() / OpenFileMapping() for shared memory regions
 * - MapViewOfFile() / UnmapViewOfFile() for memory mapping
 * - WaitOnAddress() / WakeByAddressSingle() for synchronization (Windows 8+)
 */

#include "SharedMemoryPlatform.h"

#ifdef _WIN32

#include <EntropyCore.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <synchapi.h>

#include <format>
#include <string>

namespace EntropyEngine::Networking::SharedMemory
{

// ============================================================================
// Helper Functions
// ============================================================================

static std::string getWin32ErrorString(DWORD errorCode) {
    if (errorCode == 0) {
        return "No error";
    }

    LPSTR messageBuffer = nullptr;
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&messageBuffer), 0, nullptr);

    std::string message(messageBuffer, size);
    LocalFree(messageBuffer);

    // Remove trailing newline
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }

    return message;
}

// Convert region name to Windows format (Local\\ namespace)
static std::wstring toWindowsName(const char* name) {
    std::wstring wname = L"Local\\";
    while (*name) {
        wname += static_cast<wchar_t>(*name);
        ++name;
    }
    return wname;
}

// ============================================================================
// PlatformOps Implementation
// ============================================================================

NativeHandle PlatformOps::createRegion(const char* name, size_t size) {
    if (!name || size == 0) {
        ENTROPY_LOG_ERROR("SharedMemory::createRegion: Invalid parameters");
        return INVALID_HANDLE_VALUE_SHM;
    }

    std::wstring wname = toWindowsName(name);

    // Use INVALID_HANDLE_VALUE to create backed by paging file (not a regular file)
    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE,                           // Use paging file
                                       nullptr,                                        // Default security
                                       PAGE_READWRITE,                                 // Read/write access
                                       static_cast<DWORD>((size >> 32) & 0xFFFFFFFF),  // High-order size
                                       static_cast<DWORD>(size & 0xFFFFFFFF),          // Low-order size
                                       wname.c_str());                                 // Object name

    if (handle == nullptr) {
        DWORD error = GetLastError();
        ENTROPY_LOG_ERROR(std::format("SharedMemory::createRegion: CreateFileMapping failed for '{}': {} ({})", name,
                                      getWin32ErrorString(error), error));
        return INVALID_HANDLE_VALUE_SHM;
    }

    // Check if region already existed
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ENTROPY_LOG_DEBUG(std::format("SharedMemory::createRegion: Region '{}' already exists, opened existing", name));
    }

    ENTROPY_LOG_DEBUG(std::format("SharedMemory::createRegion: Created region '{}' (size={})", name, size));
    return handle;
}

NativeHandle PlatformOps::openRegion(const char* name) {
    if (!name) {
        ENTROPY_LOG_ERROR("SharedMemory::openRegion: Invalid parameters");
        return INVALID_HANDLE_VALUE_SHM;
    }

    std::wstring wname = toWindowsName(name);

    HANDLE handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS,  // Read/write access
                                     FALSE,                // Don't inherit handle
                                     wname.c_str());       // Object name

    if (handle == nullptr) {
        DWORD error = GetLastError();
        ENTROPY_LOG_DEBUG(std::format("SharedMemory::openRegion: OpenFileMapping failed for '{}': {} ({})", name,
                                      getWin32ErrorString(error), error));
        return INVALID_HANDLE_VALUE_SHM;
    }

    ENTROPY_LOG_DEBUG(std::format("SharedMemory::openRegion: Opened region '{}'", name));
    return handle;
}

void* PlatformOps::mapRegion(NativeHandle handle, size_t size) {
    if (handle == INVALID_HANDLE_VALUE_SHM || size == 0) {
        ENTROPY_LOG_ERROR("SharedMemory::mapRegion: Invalid parameters");
        return nullptr;
    }

    void* ptr = MapViewOfFile(handle,               // Handle to mapping object
                              FILE_MAP_ALL_ACCESS,  // Read/write access
                              0,                    // High-order offset
                              0,                    // Low-order offset
                              size);                // Number of bytes to map

    if (ptr == nullptr) {
        DWORD error = GetLastError();
        ENTROPY_LOG_ERROR(
            std::format("SharedMemory::mapRegion: MapViewOfFile failed: {} ({})", getWin32ErrorString(error), error));
        return nullptr;
    }

    ENTROPY_LOG_DEBUG(std::format("SharedMemory::mapRegion: Mapped region at {} (size={})", ptr, size));
    return ptr;
}

void PlatformOps::unmapRegion(void* ptr, size_t size) {
    (void)size;  // Not used on Windows

    if (!ptr) {
        return;
    }

    if (!UnmapViewOfFile(ptr)) {
        DWORD error = GetLastError();
        ENTROPY_LOG_WARNING(std::format("SharedMemory::unmapRegion: UnmapViewOfFile failed: {} ({})",
                                        getWin32ErrorString(error), error));
    } else {
        ENTROPY_LOG_DEBUG(std::format("SharedMemory::unmapRegion: Unmapped region at {}", ptr));
    }
}

void PlatformOps::closeRegion(NativeHandle handle) {
    if (handle == INVALID_HANDLE_VALUE_SHM || handle == nullptr) {
        return;
    }

    if (!CloseHandle(handle)) {
        DWORD error = GetLastError();
        ENTROPY_LOG_WARNING(
            std::format("SharedMemory::closeRegion: CloseHandle failed: {} ({})", getWin32ErrorString(error), error));
    } else {
        ENTROPY_LOG_DEBUG("SharedMemory::closeRegion: Closed handle");
    }
}

void PlatformOps::destroyRegion(const char* name) {
    // On Windows, named file mappings are reference counted and automatically
    // destroyed when all handles are closed. There's no explicit "unlink"
    // like POSIX shm_unlink. The region persists until all processes close
    // their handles.
    (void)name;
    ENTROPY_LOG_DEBUG(
        std::format("SharedMemory::destroyRegion: Region '{}' will be destroyed when all handles close", name));
}

void PlatformOps::wake(std::atomic<uint32_t>* addr) {
    if (!addr) {
        return;
    }

    // WakeByAddressSingle wakes one thread waiting on this address
    WakeByAddressSingle(addr);
}

void PlatformOps::wakeAll(std::atomic<uint32_t>* addr) {
    if (!addr) {
        return;
    }

    // WakeByAddressAll wakes all threads waiting on this address
    WakeByAddressAll(addr);
}

bool PlatformOps::waitUntilChanged(std::atomic<uint32_t>* addr, uint32_t expected, int timeoutMs) {
    if (!addr) {
        return false;
    }

    // Fast path: check if already changed
    if (addr->load(std::memory_order_acquire) != expected) {
        return true;
    }

    DWORD timeout = (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);

    // WaitOnAddress compares the value at addr with expected and waits if equal
    // Returns when value changes or timeout expires
    BOOL result = WaitOnAddress(addr, &expected, sizeof(uint32_t), timeout);

    if (!result) {
        DWORD error = GetLastError();
        if (error == ERROR_TIMEOUT) {
            return false;  // Timed out
        }
        // Other errors - just check if value changed
    }

    return addr->load(std::memory_order_acquire) != expected;
}

std::string PlatformOps::getLastErrorString() {
    return getWin32ErrorString(GetLastError());
}

bool PlatformOps::isAvailable() {
    // WaitOnAddress requires Windows 8+ (we require this for modern sync primitives)
    // CreateFileMapping is available on all Windows versions

    // Try to create a test region to verify
    std::wstring testName = L"Local\\__entropy_shm_test";
    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, testName.c_str());

    if (handle != nullptr) {
        CloseHandle(handle);
        return true;
    }

    return false;
}

uint32_t getCurrentProcessId() {
    return static_cast<uint32_t>(GetCurrentProcessId());
}

}  // namespace EntropyEngine::Networking::SharedMemory

#endif  // _WIN32
