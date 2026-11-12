// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mkwvk {
// File-backed, contiguous Vulkan cache bytes. Published files are replaced, never
// truncated. Linux reads use an unlinked owned snapshot: a writer replacing or
// truncating the original cannot invalidate the view supplied to the driver.
// Like other mapped files, media faults are not recoverable C++ exceptions.
class PipelineCacheFile {
    void* bytes_ = nullptr;
    size_t size_ = 0;
    bool rejected_ = false;
    std::string temporary_, destination_;
    static PipelineCacheFile rejected_file() {
        PipelineCacheFile result; result.rejected_ = true; return result;
    }
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
    std::wstring temporary_wide_;
    static std::wstring wide(const std::string& path) {
        if (path.empty() || path.size() > INT_MAX || path.find('\0') != std::string::npos) return {};
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), (int)path.size(), nullptr, 0);
        if (!n) return {};
        std::wstring result(n, L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), (int)path.size(), result.data(), n) != n) return {};
        return result;
    }
#else
    int file_ = -1;
#endif
    bool resize(size_t n) {
#if defined(_WIN32)
        LARGE_INTEGER length{}; length.QuadPart = (LONGLONG)n;
        return SetFilePointerEx(file_, length, nullptr, FILE_BEGIN) && SetEndOfFile(file_);
#else
        return ftruncate(file_, (off_t)n) == 0;
#endif
    }
    bool map(bool writable) {
#if defined(_WIN32)
        mapping_ = CreateFileMappingW(file_, nullptr, writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
        if (mapping_) bytes_ = MapViewOfFile(mapping_, writable ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, size_);
#else
        bytes_ = mmap(nullptr, size_, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, file_, 0);
        if (bytes_ == MAP_FAILED) bytes_ = nullptr;
#endif
        return bytes_ != nullptr;
    }
    bool unmap() {
        bool ok = true;
        if (bytes_) {
#if defined(_WIN32)
            ok = UnmapViewOfFile(bytes_) != 0;
#else
            ok = munmap(bytes_, size_) == 0;
#endif
            if (ok) bytes_ = nullptr;
        }
#if defined(_WIN32)
        if (!bytes_ && mapping_) {
            bool closed = CloseHandle(mapping_) != 0;
            if (closed) mapping_ = nullptr;
            ok = closed && ok;
        }
#endif
        return ok;
    }
    bool close() {
#if defined(_WIN32)
        if (file_ == INVALID_HANDLE_VALUE) return true;
        bool ok = CloseHandle(file_) != 0;
        if (ok) file_ = INVALID_HANDLE_VALUE;
        return ok;
#else
        if (file_ < 0) return true;
        int fd = file_; file_ = -1; // Do not retry close(EINTR) on a possibly reused fd.
        return ::close(fd) == 0;
#endif
    }
    bool create_temporary(const std::string& path) {
        destination_ = path;
#if defined(_WIN32)
        static std::atomic<unsigned long long> sequence{0};
        for (int attempt = 0; attempt < 32; ++attempt) {
            std::string candidate = path + ".tmp-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(sequence++);
            auto name = wide(candidate); if (name.empty()) return false;
            file_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file_ != INVALID_HANDLE_VALUE) {
                temporary_ = std::move(candidate); temporary_wide_ = std::move(name); return true;
            }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
        return false;
#else
        if (path.empty() || path.find('\0') != std::string::npos) return false;
        std::string name = path + ".tmp-XXXXXX";
        file_ = mkstemp(name.data());
        if (file_ < 0) return false;
        temporary_ = std::move(name);
        return fcntl(file_, F_SETFD, FD_CLOEXEC) == 0;
#endif
    }
public:
    PipelineCacheFile() = default;
    PipelineCacheFile(const PipelineCacheFile&) = delete;
    PipelineCacheFile& operator=(const PipelineCacheFile&) = delete;
    PipelineCacheFile(PipelineCacheFile&& other) noexcept { swap(other); }
    PipelineCacheFile& operator=(PipelineCacheFile&& other) noexcept {
        if (this != &other) { reset(); swap(other); } return *this;
    }
    ~PipelineCacheFile() { reset(); }
    void swap(PipelineCacheFile& other) noexcept {
        std::swap(bytes_, other.bytes_); std::swap(size_, other.size_);
        std::swap(rejected_, other.rejected_);
        temporary_.swap(other.temporary_); destination_.swap(other.destination_);
        std::swap(file_, other.file_);
#if defined(_WIN32)
        std::swap(mapping_, other.mapping_);
        temporary_wide_.swap(other.temporary_wide_);
#endif
    }
    bool reset() noexcept {
        if (!unmap()) {
            std::fputs("vk: pipeline cache view cleanup failed\n", stderr);
            return false;
        }
        const bool closed = close();
        if (!closed) {
            std::fputs("vk: pipeline cache file close failed\n", stderr);
#if defined(_WIN32)
            return false; // Retain ownership for a later retry; do not unlink an open file.
#endif
        }
        if (!temporary_.empty()) {
#if defined(_WIN32)
            if (!temporary_wide_.empty() && !DeleteFileW(temporary_wide_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
#else
            if (unlink(temporary_.c_str()) && errno != ENOENT) {
#endif
                std::fputs("vk: pipeline cache temporary cleanup failed\n", stderr);
                return false; // Keep the owned name so explicit reset can retry.
            }
        }
        temporary_.clear(); destination_.clear(); size_ = 0;
        rejected_ = false;
#if defined(_WIN32)
        temporary_wide_.clear();
#endif
        return closed;
    }
    const uint8_t* data() const { return static_cast<const uint8_t*>(bytes_); }
    uint8_t* data() { return static_cast<uint8_t*>(bytes_); }
    size_t size() const { return bytes_ ? size_ : 0; }
    bool empty() const { return !bytes_; }
    bool rejected() const { return rejected_; } // Header incompatible, not an acquisition/I/O failure.
    static PipelineCacheFile writable(const std::string& path, size_t capacity) {
        PipelineCacheFile result;
        if (!capacity || capacity > (uint64_t)INT64_MAX || !result.create_temporary(path)) return {};
        result.size_ = capacity;
        if (!result.resize(capacity) || !result.map(true)) return {};
        return result;
    }
    // Validate the small header BEFORE copying/mapping the full payload.
    template<class Valid> static PipelineCacheFile read(const std::string& path, Valid valid) {
        PipelineCacheFile result;
        uint8_t header[32]{};
        uint64_t length = 0;
#if defined(_WIN32)
        auto name = wide(path); if (name.empty()) return {};
        result.file_ = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (result.file_ == INVALID_HANDLE_VALUE) return {};
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER n{}; DWORD got = 0;
        if (GetFileType(result.file_) != FILE_TYPE_DISK || !GetFileInformationByHandle(result.file_, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(result.file_, &n) || n.QuadPart <= 32 ||
            !ReadFile(result.file_, header, sizeof header, &got, nullptr) || got != sizeof header) return {};
        length = (uint64_t)n.QuadPart;
        if (length > SIZE_MAX) return {};
        if (!valid(header, length)) return rejected_file();
        result.size_ = (size_t)length;
        if (!result.map(false)) return {};
#else
        if (path.empty() || path.find('\0') != std::string::npos) return {};
        int source = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (source < 0) return {};
        struct Source { int fd; ~Source() { ::close(fd); } } owned{source};
        struct stat st{};
        if (fstat(source, &st) || !S_ISREG(st.st_mode) || st.st_size <= 32 ||
            pread(source, header, sizeof header, 0) != sizeof header) return {};
        length = (uint64_t)st.st_size;
        if (length > SIZE_MAX) return {};
        if (!valid(header, length)) return rejected_file();
        if (!result.create_temporary(path)) return {};
        if (unlink(result.temporary_.c_str())) return {};
        result.temporary_.clear(); result.destination_.clear();
        uint8_t chunk[64 * 1024]; uint64_t offset = 0;
        while (offset < length) {
            size_t want = (size_t)std::min<uint64_t>(sizeof chunk, length - offset);
            ssize_t n = pread(source, chunk, want, (off_t)offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return {};
            size_t done = 0;
            while (done < (size_t)n) {
                ssize_t wrote = write(result.file_, chunk + done, (size_t)n - done);
                if (wrote < 0 && errno == EINTR) continue;
                if (wrote <= 0) return {};
                done += (size_t)wrote;
            }
            offset += (uint64_t)n;
        }
        result.size_ = (size_t)length;
        if (!result.map(false)) return {};
#endif
        return result;
    }
    bool publish(size_t actual) {
        if (!bytes_ || temporary_.empty() || !actual || actual > size_) return false;
#if defined(_WIN32)
        if (!FlushViewOfFile(bytes_, actual)) return false;
#else
        if (msync(bytes_, actual, MS_SYNC)) return false;
#endif
        if (!unmap() || !resize(actual)) return false;
#if defined(_WIN32)
        if (!FlushFileBuffers(file_) || !close()) return false;
        auto from = wide(temporary_), to = wide(destination_);
        if (from.empty() || to.empty() || !MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
#else
        if (fsync(file_) || !close() || rename(temporary_.c_str(), destination_.c_str())) return false;
#endif
        temporary_.clear(); destination_.clear(); size_ = 0;
        return true;
    }
};
} // namespace mkwvk
