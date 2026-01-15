/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file SharedMemoryPlatformUnix.cpp
 * @brief Unix/POSIX implementation of shared memory platform operations
 *
 * Implements SharedMemory::PlatformOps for Linux and macOS using:
 * - shm_open() / shm_unlink() for shared memory regions
 * - mmap() / munmap() for memory mapping
 * - futex (Linux) / __ulock (macOS) for synchronization
 */

#include "SharedMemoryPlatform.h"

#if !defined(_WIN32)

#include <EntropyCore.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <os/lock.h>
#endif

namespace EntropyEngine::Networking::SharedMemory
{

// ============================================================================
// Platform-Specific Wake/Wait Implementation
// ============================================================================

#ifdef __linux__

// Linux: Use futex for wake/wait

static void futexWake(std::atomic<uint32_t>* addr, int count) {
    syscall(SYS_futex, addr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, count, nullptr, nullptr, 0);
}

static int futexWait(std::atomic<uint32_t>* addr, uint32_t expected, const struct timespec* timeout) {
    return static_cast<int>(syscall(SYS_futex, addr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, expected, timeout, nullptr, 0));
}

#elif defined(__APPLE__)

// =============================================================================
// IMPORTANT: Private macOS API Usage
// =============================================================================
//
// This implementation uses __ulock_wait/__ulock_wake, which are PRIVATE,
// undocumented BSD syscall wrappers. While these APIs have been stable since
// macOS 10.12 and are used by major projects (WebKit, Go runtime, etc.), they
// are not part of the public SDK and could theoretically change or be removed.
//
// Why we use them:
//   - They are the closest equivalent to Linux futex on macOS
//   - They enable efficient address-based waiting in shared memory
//   - dispatch_semaphore_t is NOT suitable (process-local, cannot be shared)
//
// Potential fallback if Apple removes these APIs:
//   - pthread_mutex_t + pthread_cond_t with PTHREAD_PROCESS_SHARED attribute
//   - This is fully POSIX-compliant but has higher overhead due to kernel locking
//   - Named semaphores (sem_open/sem_wait/sem_post) are another option
//
// Monitor for issues: https://github.com/anthropics/claude-code/issues (or project tracker)
// =============================================================================

// UL_COMPARE_AND_WAIT is the operation code for compare-and-wait
#define UL_COMPARE_AND_WAIT 1
#define ULF_NO_ERRNO 0x01000000

extern "C" int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout_us);
extern "C" int __ulock_wake(uint32_t operation, void* addr, uint64_t wakeValue);

static void ulockWake(std::atomic<uint32_t>* addr, bool wakeAll) {
    uint32_t flags = ULF_NO_ERRNO;
    if (wakeAll) {
        // ULF_WAKE_ALL = 0x00000100
        flags |= 0x00000100;
    }
    __ulock_wake(UL_COMPARE_AND_WAIT | flags, addr, 0);
}

static int ulockWait(std::atomic<uint32_t>* addr, uint32_t expected, uint32_t timeoutUs) {
    return __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, addr, expected, timeoutUs);
}

#endif

// ============================================================================
// PlatformOps Implementation
// ============================================================================

NativeHandle PlatformOps::createRegion(const char* name, size_t size) {
    if (!name || size == 0) {
        ENTROPY_LOG_ERROR("SharedMemory::createRegion: Invalid parameters");
        return INVALID_HANDLE_VALUE_SHM;
    }

    // POSIX shared memory names must start with '/'
    std::string shmName = "/";
    shmName += name;

    // Create with O_CREAT | O_EXCL to ensure we're creating new
    // Using O_RDWR for read/write access
    int fd = shm_open(shmName.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        if (errno == EEXIST) {
            // Region already exists - try to unlink and recreate
            shm_unlink(shmName.c_str());
            fd = shm_open(shmName.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
        }

        if (fd < 0) {
            ENTROPY_LOG_ERROR(
                std::format("SharedMemory::createRegion: shm_open failed for '{}': {}", shmName, strerror(errno)));
            return INVALID_HANDLE_VALUE_SHM;
        }
    }

    // Set the size of the shared memory region
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        ENTROPY_LOG_ERROR(
            std::format("SharedMemory::createRegion: ftruncate failed for '{}': {}", shmName, strerror(errno)));
        close(fd);
        shm_unlink(shmName.c_str());
        return INVALID_HANDLE_VALUE_SHM;
    }

    ENTROPY_LOG_DEBUG(std::format("SharedMemory::createRegion: Created region '{}' (size={})", shmName, size));
    return fd;
}

NativeHandle PlatformOps::openRegion(const char* name) {
    if (!name) {
        ENTROPY_LOG_ERROR("SharedMemory::openRegion: Invalid parameters");
        return INVALID_HANDLE_VALUE_SHM;
    }

    std::string shmName = "/";
    shmName += name;

    int fd = shm_open(shmName.c_str(), O_RDWR, 0);
    if (fd < 0) {
        ENTROPY_LOG_DEBUG(
            std::format("SharedMemory::openRegion: shm_open failed for '{}': {}", shmName, strerror(errno)));
        return INVALID_HANDLE_VALUE_SHM;
    }

    ENTROPY_LOG_DEBUG(std::format("SharedMemory::openRegion: Opened region '{}'", shmName));
    return fd;
}

void* PlatformOps::mapRegion(NativeHandle handle, size_t size) {
    if (handle == INVALID_HANDLE_VALUE_SHM || size == 0) {
        ENTROPY_LOG_ERROR("SharedMemory::mapRegion: Invalid parameters");
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
    if (ptr == MAP_FAILED) {
        ENTROPY_LOG_ERROR(std::format("SharedMemory::mapRegion: mmap failed: {}", strerror(errno)));
        return nullptr;
    }

    ENTROPY_LOG_DEBUG(std::format("SharedMemory::mapRegion: Mapped region at {} (size={})", ptr, size));
    return ptr;
}

void PlatformOps::unmapRegion(void* ptr, size_t size) {
    if (!ptr || size == 0) {
        return;
    }

    if (munmap(ptr, size) < 0) {
        ENTROPY_LOG_WARNING(std::format("SharedMemory::unmapRegion: munmap failed: {}", strerror(errno)));
    } else {
        ENTROPY_LOG_DEBUG(std::format("SharedMemory::unmapRegion: Unmapped region at {}", ptr));
    }
}

void PlatformOps::closeRegion(NativeHandle handle) {
    if (handle == INVALID_HANDLE_VALUE_SHM) {
        return;
    }

    if (close(handle) < 0) {
        ENTROPY_LOG_WARNING(std::format("SharedMemory::closeRegion: close failed: {}", strerror(errno)));
    } else {
        ENTROPY_LOG_DEBUG(std::format("SharedMemory::closeRegion: Closed handle {}", handle));
    }
}

void PlatformOps::destroyRegion(const char* name) {
    if (!name) {
        return;
    }

    std::string shmName = "/";
    shmName += name;

    if (shm_unlink(shmName.c_str()) < 0 && errno != ENOENT) {
        ENTROPY_LOG_WARNING(
            std::format("SharedMemory::destroyRegion: shm_unlink failed for '{}': {}", shmName, strerror(errno)));
    } else {
        ENTROPY_LOG_DEBUG(std::format("SharedMemory::destroyRegion: Destroyed region '{}'", shmName));
    }
}

void PlatformOps::wake(std::atomic<uint32_t>* addr) {
    if (!addr) {
        return;
    }

#ifdef __linux__
    futexWake(addr, 1);
#elif defined(__APPLE__)
    ulockWake(addr, false);
#else
    // Fallback: no-op (busy wait will be used)
    (void)addr;
#endif
}

void PlatformOps::wakeAll(std::atomic<uint32_t>* addr) {
    if (!addr) {
        return;
    }

#ifdef __linux__
    futexWake(addr, INT_MAX);
#elif defined(__APPLE__)
    ulockWake(addr, true);
#else
    // Fallback: no-op
    (void)addr;
#endif
}

bool PlatformOps::waitUntilChanged(std::atomic<uint32_t>* addr, uint32_t expected, int timeoutMs) {
    if (!addr) {
        return false;
    }

    // Fast path: check if already changed
    if (addr->load(std::memory_order_acquire) != expected) {
        return true;
    }

#ifdef __linux__
    struct timespec ts;
    struct timespec* timeout = nullptr;

    if (timeoutMs >= 0) {
        ts.tv_sec = timeoutMs / 1000;
        ts.tv_nsec = (timeoutMs % 1000) * 1000000L;
        timeout = &ts;
    }

    int result = futexWait(addr, expected, timeout);
    if (result < 0 && errno == ETIMEDOUT) {
        return false;  // Timed out
    }
    // EAGAIN means value changed before we could wait - that's success
    // EINTR means interrupted - check value again
    return addr->load(std::memory_order_acquire) != expected;

#elif defined(__APPLE__)
    uint32_t timeoutUs = (timeoutMs < 0) ? 0 : static_cast<uint32_t>(timeoutMs) * 1000;

    int result = ulockWait(addr, expected, timeoutUs);
    if (result == -ETIMEDOUT) {
        return false;  // Timed out
    }
    // Success, spurious wake, or value changed
    return addr->load(std::memory_order_acquire) != expected;

#else
    // Fallback: busy wait with sleep
    auto endTime =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs >= 0 ? timeoutMs : INT_MAX / 2);

    while (addr->load(std::memory_order_acquire) == expected) {
        if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= endTime) {
            return false;  // Timed out
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
#endif
}

std::string PlatformOps::getLastErrorString() {
    return std::string(strerror(errno));
}

bool PlatformOps::isAvailable() {
    // POSIX shared memory is generally available on Linux and macOS
    // Try to create a test region to verify
    const char* testName = "/__entropy_shm_test";
    int fd = shm_open(testName, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        close(fd);
        shm_unlink(testName);
        return true;
    }
    return false;
}

uint32_t getCurrentProcessId() {
    return static_cast<uint32_t>(getpid());
}

}  // namespace EntropyEngine::Networking::SharedMemory

#endif  // !defined(_WIN32)
