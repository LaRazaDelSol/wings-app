// Wings cover-art module: small POSIX file helpers (internal).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same calls and fallbacks as wings/app/src/core/save_file.cpp, which already
// runs on the console: open/read/write/close, rename (retried after unlink for
// filesystems that will not replace), mkdir, stat, opendir/readdir.
#pragma once

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wings::art::fs
{

inline bool read_file(const std::string &path, std::string *out, std::size_t max_bytes)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    std::string buf;
    char chunk[16384];
    for (;;)
    {
        const ssize_t n = ::read(fd, chunk, sizeof chunk);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return false;
        }
        if (n == 0)
            break;
        buf.append(chunk, static_cast<std::size_t>(n));
        if (buf.size() > max_bytes)
        {
            ::close(fd);
            return false;
        }
    }
    ::close(fd);
    *out = std::move(buf);
    return true;
}

// Writes path via "path.tmp" then renames. Integrity does not depend on fsync:
// the formats written through here carry a length and a checksum.
inline bool write_atomic(const std::string &path, const char *a, std::size_t an, const char *b,
                         std::size_t bn)
{
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    auto put = [&](const char *p, std::size_t n)
    {
        while (n)
        {
            const ssize_t w = ::write(fd, p, n);
            if (w < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            p += w;
            n -= static_cast<std::size_t>(w);
        }
        return true;
    };
    const bool ok = put(a, an) && (bn == 0 || put(b, bn));
    if (::close(fd) != 0 || !ok)
    {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0)
    {
        ::unlink(path.c_str()); // some filesystems refuse to replace
        if (::rename(tmp.c_str(), path.c_str()) != 0)
        {
            ::unlink(tmp.c_str());
            return false;
        }
    }
    return true;
}

inline bool is_dir(const std::string &path)
{
    struct stat st
    {
    };
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool ensure_dir(const std::string &path)
{
    if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST)
        return is_dir(path);
    return false;
}

// mkdir -p
inline bool ensure_dirs(const std::string &path)
{
    if (path.empty())
        return false;
    std::string cur;
    for (std::size_t i = 0; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
        {
            if (!cur.empty() && cur != "/" && !is_dir(cur) && !ensure_dir(cur))
                return false;
        }
        if (i < path.size())
            cur.push_back(path[i]);
    }
    return is_dir(path);
}

struct DirEntry
{
    std::string name;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

// False when the directory cannot be opened.
inline bool list_dir(const std::string &dir, std::vector<DirEntry> *out)
{
    DIR *d = ::opendir(dir.c_str());
    if (!d)
        return false;
    while (const dirent *e = ::readdir(d))
    {
        const std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        DirEntry de;
        de.name = name;
        struct stat st
        {
        };
        if (::stat((dir + "/" + name).c_str(), &st) == 0)
        {
            de.size = static_cast<std::uint64_t>(st.st_size);
            de.mtime = static_cast<std::int64_t>(st.st_mtime);
        }
        out->push_back(std::move(de));
    }
    ::closedir(d);
    return true;
}

inline bool file_size(const std::string &path, std::uint64_t *size)
{
    struct stat st
    {
    };
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    *size = static_cast<std::uint64_t>(st.st_size);
    return true;
}

} // namespace wings::art::fs
