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

std::string makePreview(const std::string& decoded, size_t maxBytes) {
    std::string out;
    out.reserve(maxBytes + 8);
    size_t n = 0;
    bool binary = false;
    for (unsigned char c : decoded) {
        if (c == '\n' || c == '\r' || c == '\t' || c == 0x0b || c == 0x0c)
            continue;  // compress whitespace for preview
        if (c < 32 || c == 127) {
            binary = true;
            break;
        }
        ++n;
        if (n > maxBytes) break;
        out.push_back(static_cast<char>(c));
    }
    if (binary) {
        // hexdump-style short view
        out.clear();
        size_t shown = 0;
        for (unsigned char c : decoded) {
            if (shown == 0) {
                char line[16];
                std::snprintf(line, sizeof(line), "%04zx: ", shown);
                out += line;
            }
            char h[4];
            std::snprintf(h, sizeof(h), "%02x ", c);
            out += h;
            if (++shown % 16 == 0) out += "\n";
            if (shown >= maxBytes) { out += "...\n"; break; }
        }
        out += "\n[binary data]";
    }
    return out;
}

}  // namespace pdfx