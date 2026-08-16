#include "cff.h"

#include <cstdint>
#include <string>

namespace pdfx {

namespace {

// A reader over the CFF binary layout with bounds checking. `mtx` records the
// start offset of the current INDEX so its per-entry object offsets (which are
// relative to the INDEX start) can be resolved later.
class R {
public:
    explicit R(const std::string& d) : d_(d), pos_(0) {}
    bool atEnd() const { return pos_ >= d_.size(); }
    size_t pos() const { return pos_; }
    void seek(size_t p) { pos_ = p; }
    bool avail(size_t n) const {
        return n <= d_.size() && pos_ <= d_.size() - n;
    }
    bool u8(uint8_t& v) {
        if (!avail(1)) return false;
        v = static_cast<uint8_t>(d_[pos_++]);
        return true;
    }
    bool u16(uint16_t& v) {
        if (!avail(2)) return false;
        v = static_cast<uint16_t>((static_cast<uint8_t>(d_[pos_]) << 8) |
                                  static_cast<uint8_t>(d_[pos_ + 1]));
        pos_ += 2;
        return true;
    }
    bool u32(uint32_t& v) {
        if (!avail(4)) return false;
        v = 0;
        for (int i = 0; i < 4; ++i)
            v = (v << 8) | static_cast<uint8_t>(d_[pos_ + i]);
        pos_ += 4;
        return true;
    }
    const char* data() const { return d_.data(); }

private:
    const std::string& d_;
    size_t pos_;
};

// The 1-based offsets in an INDEX are relative to the INDEX's own start.
struct Index {
    size_t start = 0;          // byte position of the count field
    std::vector<std::pair<size_t, size_t>> items;  // {offset, length}
    size_t end = 0;            // first byte after the INDEX
};

// Parse an INDEX at the current position. Leaves the reader right after it.
// CFF INDEX offsets are 1-based and measured relative to the byte before the
// data region: element i begins at (dataStart + offs[i] - 1), where dataStart
// is the first byte after the offset array.
bool readIndex(R& r, Index& out) {
    out.start = r.pos();
    uint16_t count = 0;
    if (!r.u16(count)) return false;
    if (count == 0) {
        out.end = r.pos();
        return true;
    }
    uint8_t offSize = 0;
    if (!r.u8(offSize)) return false;
    if (offSize < 1 || offSize > 4) return false;
    std::vector<uint32_t> offs;
    offs.reserve(static_cast<size_t>(count) + 1);
    for (size_t i = 0; i <= static_cast<size_t>(count); ++i) {
        uint32_t v = 0;
        for (uint8_t b = 0; b < offSize; ++b) {
            uint8_t byte = 0;
            if (!r.u8(byte)) return false;
            v = (v << 8) | byte;
        }
        offs.push_back(v);
    }
    if (offs.empty()) return false;
    if (offs[0] != 1) return false;  // first offset is always 1
    const size_t dataStart = r.pos();  // first byte of the data region
    out.items.reserve(static_cast<size_t>(count));
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
        const size_t begin = dataStart + offs[i] - 1;
        const size_t length = offs[i + 1] - offs[i];
        out.items.emplace_back(begin, length);
    }
    const size_t dataEnd = dataStart + offs.back() - 1;
    r.seek(dataEnd);
    out.end = dataEnd;
    return true;
}

// Parse an INDEX and collect each item as a raw byte range.
bool readIndexItems(const std::string& data, size_t start, Index& out) {
    R r(data);
    r.seek(start);
    return readIndex(r, out);
}

// Top DICT operators we care about: 17 = CharStrings (relative offset),
// 15 = charset (relative offset). Bytes 0..21 are operators; `12 n` is a two
// byte operator; everything else pushes an operand.
struct TopDict {
    long long charStrings = -1;  // offset relative to the Top DICT INDEX start
    long long charset = 0;
};

bool parseTopDict(const std::string& data, const Index& index, TopDict& out) {
    if (index.items.empty()) return false;
    const size_t begin = index.items[0].first;
    const size_t end = begin + index.items[0].second;
    size_t p = begin;
    std::vector<int64_t> operands;
    while (p < end) {
        const uint8_t b = static_cast<uint8_t>(data[p]);
        if (b <= 21) {
            int op = b;
            ++p;
            if (b == 12 && p < end) {
                op = 1200 + static_cast<uint8_t>(data[p]);
                ++p;
            }
            if (op == 17 && !operands.empty())
                out.charStrings = operands.back();
            else if (op == 15 && !operands.empty())
                out.charset = operands.back();
            operands.clear();
            continue;
        }
        if (b == 28) {
            if (p + 2 > end) break;
            const int16_t v = static_cast<int16_t>((data[p + 1] << 8) |
                                                   static_cast<uint8_t>(data[p + 2]));
            operands.push_back(v);
            p += 3;
            continue;
        }
        if (b == 29) {
            if (p + 4 > end) break;
            const int32_t v = (static_cast<uint8_t>(data[p + 1]) << 24) |
                              (static_cast<uint8_t>(data[p + 2]) << 16) |
                              (static_cast<uint8_t>(data[p + 3]) << 8) |
                              static_cast<uint8_t>(data[p + 4]);
            operands.push_back(v);
            p += 5;
            continue;
        }
        if (b == 30) {
            // Real number operand — skip its nibble stream.
            ++p;
            bool done = false;
            while (p < end && !done) {
                const uint8_t n = static_cast<uint8_t>(data[p]);
                if ((n & 0x0f) == 0x0f) done = true;
                if ((n >> 4) == 0x0f) done = true;
                ++p;
            }
            if (!done) break;
            continue;
        }
        if (b >= 32 && b <= 246) {
            operands.push_back(static_cast<int64_t>(b) - 139);
            ++p;
            continue;
        }
        if (b >= 247 && b <= 250) {
            if (p + 1 >= end) break;
            const int64_t v = (static_cast<int64_t>(b) - 247) * 256 +
                              static_cast<uint8_t>(data[p + 1]) + 108;
            operands.push_back(v);
            p += 2;
            continue;
        }
        if (b >= 251 && b <= 254) {
            if (p + 1 >= end) break;
            const int64_t v = -(static_cast<int64_t>(b) - 251) * 256 -
                              static_cast<uint8_t>(data[p + 1]) - 108;
            operands.push_back(v);
            p += 2;
            continue;
        }
        // 22..27 or 255: reserved / reserved operand.
        ++p;
    }
    return out.charStrings >= 0;
}

}  // namespace

bool parseCff(const std::string& data, CffInfo& out) {
    out = CffInfo{};
    if (data.size() < 4) return false;
    // Header: major, minor, hdrSize, offSize.
    const uint8_t major = static_cast<uint8_t>(data[0]);
    const uint8_t minor = static_cast<uint8_t>(data[1]);
    if (major != 1) return false;
    const uint8_t hdrSize = static_cast<uint8_t>(data[2]);
    const uint8_t offSize = static_cast<uint8_t>(data[3]);
    (void)minor;
    if (hdrSize < 4) return false;
    (void)offSize;

    R r(data);
    r.seek(hdrSize);

    // Name INDEX -> PostScript font name(s).
    Index nameIdx;
    if (!readIndex(r, nameIdx)) return false;
    for (const auto& it : nameIdx.items) {
        out.fontNames.push_back(data.substr(it.first, it.second));
    }

    // Top DICT INDEX -> find the CharStrings / charset offsets.
    Index topDictIdx;
    if (!readIndex(r, topDictIdx)) return false;
    // CharStrings / charset offsets from the Top DICT. Some producers emit
    // them absolute (relative to byte 0 of the font program); others relative
    // to the first byte of the Top DICT INDEX's data region. Try both.
    auto glyphsOf = [&](long long offset) -> int {
        if (offset < 0) return 0;
        Index cs;
        if (!readIndexItems(data, static_cast<size_t>(offset), cs)) return 0;
        return static_cast<int>(cs.items.size());
    };
    TopDict top;
    if (parseTopDict(data, topDictIdx, top)) {
        int g = glyphsOf(top.charStrings);
        if (g == 0) {
            const size_t topData =
                topDictIdx.items.empty() ? 0 : topDictIdx.items[0].first;
            g = glyphsOf(top.charStrings + static_cast<long long>(topData));
            if (g > 0) top.charStrings += static_cast<long long>(topData);
        }
        out.glyphCount = g;
        // Charset (if present) resolves glyph ids -> SID, which we could
        // later map to the String INDEX. Kept minimal.
        if (top.charset > 0 && top.charset < static_cast<long long>(data.size()))
            top.charset = 0;  // no charset-specific parsing yet
        (void)top;
    }

    // String INDEX -> all strings referenced by the font.
    Index strIdx;
    if (!readIndex(r, strIdx)) return false;
    for (const auto& it : strIdx.items) {
        out.strings.push_back(data.substr(it.first, it.second));
    }

    out.ok = !out.fontNames.empty();
    return out.ok;
}

}  // namespace pdfx
