#include "aligned_client_buffer.hpp"

#include <glog/logging.h>
#include <sys/mman.h>
#include <cstdlib>
#include <cstring>

#include "utils.h"

namespace mooncake {

std::shared_ptr<AlignedClientBufferAllocator>
AlignedClientBufferAllocator::create(size_t size, const std::string& protocol,
                                     bool use_hugepage) {
    if (size == 0) {
        LOG(ERROR)
            << "AlignedClientBufferAllocator: size must be greater than 0";
        return nullptr;
    }

    // Align size up to kDirectIOAlignment
    size_t aligned_size = align_up(size, kDirectIOAlignment);

    void* aligned_buffer = nullptr;

    if (use_hugepage) {
        // Use hugepage allocation (already aligned)
        aligned_buffer =
            allocate_buffer_mmap_memory(aligned_size, kDirectIOAlignment);
        if (!aligned_buffer) {
            LOG(ERROR) << "AlignedClientBufferAllocator: failed to allocate "
                       << "hugepage memory of size " << aligned_size;
            return nullptr;
        }
    } else {
        // Use mmap(MAP_SHARED | MAP_ANONYMOUS) instead of posix_memalign.
        // Kernel 5.15 pin_user_pages(FOLL_LONGTERM) rejects long-term
        // pinning of private anonymous pages (MAP_PRIVATE), which causes
        // io_uring_register_buffers to fail with EFAULT.  MAP_SHARED
        // anonymous mappings are backed by shmem and are allowed for
        // long-term pinning.
        aligned_buffer = mmap(nullptr, aligned_size,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS,
                              -1, 0);
        if (aligned_buffer == MAP_FAILED) {
            LOG(ERROR) << "AlignedClientBufferAllocator: mmap failed "
                       << "errno=" << errno << " (" << strerror(errno) << ")";
            aligned_buffer = nullptr;
            return nullptr;
        }
        // mmap returns zeroed memory, no memset needed.
    }

    // Verify alignment
    if (reinterpret_cast<uintptr_t>(aligned_buffer) % kDirectIOAlignment != 0) {
        LOG(ERROR) << "AlignedClientBufferAllocator: allocated buffer is not "
                   << "aligned to " << kDirectIOAlignment << " bytes";
        if (use_hugepage) {
            free_buffer_mmap_memory(aligned_buffer, aligned_size);
        } else {
            free(aligned_buffer);
        }
        return nullptr;
    }

    LOG(INFO) << "AlignedClientBufferAllocator: allocated " << aligned_size
              << " bytes at address " << aligned_buffer << " (aligned to "
              << kDirectIOAlignment << " bytes)";

    // Use custom deleter to properly free the aligned memory
    return std::shared_ptr<AlignedClientBufferAllocator>(
        new AlignedClientBufferAllocator(aligned_buffer, aligned_size, protocol,
                                         use_hugepage));
}

AlignedClientBufferAllocator::AlignedClientBufferAllocator(
    void* aligned_buffer, size_t size, const std::string& protocol,
    bool use_hugepage)
    : ClientBufferAllocator(aligned_buffer, size, protocol),
      owns_memory_(true),
      allocated_size_(size) {
    // Store hugepage flag in parent class's use_hugepage_ member
    // We need this for proper cleanup
    use_hugepage_ = use_hugepage;
}

AlignedClientBufferAllocator::~AlignedClientBufferAllocator() {
    // Free the aligned memory we allocated
    // The parent class destructor will not free it because is_external_memory_
    // is set to true
    if (owns_memory_ && buffer_) {
        if (use_hugepage_) {
            LOG(INFO)
                << "AlignedClientBufferAllocator: freeing hugepage memory "
                << "at " << buffer_ << " (" << allocated_size_ << " bytes)";
            free_buffer_mmap_memory(buffer_, allocated_size_);
        } else {
            LOG(INFO) << "AlignedClientBufferAllocator: freeing mmap memory "
                      << "at " << buffer_ << " (" << allocated_size_
                      << " bytes)";
            munmap(buffer_, allocated_size_);
        }
        buffer_ = nullptr;
    }
}

}  // namespace mooncake
