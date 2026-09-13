#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "types.h"

namespace mooncake {

class PyClient;

class GDSReader {
 public:
    GDSReader(const std::string& root, int device, size_t max_io_bytes);
    ~GDSReader();
    std::vector<int> Read(const std::shared_ptr<PyClient>& store,
                          const std::vector<std::string>& keys,
                          const std::vector<uintptr_t>& destinations,
                          const std::vector<size_t>& sizes);
    std::map<std::string, uint64_t> Stats() const;
    bool BeginIOWindow(uint64_t window_id, int64_t start_ns);
    bool EndIOWindow(uint64_t window_id, int64_t end_ns, bool abort = false);
    std::string IOWindow() const;
    static int64_t IOClockNs();

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mooncake
