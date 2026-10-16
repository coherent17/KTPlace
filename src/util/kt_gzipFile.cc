// @file kt_gzipFile.cc
// An input stream over a file that may be gzip-compressed

#include "util/kt_gzipFile.h"

#include <zlib.h>

namespace ktplace {

GzipFileBuf::GzipFileBuf(const std::string &path) : file(gzopen(path.c_str(), "rb")) {
    if (file != nullptr) {
        gzbuffer(static_cast<gzFile>(file), 1 << 17);
    }
}

GzipFileBuf::~GzipFileBuf() {
    if (file != nullptr) {
        gzclose(static_cast<gzFile>(file));
    }
}

GzipFileBuf::int_type GzipFileBuf::underflow() {
    if (gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }
    if (file == nullptr) {
        return traits_type::eof();
    }
    const int got =
        gzread(static_cast<gzFile>(file), buffer.data(), static_cast<unsigned>(buffer.size()));
    if (got <= 0) {
        return traits_type::eof();
    }
    setg(buffer.data(), buffer.data(), buffer.data() + got);
    return traits_type::to_int_type(*gptr());
}

GzipInputFile::GzipInputFile(const std::string &path) : std::istream(nullptr), buf(path) {
    rdbuf(&buf);
    if (!buf.isOpen()) {
        setstate(std::ios::failbit);
    }
}

}  // namespace ktplace
