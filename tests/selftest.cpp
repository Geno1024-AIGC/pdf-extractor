#include <cstdio>
#include <cstring>

#include "filters.h"
#include "pdfparser.h"

using namespace pdfx;

static const char* kUsage =
    "usage: pdfselftest <file.pdf>\n"
    "Lists objects parsed by PdfFile and decodes + verifies streams.\n";

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs(kUsage, stdout);
        return 1;
    }
    PdfFile pdf;
    if (!pdf.load(argv[1])) {
        std::fprintf(stderr, "load failed: %s\n", pdf.error.c_str());
        return 1;
    }
    std::printf("objects: %zu\n", pdf.objects.size());
    for (const auto& o : pdf.objects) {
        std::printf("obj %d gen %d off %lld type %s%s",
                    o.id, o.gen, static_cast<long long>(o.offset),
                    o.type.c_str(), o.isStream ? " (stream)" : "");
        if (o.isStream) {
            std::printf(" len %lld filters=[", static_cast<long long>(o.rawLength));
            for (size_t i = 0; i < o.filters.size(); ++i)
                std::printf("%s%s", i ? "," : "", o.filters[i].c_str());
            std::printf("]");
            if (!o.typeName.empty()) std::printf(" /Type=%s", o.typeName.c_str());
            if (!o.subtype.empty()) std::printf(" /Subtype=%s", o.subtype.c_str());

            std::string raw, dec;
            if (pdf.readStream(o, raw)) {
                std::printf(" raw=%zuB", raw.size());
                if (!o.filters.empty() && pdf.readStreamDecoded(o, dec)) {
                    std::printf(" decoded=%zuB \"", dec.size());
                    for (char cc : dec) {
                        if (cc >= 32 && cc < 127) std::printf("%c", cc);
                        else std::printf("\\x%02x", static_cast<unsigned char>(cc));
                    }
                    std::printf("\"");
                }
            }
        }
        std::printf("\n");
    }
    return 0;
}