#include "extractor.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "pdfparser.h"

namespace pdfx {

std::string guessExtension(const Object& obj, const std::string& decoded) {
    auto hasFilter = [&](const std::string& f) {
        for (const auto& flt : obj.filters)
            if (flt == f) return true;
        return false;
    };
    if (obj.subtype == "Image") {
        if (hasFilter("DCTDecode")) return "jpg";
        if (hasFilter("JPXDecode")) return "jp2";
        if (hasFilter("CCITTFaxDecode")) return "g3";
        if (hasFilter("JBIG2Decode")) return "jb2";
        if (hasFilter("LZWDecode") || hasFilter("FlateDecode")) return "png";
    }
    // content magic sniff
    if (decoded.size() >= 4 && decoded.compare(0, 4, "\x89PNG") == 0)
        return "png";
    if (decoded.size() >= 3 && decoded[0] == '\xff' && decoded[1] == '\xd8' &&
        decoded[2] == '\xff')
        return "jpg";
    if (decoded.size() >= 4 && decoded.compare(0, 4, "%PDF") == 0)
        return "pdf";
    if (decoded.size() >= 5 && decoded.compare(0, 5, "%!PS") == 0)
        return "ps";
    if (decoded.size() >= 4 && decoded.compare(0, 4, "OTTO") == 0)
        return "otf";
    // plain text content: looks printable
    bool printable = true;
    size_t printableCount = 0;
    size_t total = 0;
    for (size_t i = 0; i < decoded.size() && total < 64; ++i) {
        unsigned char c = static_cast<unsigned char>(decoded[i]);
        if (c == '\n' || c == '\r' || c == '\t') ++printableCount;
        else if (c >= 32 && c < 127) ++printableCount;
        else printable = false;
        ++total;
    }
    if (printable && total > 0) return "txt";
    return "bin";
}

std::string extractStream(const PdfFile& pdf, const Object& obj,
                          const std::string& outdir) {
    if (!obj.isStream) return {};
    std::filesystem::path dir(outdir);
    if (!std::filesystem::exists(dir))
        std::filesystem::create_directories(dir);

    std::string decoded;
    bool decoded_ok = pdf.readStreamDecoded(obj, decoded);
    if (!decoded_ok && !pdf.readStream(obj, decoded)) return {};

    const std::string ext =
        decoded_ok ? guessExtension(obj, decoded)
                   : (obj.subtype == "Image" ? "bin" : "raw");
    std::filesystem::path file =
        dir / ("obj_" + std::to_string(obj.id) + "." + ext);

    std::ofstream out(file, std::ios::binary);
    if (!out) return {};
    out.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
    if (!out) return {};
    return file.string();
}

std::vector<std::string> extractAllStreams(const PdfFile& pdf,
                                           const std::string& outdir) {
    std::vector<std::string> written;
    for (const auto& obj : pdf.objects) {
        if (!obj.isStream) continue;
        const std::string f = extractStream(pdf, obj, outdir);
        if (!f.empty()) written.push_back(f);
    }
    return written;
}

std::string makePreview(const std::string& decoded, size_t maxBytes,
                        bool withLineNumbers) {
    // Decide text vs binary by sampling the printable ratio. Bytes >= 0x80
    // count as printable too (UTF-8 text); only control characters and the
    // high-bit runs that aren't valid UTF-8 pull a stream toward binary.
    bool binary = true;
    size_t printable = 0;
    size_t sample = 0;
    for (unsigned char c : decoded) {
        if (sample == 256) break;
        if (c == '\n' || c == '\r' || c == '\t') {
            printable++;
        } else if (c >= 32 && c < 127) {
            printable++;
        } else if (c >= 0x80) {
            printable++;  // assume UTF-8 continuation / non-ASCII text
        }
        sample++;
    }
    if (sample > 0 && printable * 10 >= sample * 8) binary = false;

    if (binary) {
        // hexdump-style short view (no offset prefix, matching the Hex tab)
        std::string out;
        size_t shown = 0;
        for (unsigned char c : decoded) {
            char h[4];
            std::snprintf(h, sizeof(h), "%02x ", c);
            out += h;
            if (++shown % 16 == 0) out += "\n";
            if (shown >= maxBytes) { out += "...\n"; break; }
        }
        out += "\n[binary data]";
        return out;
    }

    // Text: keep newlines, optionally emit a line number prefix per line.
    std::string out;
    std::string line;
    size_t lineNo = 1;
    size_t shown = 0;
    auto flushLine = [&]() {
        if (withLineNumbers) {
            char num[16];
            std::snprintf(num, sizeof(num), "%7zu| ", lineNo);
            out += num;
        }
        ++lineNo;
        out += line;
        if (!out.empty() && out.back() != '\n') out += "\n";
        line.clear();
    };
    for (unsigned char c : decoded) {
        if (c == '\r') continue;  // normalize CRLF
        if (c == '\n') {
            flushLine();
            continue;
        }
        if (c == '\t') c = ' ';
        if (c < 32 || c == 127) {  // stray control char -> treat as binary-ish
            flushLine();
            out += "[...binary shown as hex...]";
            return out;
        }
        if (++shown > maxBytes) {
            line += "…";
            flushLine();
            out += "[truncated]";
            return out;
        }
        line.push_back(static_cast<char>(c));
    }
    if (!line.empty()) flushLine();
    return out;
}

std::vector<std::string> extractShownText(const std::string& content) {
    std::vector<std::string> out;
    size_t i = 0;
    const size_t n = content.size();
    auto isDelim = [](char c) {
        return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
               c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
    };
    while (i < n) {
        const char c = content[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            ++i;
            continue;
        }
        if (c == '%') {  // comment
            while (i < n && content[i] != '\n') ++i;
            continue;
        }
        if (c == '(') {
            // Literal string: '(' ... ')' with \ escapes and balanced parens.
            ++i;
            std::string s;
            int depth = 1;
            while (i < n && depth > 0) {
                const char ch = content[i++];
                if (ch == '\\') {
                    if (i >= n) break;
                    const char e = content[i++];
                    switch (e) {
                        case 'n': s += '\n'; break;
                        case 'r': s += '\r'; break;
                        case 't': s += '\t'; break;
                        case 'b': s += '\b'; break;
                        case 'f': s += '\f'; break;
                        case '(': s += '('; break;
                        case ')': s += ')'; break;
                        case '\\': s += '\\'; break;
                        default:
                            if (e >= '0' && e <= '7') {
                                int oct = e - '0';
                                int k = 1;
                                while (k < 3 && i < n && content[i] >= '0' &&
                                       content[i] <= '7') {
                                    oct = oct * 8 + (content[i] - '0');
                                    ++i;
                                    ++k;
                                }
                                s += static_cast<char>(oct);
                            } else {
                                s += e;
                            }
                    }
                } else if (ch == '(') {
                    ++depth;
                    s += ch;
                } else if (ch == ')') {
                    --depth;
                    if (depth == 0) break;
                    s += ch;
                } else {
                    s += ch;
                }
            }
            out.push_back(s);
            continue;
        }
        if (c == '<') {
            // Either a hex string or a dictionary. A dict is "<<...>>"; a hex
            // string is "<hexdigits>".
            if (i + 1 < n && content[i + 1] == '<') {
                size_t j = i + 2;
                while (j + 1 < n && !(content[j] == '>' && content[j + 1] == '>'))
                    ++j;
                i = (j + 2 <= n) ? j + 2 : j;
                continue;
            }
            ++i;
            std::string hex;
            while (i < n && content[i] != '>') hex += content[i++];
            ++i;  // skip '>'
            std::string s;
            size_t hi = 0;
            while (hi + 1 < hex.size()) {
                auto nib = [&](char h) -> int {
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                    return -1;
                };
                const int hi_ = nib(hex[hi]);
                const int lo = nib(hex[hi + 1]);
                if (hi_ >= 0 && lo >= 0)
                    s += static_cast<char>((hi_ << 4) | lo);
                hi += 2;
            }
            out.push_back(s);
            continue;
        }
        if (isDelim(c)) {
            ++i;
            continue;
        }
        // Any other token (operator such as Tj/TJ/Tf/...) is irrelevant:
        // the strings have already been collected in stream order above.
        while (i < n && !isDelim(content[i]) && content[i] != '\n' &&
               content[i] != '\r' && content[i] != '\t' && content[i] != ' ')
            ++i;
    }
    // Remove the NUL padding bytes often appended by some producers.
    std::vector<std::string> cleaned;
    cleaned.reserve(out.size());
    for (std::string& s : out) {
        while (!s.empty() && (s.back() == '\0' || (unsigned char)s.back() == 0xff))
            s.pop_back();
        cleaned.push_back(s);
    }
    return cleaned;
}

}  // namespace pdfx