#pragma once
#ifndef _WIN32

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dutyon {

// Same-directory rename keeps the previous file intact on a failed write.
// Credentials are private from creation; fsync covers both data and rename.
inline bool atomicWritePrivate(const std::string& path, const std::string& data) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    std::filesystem::create_directories(parent, ec);
    if (ec) return false;
    std::string temp = path + ".tmp.XXXXXX";
    int fd = mkstemp(temp.data());
    if (fd < 0) return false;
    bool ok = fchmod(fd, 0600) == 0;
    size_t offset = 0;
    while (ok && offset < data.size()) {
        const auto n = write(fd, data.data() + offset, data.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        offset += static_cast<size_t>(n);
    }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok) ok = rename(temp.c_str(), path.c_str()) == 0;
    if (!ok) { unlink(temp.c_str()); return false; }
    int dir = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return false;
    ok = fsync(dir) == 0;
    close(dir);
    return ok;
}
} // namespace dutyon
#endif
