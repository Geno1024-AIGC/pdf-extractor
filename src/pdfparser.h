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
    int64_t rawLength = 0;      // length of raw stream bytes
    std::string subtype;        // /Subtype if present (e.g. "Image", "Font")
    std::string typeName;       // /Type if present
};

struct Document {
    std::vector<Object> objects;
    std::string error;
};

bool parsePdf(const std::string& path, Document& doc);

}  // namespace pdfx

#endif  // PDFPARSER_H