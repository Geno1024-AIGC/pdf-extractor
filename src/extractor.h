#ifndef EXTRACTOR_H
#define EXTRACTOR_H

#include <string>
#include <vector>

namespace pdfx {

class PdfFile;

// Guess a sensible file extension for a decoded stream based on /Subtype,
// filter names and content magic bytes. Returns a string without the dot.
std::string guessExtension(const class Object& obj, const std::string& decoded);

// Extract the decoded content of every stream object in `pdf` into `outdir`.
// Files are named "obj_<number>.<ext>" (or "_raw" suffix added on error).
// Returns the list of written files.
std::vector<std::string> extractAllStreams(const PdfFile& pdf,
                                           const std::string& outdir);

// Extract a single stream object into `outdir`, returning its file path.
// Returns an empty string on failure.
std::string extractStream(const PdfFile& pdf, const class Object& obj,
                          const std::string& outdir);

// Convert decoded bytes to a human-readable preview (up to `maxBytes`).
std::string makePreview(const std::string& decoded, size_t maxBytes = 4096);

}  // namespace pdfx

#endif  // EXTRACTOR_H