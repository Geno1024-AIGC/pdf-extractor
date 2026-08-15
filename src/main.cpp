#include "filters.h"
#include "mainwindow.h"
#include "pdfparser.h"

#include <cstdio>
#include <cstdlib>

#include <QApplication>

using namespace pdfx;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow w;
    if (argc > 1) w.openPath(QString::fromLocal8Bit(argv[1]));
    w.show();
    return app.exec();
}