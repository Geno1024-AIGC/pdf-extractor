#include "cli.h"
#include "mainwindow.h"

#include <string>
#include <vector>

#include <QApplication>

using namespace pdfx;

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    int rc = runCli(args);
    if (rc >= 0) return rc;  // handled as a CLI-only invocation

    QApplication app(argc, argv);
    MainWindow w;
    if (rc == -1 && !args.empty() && args[0] != "gui") {
        w.openPath(QString::fromLocal8Bit(args[0].c_str()));
    }
    w.show();
    return app.exec();
}