#ifndef CLI_H
#define CLI_H

#include <functional>
#include <string>
#include <vector>

namespace pdfx {

class PdfFile;

// A small command interpreter shared by the terminal CLI and the GUI
// command line. Output is delivered through `emit`.
//
// result has state:
//   0   ok
//   1   command recognized but failed (message in output)
//   2   quit requested
int execCommand(PdfFile* pdf, const std::string& cmd,
                const std::function<void(const std::string&)>& emit,
                bool lineNumbers = true);

struct CliOptions {
    bool gui = false;                 // force GUI
    int extractId = -1;               // extract a single object number
    std::string outDir;               // extraction target directory
    std::string command;              // remaining CLI subcommand
    std::string pdfPath;              // input PDF
};

int runCli(const std::vector<std::string>& args);

}  // namespace pdfx

#endif  // CLI_H