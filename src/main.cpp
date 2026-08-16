#include "cli.h"
#include "mainwindow.h"

#include <string>
#include <vector>

#include <QApplication>

using namespace pdfx;

namespace {
// Cross-platform argument handling: QApplication::arguments() is the only
// Unicode-safe source of command-line args (Windows argv is in the local
// codepage and can mangle non-ASCII paths, unlike QCoreApplication which
// converts via WideCharToMultiByte(UTF-8)).
std::vector<std::string> appArgs(int argc, char** argv) {
    std::vector<std::string> out;
    QStringList qArgs = QApplication::arguments();  // includes argv[0]
    for (int i = 1; i < qArgs.size(); ++i)
        out.push_back(qArgs.at(i).toStdString());
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("pdf-extractor");
    QCoreApplication::setApplicationName("pdf-extractor");
    const std::vector<std::string> args = appArgs(argc, argv);

    int rc = runCli(args);
    if (rc >= 0) return rc;  // handled as a CLI-only invocation

    MainWindow w;
    if (rc == -1 && !args.empty() && args[0] != "gui") {
        w.openPath(QString::fromStdString(args[0]));
    } else {
        // No file argument: reopen the most recent file, if any.
        w.openMostRecent();
    }
    w.show();
    return app.exec();
}