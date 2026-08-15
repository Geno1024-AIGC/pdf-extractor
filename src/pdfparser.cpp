#include "filters.h"
#include "pdfparser.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pdfx {

namespace {

struct Reader {
    const std::string& s;
    size_t pos = 0;

    void skipWs() {
        while (pos < s.size()) {
            const char c = s[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
                c == char(0)) {
                ++pos;
            } else if (c == '%') {
                while (pos < s.size() && s[pos] != '\n' && s[pos] != '\r') ++pos;
            } else {
                break;
            }
        }
    }

    bool starts(const char* t) const {
        return s.compare(pos, std::strlen(t), t) == 0;
    }
};

std::string readName(Reader& r) {
    const size_t start = r.pos;
    while (r.pos < r.s.size()) {
        const char c = r.s[r.pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == char(0) || c == '/' || c == '(' || c == ')' || c == '<' ||
            c == '>' || c == '[' || c == ']' || c == '{' || c == '}') {
            break;
        }
        ++r.pos;
    }
    return r.s.substr(start, r.pos - start);
}

bool tryReadNumber(Reader& r, long long& out) {
    const char* p = r.s.c_str() + r.pos;
    char* endp = nullptr;
    errno = 0;
    const long long v = std::strtoll(p, &endp, 10);
    if (endp == p) return false;
    out = v;
    r.pos += static_cast<size_t>(endp - p);
    return true;
}

// Skip a single PDF value in a balanced way (dict / array / string / name / atom).
void skipValue(Reader& r) {
    r.skipWs();
    if (r.pos >= r.s.size()) return;
    const char c = r.s[r.pos];

    if (c == '<') {
        if (r.starts("<<")) {  // nested dict
            r.pos += 2;
            int depth = 1;
            while (r.pos < r.s.size() && depth > 0) {
                if (r.starts("<<")) {
                    ++depth;
                    r.pos += 2;
                } else if (r.starts(">>")) {
                    --depth;
                    r.pos += 2;
                } else {
                    ++r.pos;
                }
            }
        } else {  // hex string
            while (r.pos < r.s.size() && r.s[r.pos] != '>') ++r.pos;
            if (r.pos < r.s.size()) ++r.pos;
        }
    } else if (c == '[') {
        ++r.pos;
        int depth = 1;
        while (r.pos < r.s.size() && depth > 0) {
            if (r.s[r.pos] == '[') ++depth;
            else if (r.s[r.pos] == ']') {
                --depth;
                ++r.pos;
                continue;
            } else if (r.s[r.pos] == '<') {
                if (r.starts("<<")) {
                    skipValue(r);
                    continue;
                }
                while (r.pos < r.s.size() && r.s[r.pos] != '>') ++r.pos;
                if (r.pos < r.s.size()) ++r.pos;
            } else if (r.s[r.pos] == '(') {
                skipValue(r);
            } else {
                ++r.pos;
            }
        }
    } else if (c == '(') {
        ++r.pos;
        int depth = 1;
        while (r.pos < r.s.size() && depth > 0) {
            if (r.s[r.pos] == '\\') {
                r.pos += 2;
            } else if (r.s[r.pos] == '(') {
                ++depth;
                ++r.pos;
            } else if (r.s[r.pos] == ')') {
                --depth;
                ++r.pos;
            } else {
                ++r.pos;
            }
        }
    } else if (c == '/') {
        ++r.pos;
        readName(r);
    } else {
        bool isNum = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
        // read an atom token (number / keyword / name-body)
        while (r.pos < r.s.size()) {
            const char d = r.s[r.pos];
            if (d == ' ' || d == '\t' || d == '\r' || d == '\n' || d == '/' ||
                d == '[' || d == ']' || d == '<' || d == '>' || d == '(' ||
                d == ')' || d == char(0)) {
                break;
            }
            ++r.pos;
        }
        // indirect reference: "N G R" must be consumed as one value
        if (isNum) {
            Reader ref{r.s, r.pos};
            ref.skipWs();
            long long ignored;
            if (tryReadNumber(ref, ignored)) {
                ref.skipWs();
                if (ref.s.compare(ref.pos, 1, "R") == 0) {
                    r.pos = ref.pos + 1;
                }
            }
        }
    }
}

struct StreamMeta {
    long long length = -1;
    bool lengthIndirect = false;
    int lengthRef = -1;
    std::vector<std::string> filters;
    std::string type;
    std::string subtype;
    // image fields
    int width = 0;
    int height = 0;
    int bitsPerComponent = 8;
    int components = 1;
    int predictor = 1;
    std::string colorspace;
};

bool parseDict(Reader& r, StreamMeta* m) {
    const size_t save = r.pos;
    r.skipWs();
    if (!r.starts("<<")) {
        r.pos = save;
        return false;
    }
    r.pos += 2;
    while (true) {
        r.skipWs();
        if (r.pos >= r.s.size()) return false;
        if (r.starts(">>")) {
            r.pos += 2;
            return true;
        }
        if (r.s[r.pos] != '/') return false;
        ++r.pos;
        const std::string key = readName(r);
        r.skipWs();

        if (r.pos >= r.s.size()) return false;
        if (key == "Length") {
            long long first;
            if (tryReadNumber(r, first)) {
                r.skipWs();
                const size_t save2 = r.pos;
                long long gen;
                if (tryReadNumber(r, gen)) {
                    r.skipWs();
                    if (r.starts("R")) {
                        r.pos += 1;
                        m->length = -1;
                        m->lengthIndirect = true;
                        m->lengthRef = static_cast<int>(first);
                    } else {
                        r.pos = save2;
                        m->length = first;
                    }
                } else {
                    m->length = first;
                }
            }
        } else if (key == "Filter" || key == "F") {
            if (r.s[r.pos] == '/') {
                ++r.pos;
                m->filters.push_back(readName(r));
            } else if (r.s[r.pos] == '[') {
                Reader sub{r.s, r.pos};
                sub.pos += 1;
                sub.skipWs();
                while (sub.pos < r.s.size() && !sub.starts("]")) {
                    if (sub.s[sub.pos] == '/') {
                        ++sub.pos;
                        m->filters.push_back(readName(sub));
                    } else {
                        ++sub.pos;
                    }
                    sub.skipWs();
                }
                r.pos = sub.pos;
                if (r.starts("]")) ++r.pos;
            }
        } else if (key == "Type") {
            if (r.s[r.pos] == '/') {
                ++r.pos;
                m->type = readName(r);
            } else {
                skipValue(r);
            }
        } else if (key == "Subtype") {
            if (r.s[r.pos] == '/') {
                ++r.pos;
                m->subtype = readName(r);
            } else {
                skipValue(r);
            }
        } else if (key == "Width") {
            long long v;
            if (tryReadNumber(r, v)) m->width = static_cast<int>(v);
        } else if (key == "Height") {
            long long v;
            if (tryReadNumber(r, v)) m->height = static_cast<int>(v);
        } else if (key == "BitsPerComponent") {
            long long v;
            if (tryReadNumber(r, v)) m->bitsPerComponent = static_cast<int>(v);
        } else if (key == "ColorSpace") {
            if (r.s[r.pos] == '/') {
                ++r.pos;
                m->colorspace = readName(r);
                int n = 1;
                if (m->colorspace == "DeviceGray") n = 1;
                else if (m->colorspace == "DeviceRGB") n = 3;
                else if (m->colorspace == "DeviceCMYK") n = 4;
                m->components = n;
            } else {
                skipValue(r);
            }
        } else if (key == "DecodeParms") {
            // The value may be << dict >>, [ ... ], a name ref, or hex string.
            if (r.pos + 1 < r.s.size() && r.s[r.pos] == '<' &&
                r.s[r.pos + 1] == '<') {
                r.pos += 2;
                r.skipWs();
                while (r.pos < r.s.size() && !r.starts(">>")) {
                    r.skipWs();
                    if (r.s[r.pos] != '/') {
                        skipValue(r);
                        continue;
                    }
                    ++r.pos;
                    std::string k = readName(r);
                    if (k == "Predictor") {
                        long long v;
                        if (tryReadNumber(r, v))
                            m->predictor = static_cast<int>(v);
                    } else if (k == "Colors") {
                        long long v;
                        if (tryReadNumber(r, v) && v > 0)
                            m->components = static_cast<int>(v);
                    } else if (k == "Columns") {
                        long long v;
                        if (tryReadNumber(r, v) && v > 0)
                            m->width = static_cast<int>(v);
                    } else {
                        skipValue(r);
                    }
                    r.skipWs();
                }
                if (r.pos + 1 < r.s.size()) r.pos += 2;
            } else {
                skipValue(r);
            }
        } else {
            skipValue(r);
        }
    }
}

}  // namespace

bool PdfFile::load(const std::string& path) {
    return loadPath(path);
}

bool PdfFile::loadPath(const std::string& path) {
    // helper to read the whole file into data_
    auto readFile = [&](std::ifstream&& in) -> bool {
        if (!in) {
            error = "cannot open file";
            return false;
        }
        std::ostringstream oss;
        oss << in.rdbuf();
        data_ = oss.str();
        if (data_.size() < 8 || data_.compare(0, 5, "%PDF-") != 0) {
            error = "not a PDF file";
            return false;
        }
        return true;
    };

#ifdef _WIN32
    // Windows: open with wide chars so non-ASCII paths work.
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(len > 0 ? len : 1), L'\0');
    if (len <= 0) {
        error = "cannot open file";
        return false;
    }
    ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], len);
    wpath.resize(static_cast<size_t>(len - 1));
    if (!readFile(std::ifstream(wpath.c_str(), std::ios::binary))) return false;
#else
    if (!readFile(std::ifstream(path, std::ios::binary))) return false;
#endif

    const std::regex objRe(R"((\d+)\s+(\d+)\s+obj(?=\s))");
    const auto begin = std::sregex_iterator(data_.begin(), data_.end(), objRe);
    const auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        Object obj;
        obj.id = std::stoi((*it)[1].str());
        obj.gen = std::stoi((*it)[2].str());
        obj.offset = static_cast<int64_t>(it->position());

        Reader r{data_, static_cast<size_t>(obj.offset) + static_cast<size_t>(it->length())};
        r.skipWs();

        StreamMeta meta;
        const bool isDict = parseDict(r, &meta);
        bool hasStream = false;
        if (isDict) {
            r.skipWs();
            if (r.starts("stream")) {
                r.pos += 6;
                if (r.pos < data_.size() && data_[r.pos] == '\r') ++r.pos;
                if (r.pos < data_.size() && data_[r.pos] == '\n') ++r.pos;
                hasStream = true;
                obj.streamStart = static_cast<int64_t>(r.pos);
            }
        }

        if (hasStream) {
            obj.isStream = true;
            obj.type = "stream";
            obj.filters = meta.filters;
            obj.typeName = meta.type;
            obj.subtype = meta.subtype;
            obj.width = meta.width;
            obj.height = meta.height;
            obj.bitsPerComponent = meta.bitsPerComponent;
            obj.components = meta.components;
            obj.predictor = meta.predictor;
            obj.colorspace = meta.colorspace;
            if (meta.lengthIndirect) {
                obj.rawLength = -1;
                obj.lengthRef = meta.lengthRef;
            } else if (meta.length >= 0) {
                obj.rawLength = meta.length;
            } else {
                obj.rawLength = -1;
            }
            if (obj.rawLength < 0 && !meta.lengthIndirect) {
                const size_t from = static_cast<size_t>(obj.streamStart);
                const size_t found = data_.find("endstream", from);
                obj.rawLength = static_cast<int64_t>(
                    (found == std::string::npos ? data_.size() : found) - from);
            }
        } else if (isDict) {
            obj.type = "dict";
        } else {
            r.skipWs();
            if (r.pos < data_.size() && data_[r.pos] == '[') obj.type = "array";
            else if (r.pos < data_.size() && data_[r.pos] == '/') obj.type = "name";
            else if (r.pos < data_.size() &&
                     (data_[r.pos] == '(' || data_[r.pos] == '<')) obj.type = "string";
            else obj.type = "atom";
        }
        objects.push_back(std::move(obj));
    }

    for (auto& obj : objects) {
        if (!obj.isStream || obj.rawLength >= 0 || obj.lengthRef < 0) continue;
        for (const auto& other : objects) {
            if (other.id != obj.lengthRef || other.isStream) continue;
            size_t body = data_.find("obj", static_cast<size_t>(other.offset));
            if (body == std::string::npos) continue;
            Reader rr{data_, body + 3};
            rr.skipWs();
            long long v;
            if (tryReadNumber(rr, v)) {
                obj.rawLength = v;
                break;
            }
        }
    }

    std::sort(objects.begin(), objects.end(),
              [](const Object& a, const Object& b) { return a.offset < b.offset; });
    parseXrefAndTrailer();
    return true;
}

void PdfFile::parseXrefAndTrailer() {
    // Locate the "xref" keyword (classic xref table).
    const char* const xrefTok = "xref";
    const size_t xrefPos = data_.find(xrefTok);
    if (xrefPos == std::string::npos) {
        trailer.hasXref = false;
        return;
    }

    Reader r{data_, xrefPos + 4};
    r.skipWs();
    while (r.pos < data_.size()) {
        const size_t save = r.pos;
        long long start, count;
        if (!tryReadNumber(r, start) || !tryReadNumber(r, count)) {
            r.pos = save;
            break;
        }
        r.skipWs();
        if (r.starts("trailer")) break;
        if (count <= 0 || count > 10000000) break;
        for (long long i = 0; i < count; ++i) {
            const char* p = data_.c_str() + r.pos;
            char* endp = nullptr;
            errno = 0;
            const long long off = std::strtoll(p, &endp, 10);
            if (endp == p) break;
            r.pos += static_cast<size_t>(endp - p);
            r.skipWs();
            long long gen;
            if (!tryReadNumber(r, gen)) break;
            r.skipWs();
            if (r.pos < data_.size() &&
                (data_[r.pos] == 'n' || data_[r.pos] == 'f')) {
                const bool free = (data_[r.pos] == 'f');
                ++r.pos;
                XrefEntry e;
                e.id = static_cast<int>(start + i);
                e.gen = static_cast<int>(gen);
                e.offset = off;
                e.free = free;
                xref.push_back(e);
                if (free && e.id == 0) owner = e;
            } else {
                break;
            }
            r.skipWs();
        }
    }
    trailer.hasXref = !xref.empty();

    // Locate the trailer dictionary.
    const size_t trailerPos = data_.find("trailer");
    if (trailerPos == std::string::npos) return;
    Reader t{data_, trailerPos + 7};
    t.skipWs();
    if (!t.starts("<<")) return;
    t.pos += 2;
    while (true) {
        t.skipWs();
        if (t.pos >= data_.size()) return;
        if (t.starts(">>")) break;
        if (data_[t.pos] != '/') return;
        ++t.pos;
        const std::string key = readName(t);
        t.skipWs();
        if (key == "Size") {
            long long v;
            if (tryReadNumber(t, v)) trailer.size = static_cast<int>(v);
        } else if (key == "Root") {
            long long v;
            if (tryReadNumber(t, v)) trailer.root = static_cast<int>(v);
        } else if (key == "Info") {
            long long v;
            if (tryReadNumber(t, v)) trailer.info = static_cast<int>(v);
        } else if (key == "Prev") {
            long long v;
            if (tryReadNumber(t, v)) trailer.prev = static_cast<int>(v);
        } else {
            skipValue(t);
        }
    }
}

bool PdfFile::readStream(const Object& obj, std::string& out) const {
    if (!obj.isStream || obj.streamStart < 0 || obj.rawLength < 0) return false;
    const size_t start = static_cast<size_t>(obj.streamStart);
    const size_t len = static_cast<size_t>(obj.rawLength);
    if (start + len > data_.size()) return false;
    out.assign(data_, start, len);
    return true;
}

bool PdfFile::readStreamDecoded(const Object& obj, std::string& out) const {
    std::string raw;
    if (!readStream(obj, raw)) return false;
    out = raw;
    for (auto it = obj.filters.rbegin(); it != obj.filters.rend(); ++it) {
        std::string step;
        if (!decodeFilter(*it, out.data(), out.size(), step)) return false;
        out.swap(step);
    }
    return true;
}

bool PdfFile::readObjectSource(const Object& obj, std::string& out) const {
    if (obj.offset < 0) return false;
    const size_t start = static_cast<size_t>(obj.offset);
    if (start >= data_.size()) return false;
    if (obj.isStream && obj.streamStart >= 0 && obj.rawLength >= 0) {
        // Between encoded stream bytes and the closing "endstream": skip the
        // stream data and resume right after "endstream\n".
        size_t p = static_cast<size_t>(obj.streamStart) +
                   static_cast<size_t>(obj.rawLength);
        const std::string mark = "\nendstream";
        const size_t hit = data_.find(mark, p);
        if (hit == std::string::npos) return false;
        p = hit + mark.size();
        // trim whitespace after endstream
        while (p < data_.size() && (data_[p] == '\r' || data_[p] == '\n'))
            ++p;
        const size_t end = data_.find("endobj", p);
        if (end == std::string::npos) return false;
        out = data_.substr(start, end - start);
        return true;
    }
    const std::string mark = "endobj";
    const size_t end = data_.find(mark, start);
    if (end == std::string::npos) return false;
    out = data_.substr(start, end - start);
    return true;
}

bool applyPredictor(const Object& obj, const std::string& in,
                    std::vector<unsigned char>& out) {
    if (obj.predictor <= 1) {
        out.assign(in.begin(), in.end());
        return true;
    }
    const int colors = obj.components;
    const int bpc = obj.bitsPerComponent;
    const int cols = obj.width;
    if (colors <= 0 || bpc <= 0 || cols <= 0) return false;

    // Bytes per sample-row.
    const int bytesPerRow = (cols * colors * bpc + 7) / 8;
    if (obj.predictor == 2) {
        // TIFF predictor: each sample is the running sum of its row.
        out.clear();
        out.reserve(in.size());
        std::vector<unsigned char> prev(colors * 2, 0);
        for (size_t i = 0; i < in.size(); ++i) {
            const int idx = static_cast<int>(i % colors);
            prev[idx] = static_cast<unsigned char>(
                (in[i] + prev[idx]) & 0xff);
            out.push_back(prev[idx]);
        }
        return true;
    }
    if (obj.predictor < 10 || obj.predictor > 15) return false;
    if (bpc != 8) return false;  // PNG predictors for <8bpc need bit packing

    // PNG predictors: each row starts with a filter-type byte.
    const size_t stride = static_cast<size_t>(bytesPerRow);
    const size_t rowBytes = stride + 1;
    if (in.size() < rowBytes) return false;
    const size_t rows = in.size() / rowBytes;
    if (rows != static_cast<size_t>(obj.height)) return false;

    out.assign(
        reinterpret_cast<const unsigned char*>(in.data()),
        reinterpret_cast<const unsigned char*>(in.data()) +
            static_cast<ptrdiff_t>(rowBytes * rows));
    // Decode rows from top to bottom, tracking the previous (decoded) row.
    for (size_t rown = 0; rown < rows; ++rown) {
        const unsigned char ft = static_cast<unsigned char>(in[rown * rowBytes]);
        const unsigned char* cur =
            reinterpret_cast<const unsigned char*>(in.data()) + rown * rowBytes + 1;
        unsigned char* dst = out.data() + rown * rowBytes + 1;
        const unsigned char* pri = rown == 0
                                       ? nullptr
                                       : out.data() + (rown - 1) * rowBytes + 1;
        switch (ft) {
            case 0:
                break;
            case 1:
                for (size_t i = 0; i < stride; ++i) {
                    const unsigned char left = i >= static_cast<size_t>(colors)
                                                   ? dst[i - static_cast<size_t>(colors)]
                                                   : 0;
                    dst[i] = static_cast<unsigned char>((cur[i] + left) & 0xff);
                }
                break;
            case 2: {
                if (!pri) { std::copy(cur, cur + stride, dst); break; }
                for (size_t i = 0; i < stride; ++i) {
                    dst[i] = static_cast<unsigned char>((cur[i] + pri[i]) & 0xff);
                }
                break;
            }
            case 3: {
                for (size_t i = 0; i < stride; ++i) {
                    const unsigned char left = i >= static_cast<size_t>(colors)
                                                   ? dst[i - static_cast<size_t>(colors)]
                                                   : 0;
                    const unsigned char up = pri ? pri[i] : 0;
                    const unsigned char avg = static_cast<unsigned char>(
                        (static_cast<int>(left) + up) / 2);
                    dst[i] = static_cast<unsigned char>((cur[i] + avg) & 0xff);
                }
                break;
            }
            case 4: {
                for (size_t i = 0; i < stride; ++i) {
                    unsigned char left = 0, up = 0, ul = 0;
                    if (i >= static_cast<size_t>(colors)) {
                        left = dst[i - static_cast<size_t>(colors)];
                        ul = (pri && i >= static_cast<size_t>(colors))
                                 ? pri[i - static_cast<size_t>(colors)]
                                 : 0;
                    }
                    if (pri) up = pri[i];
                    const int p = static_cast<int>(left) + up - static_cast<int>(ul);
                    const int pa = p - static_cast<int>(left);
                    const int pb = p - static_cast<int>(up);
                    const int pc = p - static_cast<int>(ul);
                    const int pav = std::abs(pa), pbv = std::abs(pb),
                              pcv = std::abs(pc);
                    unsigned char pred;
                    if (pav <= pbv && pav <= pcv) pred = static_cast<unsigned char>(left);
                    else if (pbv <= pcv) pred = static_cast<unsigned char>(up);
                    else pred = static_cast<unsigned char>(ul);
                    dst[i] = static_cast<unsigned char>((cur[i] + pred) & 0xff);
                }
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

}  // namespace pdfx