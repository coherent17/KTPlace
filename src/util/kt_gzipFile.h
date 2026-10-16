// @file kt_gzipFile.h
// An input stream over a file that may be gzip-compressed

#pragma once

#include <array>
#include <istream>
#include <streambuf>
#include <string>

namespace ktplace {

class GzipFileBuf : public std::streambuf {
public:
    explicit GzipFileBuf(const std::string &path);
    ~GzipFileBuf() override;

    GzipFileBuf(const GzipFileBuf &) = delete;
    GzipFileBuf &operator=(const GzipFileBuf &) = delete;

    [[nodiscard]] bool isOpen() const noexcept {
        return file != nullptr;
    }

protected:
    int_type underflow() override;

private:
    void *file = nullptr;
    std::array<char, 1 << 16> buffer{};
};

class GzipInputFile : public std::istream {
public:
    explicit GzipInputFile(const std::string &path);

private:
    GzipFileBuf buf;
};

}  // namespace ktplace
