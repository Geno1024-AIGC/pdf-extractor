#ifndef FILTERS_H
#define FILTERS_H

#include <string>

namespace pdfx {

// Decode a PDF stream according to a single /Filter name (e.g. "FlateDecode").
// Returns false if the filter is unknown or decoding fails.
bool decodeFilter(const std::string& name,
                  const char* src, size_t srcLen,
                  std::string& out);

}  // namespace pdfx

#endif  // FILTERS_H