#include "filters.h"
#include "pdfparser.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

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
        } else {
            skipValue(r);
        }
    }
}

}  // namespace

bool PdfFile::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
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
    return true;
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

}  // namespace pdfx