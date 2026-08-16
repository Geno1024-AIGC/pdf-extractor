#ifndef CFF_H
#define CFF_H

#include <string>
#include <vector>

namespace pdfx {

// Result of parsing the embedded CFF (Type 1C) font program of a PDF font
// descriptor. Covers the compact font format used by Adobe Type 1 fonts and
// CIDFontType0 fonts (the default for LaTeX/xeLaTeX PDFs).
struct CffInfo {
    std::vector<std::string> fontNames;   // from the Name INDEX
    std::vector<std::string> strings;     // from the String INDEX
    int glyphCount = 0;                   // entries in the CharStrings INDEX
    bool ok = false;
};

// Parse a raw CFF font program (this is the *decoded* stream bytes). Returns
// false (with ok=false) when the data is not a usable CFF.
bool parseCff(const std::string& data, CffInfo& out);

}  // namespace pdfx

#endif  // CFF_H
