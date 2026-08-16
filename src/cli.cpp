#include "cli.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>

#include "extractor.h"
#include "pdfparser.h"

namespace pdfx {

namespace {

std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

void printHelp(const std::function<void(const std::string&)>& emit) {
    emit("commands:");
    emit("  open <file>        load a PDF file");
    emit("  list               list all parsed objects");
    emit("  info               show file structure (xref/trailer)");
    emit("  preview <n>        show decoded content of object n");
    emit("  extract <n>        extract object n to the output dir");
    emit("  extract [ns...]    extract the given objects");
    emit("  extractall         extract every stream object");
    emit("  export <n>         export one stream object (image) by id");
    emit("  tree <n>           show the structure tree of an object");
    emit("  out <dir>          set the output directory");
    emit("  help               show this help");
    emit("  quit               exit");
}

// Minimal structural pretty-printer: re-indent the source of a small object
// so nested dicts/arrays become a readable tree.
void printTreeText(const std::string& src,
                   const std::function<void(const std::string&)>& emit) {
    auto indent = [](int n) { return std::string(static_cast<size_t>(n) * 2, ' '); };
    int depth = 0;
    std::string tok;
    auto flush = [&] {
        if (tok.empty()) return;
        emit(indent(depth) + tok);
        tok.clear();
    };
    char prev = 0;
    for (char ch : src) {
        if (ch == '<' && prev == '<') {
            tok.pop_back();  // drop the '<' that opened this pair
            flush();
            ++depth;
            emit(indent(depth) + "<<");
            prev = 0;
            continue;
        }
        if (ch == '>' && prev == '>') {
            tok.pop_back();
            flush();
            --depth;
            if (depth < 0) depth = 0;
            emit(indent(depth) + ">>");
            prev = 0;
            continue;
        }
        if (ch == '[') {
            flush();
            ++depth;
            emit(indent(depth) + "[");
            prev = 0;
            continue;
        }
        if (ch == ']') {
            flush();
            --depth;
            if (depth < 0) depth = 0;
            emit(indent(depth) + "]");
            prev = 0;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            flush();
            prev = 0;
            continue;
        }
        tok += ch;
        prev = ch;
    }
    if (!tok.empty()) emit(indent(depth) + tok);
}

bool tryParseInt(const std::string& s, int& v) {
    try {
        size_t used = 0;
        v = std::stoi(s, &used);
        return used == s.size();
    } catch (...) {
        return false;
    }
}

}  // namespace

int execCommand(PdfFile* pdf, const std::string& cmd,
                const std::function<void(const std::string&)>& emit,
                bool lineNumbers) {
    const std::vector<std::string> args = tokenize(cmd);
    if (args.empty()) return 0;

    const std::string& c = args[0];
    if (c == "help" || c == "?" || c == "h") {
        printHelp(emit);
        return 0;
    }
    if (c == "quit" || c == "exit" || c == "q") {
        emit("bye");
        return 2;
    }
    if (c == "open") {
        if (args.size() < 2) {
            emit("usage: open <file>");
            return 1;
        }
        if (!pdf->load(args[1])) {
            emit("open failed: " + pdf->error);
            return 1;
        }
        emit("opened " + args[1] + ": " +
             std::to_string(pdf->objects.size()) + " objects");
        return 0;
    }
    if (!pdf->objects.empty() && c == "info") {
        const auto& t = pdf->trailer;
        emit("trailer: " + std::string(t.hasXref ? "xref table found" :
                                                  "no classic xref"));
        emit("  /Size " + std::to_string(t.size));
        emit("  /Root " + std::to_string(t.root));
        emit("  /Info " + std::to_string(t.info));
        emit("  /Prev " + std::to_string(t.prev));
        emit("xref entries: " + std::to_string(pdf->xref.size()));
        for (const auto& e : pdf->xref) {
            emit("  " + std::to_string(e.id) + " " + std::to_string(e.gen) +
                 " " + std::to_string(e.offset) +
                 (e.free ? " f" : " n"));
        }
        return 0;
    }
    if (!pdf->objects.empty() && c == "list") {
        for (const auto& o : pdf->objects) {
            std::string line = "obj " + std::to_string(o.id) + " g" +
                               std::to_string(o.gen) + " [" + o.type + "]";
            if (o.isStream) {
                line += " len=" + std::to_string(o.rawLength);
                if (!o.filters.empty()) {
                    line += " filter=";
                    for (size_t i = 0; i < o.filters.size(); ++i) {
                        if (i) line += ",";
                        line += o.filters[i];
                    }
                }
                if (!o.subtype.empty()) line += " subtype=" + o.subtype;
            }
            emit(line);
        }
        return 0;
    }
    if (c == "preview") {
        if (args.size() < 2) {
            emit("usage: preview <n>");
            return 1;
        }
        int id;
        if (!tryParseInt(args[1], id))
            emit("preview: bad number '" + args[1] + "'");
        else {
            for (const auto& o : pdf->objects) {
                if (o.id != id) continue;
                std::string decoded;
                if (o.isStream && pdf->readStreamDecoded(o, decoded)) {
                    emit(makePreview(decoded, 4096, lineNumbers));
                } else if (o.isStream) {
                    emit("object " + std::to_string(id) +
                         " is a stream but could not be decoded");
                } else {
                    emit("object " + std::to_string(id) +
                         " is not a stream (type " + o.type + ")");
                }
                return 0;
            }
            emit("no object " + std::to_string(id));
        }
        return 0;
    }
    if (c == "out") {
        if (args.size() < 2) {
            emit("usage: out <dir>");
            return 1;
        }
        std::filesystem::create_directories(args[1]);
        emit("output dir: " + args[1]);
        return 0;
    }
    if (c == "export") {
        if (args.size() < 2) {
            emit("usage: export <n>");
            return 1;
        }
        int id;
        if (!tryParseInt(args[1], id)) {
            emit("export: bad number '" + args[1] + "'");
            return 0;
        }
        const Object* found = nullptr;
        for (const auto& o : pdf->objects) {
            if (o.id == id) {
                found = &o;
                break;
            }
        }
        if (!found) {
            emit("no object " + std::to_string(id));
            return 0;
        }
        if (!found->isStream) {
            emit("object " + std::to_string(id) +
                 " is not a stream (type " + found->type + ")");
            return 0;
        }
        const std::string f = extractStream(*pdf, *found, ".");
        if (!f.empty()) {
            emit("exported obj " + std::to_string(id) + " -> " + f);
        } else {
            emit("export obj " + std::to_string(id) + " failed");
        }
        return 0;
    }
    if (c == "tree") {
        if (args.size() < 2) {
            emit("usage: tree <n>");
            return 1;
        }
        int id;
        if (!tryParseInt(args[1], id)) {
            emit("tree: bad number '" + args[1] + "'");
            return 0;
        }
        for (const auto& o : pdf->objects) {
            if (o.id != id) continue;
            std::string src;
            if (!pdf->readObjectSource(o, src)) {
                emit("object " + std::to_string(id) +
                     " has no readable source");
                return 0;
            }
            emit("=== object " + std::to_string(id) + " (type " + o.type +
                 ") ===");
            printTreeText(src, emit);
            return 0;
        }
        emit("no object " + std::to_string(id));
        return 0;
    }
    if (c == "extract" || c == "extractall") {
        // extract [n n n ...]   -- extract given object ids (or all)
        std::vector<int> ids;
        for (size_t i = 1; i < args.size(); ++i) {
            int v;
            if (tryParseInt(args[i], v)) ids.push_back(v);
            else emit("ignoring non-number argument: " + args[i]);
        }
        size_t done = 0;
        for (const auto& o : pdf->objects) {
            if (!o.isStream) continue;
            if (c == "extract" && !ids.empty() &&
                std::find(ids.begin(), ids.end(), o.id) == ids.end())
                continue;
            const std::string f = extractStream(*pdf, o, ".");
            if (!f.empty()) {
                emit("extracted obj " + std::to_string(o.id) + " -> " + f);
                ++done;
            } else {
                emit("extract obj " + std::to_string(o.id) + " failed");
            }
        }
        emit("extracted " + std::to_string(done) + " stream(s)");
        return 0;
    }
    emit("unknown command: " + c + " (try 'help')");
    return 1;
}

int runCli(const std::vector<std::string>& args) {
    // pdfx <file.pdf> extract-all         -> extract every stream into CWD
    // pdfx <file.pdf> <object numbers...> -> extract those objects
    // pdfx list <file.pdf>
    PdfFile pdf;
    size_t i = 0;
    std::string target;
    bool isGui = args.empty();
    int rc = 0;

    auto outCb = [](const std::string& s) { std::cout << s << "\n"; };

    if (!args.empty() && args[0] == "list") {
        if (args.size() < 2) {
            std::cerr << "usage: pdfx list <file.pdf>\n";
            return 1;
        }
        target = args[1];
        if (!pdf.load(target)) {
            std::cerr << "load failed: " << pdf.error << "\n";
            return 1;
        }
        execCommand(&pdf, "list", outCb);
        return 0;
    }
    if (!args.empty() && args[0] == "info") {
        if (args.size() < 2) {
            std::cerr << "usage: pdfx info <file.pdf>\n";
            return 1;
        }
        target = args[1];
        if (!pdf.load(target)) {
            std::cerr << "load failed: " << pdf.error << "\n";
            return 1;
        }
        execCommand(&pdf, "info", outCb);
        return 0;
    }
    if (!args.empty() && args[0] == "extract") {
        // pdfx extract <file.pdf> [object ids...]
        if (args.size() < 2) {
            std::cerr << "usage: pdfx extract <file.pdf> [obj ids...]\n"
                         "       with no ids, extracts every stream.\n";
            return 1;
        }
        target = args[1];
        if (!pdf.load(target)) {
            std::cerr << "load failed: " << pdf.error << "\n";
            return 1;
        }
        std::string spec = "extractall";
        if (args.size() > 2) {
            spec = "extract";
            for (size_t j = 2; j < args.size(); ++j) spec += " " + args[j];
        }
        execCommand(&pdf, spec, outCb);
        return 0;
    }
    if (!args.empty() && args[0] == "preview") {
        // pdfx preview <file.pdf> <id>
        if (args.size() < 3) {
            std::cerr << "usage: pdfx preview <file.pdf> <id>\n";
            return 1;
        }
        target = args[1];
        if (!pdf.load(target)) {
            std::cerr << "load failed: " << pdf.error << "\n";
            return 1;
        }
        return execCommand(&pdf, "preview " + args[2], outCb);
    }
    if (!args.empty() && args[0] == "export") {
        // pdfx export <file.pdf> <id>
        if (args.size() < 3) {
            std::cerr << "usage: pdfx export <file.pdf> <id>\n";
            return 1;
        }
        target = args[1];
        if (!pdf.load(target)) {
            std::cerr << "load failed: " << pdf.error << "\n";
            return 1;
        }
        return execCommand(&pdf, "export " + args[2], outCb);
    }
    if (!args.empty() && args[0] == "tree") {
        // pdfx tree <file.pdf> <id>
        if (args.size() < 3) {
            std::cerr << "usage: pdfx tree <file.pdf> <id>\n";
            return 1;
        }
        target = args[1];
        if (!pdf.load(target)) {
            std::cerr << "load failed: " << pdf.error << "\n";
            return 1;
        }
        return execCommand(&pdf, "tree " + args[2], outCb);
    }
    // No arguments (or explicit "gui") -> launch the GUI.
    if (args.empty() || args[0] == "gui") return -1;

    // bare ".pdf" argument (not a subcommand) -> open it in the GUI
    if (args[0] != "list" && args[0] != "extract" && args[0] != "info" &&
        args[0] != "export" && args[0] != "tree" &&
        args[0] != "preview" &&
        (args[0].size() > 4 && args[0].rfind(".pdf") != std::string::npos)) {
        return -1;
    }

    std::cerr << "usage:\n"
                 "  pdfx                       start the GUI\n"
                 "  pdfx <file.pdf>            open file in the GUI\n"
                 "  pdfx list <file.pdf>       list objects\n"
                 "  pdfx info <file.pdf>       show file structure\n"
                 "  pdfx preview <file.pdf> <id>          show decoded stream\n"
                 "  pdfx extract <file.pdf> [obj id...]  extract streams\n"
                 "  pdfx export <file.pdf> <id>           export one stream\n"
                 "  pdfx tree <file.pdf> <id>             show object tree\n";
    return 2;
}

}  // namespace pdfx