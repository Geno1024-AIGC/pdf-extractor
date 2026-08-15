#include "filters.h"

#include <cassert>
#include <cctype>
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

// ASCIIHexDecode: hex chars until '>', whitespace ignored.
bool asciiHexDecode(const char* src, size_t srcLen, std::string& out) {
    out.clear();
    unsigned nibble = 0;
    bool have = false;
    for (size_t i = 0; i < srcLen; ++i) {
        const char c = src[i];
        if (c == '>') break;
        unsigned v;
        if (c >= '0' && c <= '9') v = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') v = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = static_cast<unsigned>(c - 'A' + 10);
        else continue;  // whitespace / padding
        if (!have) {
            nibble = v;
            have = true;
        } else {
            out.push_back(static_cast<char>((nibble << 4) | v));
            have = false;
        }
    }
    if (have) out.push_back(static_cast<char>(nibble << 4));
    return true;
}

// ASCII85Decode (Adobe variant): 5 chars -> 4 bytes, 'z' -> 0000, ~> terminator.
bool ascii85Decode(const char* src, size_t srcLen, std::string& out) {
    out.clear();
    if (srcLen >= 2 && src[0] == '<' && src[1] == '~') {
        src += 2;  // optional Adobe "<~" marker
        srcLen -= 2;
    }
    unsigned group = 0;
    int count = 0;
    for (size_t i = 0; i < srcLen; ++i) {
        const unsigned char c = src[i];
        if (c == '~') break;  // "~>" terminator
        if (c == 'z' && count == 0) {
            out.append("\0\0\0\0", 4);
            continue;
        }
        if (c < '!' || c > 'u') continue;  // whitespace
        group = group * 85 + (c - '!');
        if (++count == 5) {
            const char b[4] = {static_cast<char>((group >> 24) & 0xff),
                               static_cast<char>((group >> 16) & 0xff),
                               static_cast<char>((group >> 8) & 0xff),
                               static_cast<char>(group & 0xff)};
            out.append(b, 4);
            group = 0;
            count = 0;
        }
    }
    if (count > 1) {
        // pad with 'u' (=84) up to 5 chars
        for (int i = count; i < 5; ++i) group = group * 85 + 84;
        const char b[4] = {static_cast<char>((group >> 24) & 0xff),
                           static_cast<char>((group >> 16) & 0xff),
                           static_cast<char>((group >> 8) & 0xff),
                           static_cast<char>(group & 0xff)};
        out.append(b, count - 1);
    }
    return true;
}

// RunLengthDecode: length byte; <128 = literal run, >128 = repeated byte, 128 = EOD.
bool runLengthDecode(const char* src, size_t srcLen, std::string& out) {
    out.clear();
    size_t i = 0;
    while (i < srcLen) {
        const unsigned char len = static_cast<unsigned char>(src[i++]);
        if (len == 128) break;
        if (len < 128) {
            const size_t n = static_cast<size_t>(len) + 1;
            if (i + n > srcLen) return false;
            out.append(src + i, n);
            i += n;
        } else {
            if (i >= srcLen) return false;
            out.append(static_cast<size_t>(257 - len), src[i++]);
        }
    }
    return true;
}

}  // namespace

bool decodeFilter(const std::string& name,
                  const char* src, size_t srcLen,
                  std::string& out) {
    if (name == "FlateDecode" || name == "Fl") return flateDecode(src, srcLen, out);
    if (name == "ASCIIHexDecode" || name == "AHx") return asciiHexDecode(src, srcLen, out);
    if (name == "ASCII85Decode" || name == "A85") return ascii85Decode(src, srcLen, out);
    if (name == "RunLengthDecode" || name == "RL") return runLengthDecode(src, srcLen, out);
    // Passthrough filters keep the bytes unchanged (JPEG/JPX/… data).
    if (name == "DCTDecode" || name == "JPXDecode" || name == "JBIG2Decode" ||
        name == "CCITTFaxDecode") {
        out.assign(src, srcLen);
        return true;
    }
    return false;  // unknown / unsupported filter
}

}  // namespace pdfx