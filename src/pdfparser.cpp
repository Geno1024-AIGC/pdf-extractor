#include "pdfparser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>

namespace pdfx {

namespace {

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return {oss.str().c_str(), oss.str().size()};
}

}  // namespace

bool parsePdf(const std::string& path, Document& doc) {
    const std::string data = readAll(path);
    if (data.empty() || data.compare(0, 5, "%PDF-") != 0) {
        doc.error = "not a PDF file";
        return false;
    }

    const std::regex objRe(R"((\d+)\s+(\d+)\s+obj)");
    const auto begin = std::sregex_iterator(data.begin(), data.end(), objRe);
    const auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        Object obj;
        obj.id = std::stoi((*it)[1].str());
        obj.gen = std::stoi((*it)[2].str());
        obj.offset = static_cast<int64_t>(it->position());
        doc.objects.push_back(obj);
    }
    return true;
}

}  // namespace pdfx