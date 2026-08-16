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
    long long charStrings = -1;   // offset of the CharStrings INDEX
    long long charset = 0;        // offset of the charset (0 = Identity)
    long long privateSize = 0;    // Private DICT size (op 18, operand 0)
    long long privateOffset = -1; // Private DICT offset (op 18, operand 1)
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
            else if (op == 18 && operands.size() >= 2) {
                // operand[0] = size, operand[1] = offset.
                out.privateSize = operands[operands.size() - 2];
                out.privateOffset = operands.back();
            }
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

namespace {
// Type 2 charstring operator names (CFF spec, 20/5180).
const char* kType2Names[] = {
    "", "hstem", "", "vstem", "vmoveto", "rlineto", "hlineto", "vlineto",
    "rrcurveto", "", "callsubr", "return", "", "", "endchar", "", "", "",
    "hstemhm", "hintmask", "cntrmask", "rmoveto", "hmoveto", "vstemhm",
    "rcurveline", "rlinecurve", "vvcurveto", "hhcurveto", "", "",
    "vhcurveto", "hvcurveto",
};
// Escaped operator names (12 <n>).
const char* kEscapeNames[] = {
    "dotsection", "vstem3", "hstem3", "seac", "sbw", "div", "callothersubr",
    "pop", "setcurrentpoint", "", "", "hflex", "flex", "hflex1", "flex1",
};

void readOperand(const std::string& data, size_t& p, size_t end,
                 std::string& txt) {
    while (p < end) {
        const uint8_t b = static_cast<uint8_t>(data[p]);
        if (32 <= b && b <= 246) {  // -107 .. 107
            txt += std::to_string(static_cast<int>(b) - 139);
            ++p;
            return;
        }
        if (247 <= b && b <= 250) {  // positive wide
            if (p + 1 >= end) return;
            const int v = (static_cast<int>(b) - 247) * 256 +
                          static_cast<uint8_t>(data[p + 1]) + 108;
            txt += std::to_string(v);
            p += 2;
            return;
        }
        if (251 <= b && b <= 254) {  // negative wide
            if (p + 1 >= end) return;
            const int v = -(static_cast<int>(b) - 251) * 256 -
                          static_cast<uint8_t>(data[p + 1]) - 108;
            txt += std::to_string(v);
            p += 2;
            return;
        }
        if (b == 28) {  // 16-bit integer
            if (p + 2 >= end) return;
            const int v = static_cast<int16_t>((data[p + 1] << 8) |
                                               static_cast<uint8_t>(data[p + 2]));
            txt += std::to_string(v);
            p += 3;
            return;
        }
        if (b == 255) {  // 32-bit fixed (16.16); show as integer numerator
            if (p + 4 >= end) return;
            const int32_t v = (static_cast<uint8_t>(data[p + 1]) << 24) |
                              (static_cast<uint8_t>(data[p + 2]) << 16) |
                              (static_cast<uint8_t>(data[p + 3]) << 8) |
                              static_cast<uint8_t>(data[p + 4]);
            txt += std::to_string(v);
            p += 5;
            return;
        }
        break;  // not an operand start
    }
}

// CFF charset: maps a glyph id to a SID (string ID). SID 0 is .notdef. The
// table starts with a format byte (0/1/2), then SID runs for glyph 1..n.
// format 0: one u16 SID per glyph; format 1: (first u16, nLeft u8) runs;
// format 2: (first u16, nLeft u16) runs. On failure the mapping stays unset.
void parseCharset(const std::string& data, long long offset,
                  std::vector<int>& gids) {
    const size_t n = gids.size();
    if (n == 0) return;
    const size_t p0 = static_cast<size_t>(offset);
    if (p0 >= data.size()) return;
    size_t p = p0;
    const uint8_t fmt = static_cast<uint8_t>(data[p++]);
    size_t g = 1;
    if (fmt == 0) {
        while (g < n) {
            if (p + 2 > data.size()) return;
            gids[g] = (static_cast<uint8_t>(data[p]) << 8) |
                      static_cast<uint8_t>(data[p + 1]);
            p += 2;
            ++g;
        }
    } else if (fmt == 1 || fmt == 2) {
        const int nLeftBytes = (fmt == 1) ? 1 : 2;
        while (g < n) {
            if (p + 2 + nLeftBytes > data.size()) return;
            const int first = (static_cast<uint8_t>(data[p]) << 8) |
                              static_cast<uint8_t>(data[p + 1]);
            int nLeft;
            if (fmt == 1) {
                nLeft = static_cast<uint8_t>(data[p + 2]);
            } else {
                nLeft = (static_cast<uint8_t>(data[p + 2]) << 8) |
                        static_cast<uint8_t>(data[p + 3]);
            }
            p += 2 + nLeftBytes;
            int sid = first;
            for (int k = 0; k <= nLeft && g < n; ++k, ++g) {
                gids[g] = sid++;
            }
        }
    }
}

// Disassemble one Type 2 charstring program (a glyph in the CharStrings
// INDEX) into a human-readable operator listing, e.g.
//   hstem(722 -22 31 665) vstem(56 103) rmoveto(665 233) endchar()
// Subroutines are not expanded; calls appear as callsubr(N). Stems are
// tracked so hint/cntr mask bytes are skipped correctly.
void disassembleCharstring(const std::string& data, size_t begin, size_t end,
                           std::string& out) {
    size_t p = begin;
    int stems = 0;
    std::vector<std::string> operands;
    auto readOps = [&]() {
        while (p < end) {
            const uint8_t c = static_cast<uint8_t>(data[p]);
            if (c == 28 || c == 255 || (32 <= c && c <= 254)) {
                std::string num;
                readOperand(data, p, end, num);
                operands.push_back(num);
            } else {
                break;
            }
        }
    };
    auto join = [&]() {
        std::string s;
        for (size_t i = 0; i < operands.size(); ++i) {
            if (i) s += " ";
            s += operands[i];
        }
        operands.clear();
        return s;
    };
    bool first = true;
    while (p < end) {
        // readOps() advances p past any leading integer operands, so the
        // operator is whatever byte follows them.
        readOps();
        if (p >= end) break;
        const uint8_t b = static_cast<uint8_t>(data[p]);
        if (!first) out += " ";
        first = false;
        if (b == 12) {  // escaped operator
            if (p + 1 >= end) break;
            const uint8_t e = data[p + 1];
            const char* nm =
                e < sizeof(kEscapeNames) / sizeof(*kEscapeNames)
                    ? kEscapeNames[e]
                    : "";
            out += std::string(nm && *nm ? nm : "esc" + std::to_string(e)) +
                   "(" + join() + ")";
            if (e == 1 || e == 2) stems += 3;  // hstem3 / vstem3
            p += 2;
            continue;
        }
        if (b == 19 || b == 20) {  // hintmask / cntrmask, mask bytes follow
            out += std::string(b == 19 ? "hintmask" : "cntrmask") + "(" +
                   join() + ")";
            const int maskBytes = (stems + 7) / 8;
            p += 1 + static_cast<size_t>(maskBytes < 0 ? 0 : maskBytes);
            continue;
        }
        if (b < sizeof(kType2Names) / sizeof(*kType2Names) &&
            kType2Names[b][0]) {
            // hstem/vstem/hstemhm/vstemhm: each pair of operands is one stem.
            if (b == 1 || b == 3 || b == 18 || b == 23)
                stems += static_cast<int>(operands.size()) / 2;
            out += std::string(kType2Names[b]) + "(" + join() + ")";
            ++p;
            continue;
        }
        out += "op" + std::to_string(b) + "(" + join() + ")";
        ++p;
    }
    if (!operands.empty()) {  // trailing width/operands before endchar etc.
        out += " [" + join() + "]";
    }
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
    long long baseAdj = 0;
    TopDict top;
    if (parseTopDict(data, topDictIdx, top)) {
        if (top.charStrings >= 0) {
            Index cs;
            if (!readIndexItems(data, static_cast<size_t>(top.charStrings), cs)) {
                const size_t topData =
                    topDictIdx.items.empty() ? 0 : topDictIdx.items[0].first;
                if (readIndexItems(data, static_cast<size_t>(top.charStrings) +
                                             topData,
                                   cs)) {
                    baseAdj = static_cast<long long>(topData);
                }
            }
        }
        // Apply the resolved base to every Top DICT offset we use.
        if (baseAdj) {
            top.charStrings += baseAdj;
            top.charset += baseAdj;
            if (top.privateOffset >= 0) top.privateOffset += baseAdj;
        }
        (void)top;
    }

    // String INDEX -> all strings referenced by the font.
    Index strIdx;
    if (!readIndex(r, strIdx)) return false;
    for (const auto& it : strIdx.items) {
        out.strings.push_back(data.substr(it.first, it.second));
    }

    // CharStrings INDEX -> one program per glyph; disassemble each one.
    Index csIdx;
    if (top.charStrings >= 0 &&
        readIndexItems(data, static_cast<size_t>(top.charStrings), csIdx)) {
        out.glyphCount = static_cast<int>(csIdx.items.size());
        // Charset maps glyph ids -> SIDs; SIDs index the String INDEX
        // (SID 0 = .notdef, 1..390 standard, 391+ = our strings).
        std::vector<int> gids(out.glyphCount, 0);
        if (top.charset > 0) parseCharset(data, top.charset, gids);
        out.glyphNames.resize(static_cast<size_t>(out.glyphCount));
        out.glyphPrograms.reserve(static_cast<size_t>(out.glyphCount));
        for (size_t i = 0; i < csIdx.items.size(); ++i) {
            const auto& it = csIdx.items[i];
            const int sid = gids[i];
            std::string name;
            if (sid == 0)
                name = ".notdef";
            else if (sid >= 391 &&
                     static_cast<size_t>(sid - 391) < out.strings.size())
                name = out.strings[static_cast<size_t>(sid - 391)];
            else
                name = "sid" + std::to_string(sid);
            out.glyphNames[i] = name;
            std::string prog;
            disassembleCharstring(data, it.first, it.first + it.second, prog);
            out.glyphPrograms.push_back(prog);
        }
    }

    out.ok = !out.fontNames.empty();
    return out.ok;
}

}  // namespace pdfx
