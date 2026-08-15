#ifndef PDFPARSER_H
#define PDFPARSER_H

#include <cstdint>
#include <string>
#include <vector>

namespace pdfx {

struct Object {
    int id = 0;                 // object number
    int gen = 0;                // generation number
    int64_t offset = 0;         // byte offset of "N G obj" in file
    bool isStream = false;      // object is a stream object
    std::string type;           // stream / dict / array / string / number / name / ...
    std::vector<std::string> filters;  // /Filter entries (e.g. "FlateDecode")
    int64_t rawLength = 0;      // length of raw stream bytes (-1 unknown)
    int64_t streamStart = -1;   // byte offset of raw stream data
    int lengthRef = -1;         // object number of an indirect /Length reference
    std::string subtype;        // /Subtype if present (e.g. "Image", "Font")
    std::string typeName;       // /Type if present
};

class PdfFile {
public:
    std::vector<Object> objects;
    std::string error;

    // Load a PDF file: scan all top-level objects and classify them.
    // Returns false (with error set) if the file is not a PDF.
    bool load(const std::string& path);

    // Raw undecoded stream bytes for a stream object.
    bool readStream(const Object& obj, std::string& out) const;

    // Decoded stream bytes (filters applied, from outermost to innermost).
    bool readStreamDecoded(const Object& obj, std::string& out) const;

private:
    std::string data_;
};

}  // namespace pdfx

#endif  // PDFPARSER_H