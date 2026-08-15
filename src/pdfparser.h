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

    // Image geometry (only meaningful when subtype == "Image").
    int width = 0;
    int height = 0;
    int bitsPerComponent = 8;
    int components = 1;         // channels implied by /ColorSpace
    int predictor = 1;          // /DecodeParms /Predictor (1 none, 2 TIFF, >=10 PNG)
    std::string colorspace;     // /ColorSpace name (DeviceRGB, DeviceGray, ...)
};

struct XrefEntry {
    int id = 0;                 // object number
    int gen = 0;                // generation number
    int64_t offset = 0;         // byte offset of the object, or 0 if free
    bool free = false;          // entry is a free-list node
};

struct TrailerInfo {
    int size = 0;               // /Size: number of object entries (0 if absent)
    int root = 0;               // /Root object number
    int info = 0;               // /Info object number
    int prev = 0;               // /Prev (xref chain) offset
    bool hasXref = false;       // a classic xref table was found
};

class PdfFile {
public:
    std::vector<Object> objects;
    XrefEntry owner;            // 0 0 f, the head of the free list
    std::vector<XrefEntry> xref;
    TrailerInfo trailer;
    std::string error;

    // Load a PDF file: scan all top-level objects and classify them.
    // Returns false (with error set) if the file is not a PDF.
    bool load(const std::string& path);

    // Load from a native path (Unicode-safe on Windows via wide chars).
    // Falls back to load(path) on platforms without wide file support.
    bool loadPath(const std::string& path);

    // Raw undecoded stream bytes for a stream object.
    bool readStream(const Object& obj, std::string& out) const;

    // Decoded stream bytes (filters applied, from outermost to innermost).
    bool readStreamDecoded(const Object& obj, std::string& out) const;

private:
    void parseXrefAndTrailer();
    std::string data_;
};

// Undo a /DecodeParms predictor (TIFF Predictor 2 and PNG 10..15) so the raw
// image sample row bytes are returned. `obj` carries the image geometry.
// Returns false if the predictor is unsupported or the data is inconsistent.
bool applyPredictor(const Object& obj, const std::string& in,
                    std::vector<unsigned char>& out);

}  // namespace pdfx

#endif  // PDFPARSER_H