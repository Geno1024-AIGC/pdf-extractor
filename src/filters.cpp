#include "filters.h"

#include <cassert>
#include <vector>

#include <zlib.h>

namespace pdfx {

namespace {

bool flateDecode(const char* src, size_t srcLen, std::string& out) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;

    out.clear();
    std::vector<char> buf(64 * 1024);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(src));
    zs.avail_in = static_cast<uInt>(srcLen);
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf.data());
        zs.avail_out = static_cast<uInt>(buf.size());
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        out.append(buf.data(), buf.size() - zs.avail_out);
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return true;
}

}  // namespace

bool decodeFilter(const std::string& name,
                  const char* src, size_t srcLen,
                  std::string& out) {
    if (name == "FlateDecode" || name == "Fl") return flateDecode(src, srcLen, out);
    // TODO: ASCIIHexDecode, ASCII85Decode, RunLengthDecode, LZWDecode
    return false;
}

}  // namespace pdfx