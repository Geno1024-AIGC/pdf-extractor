#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>

#include "codeeditor.h"
#include "pdfparser.h"

class QTableWidget;
class QLineEdit;
class QLineEdit;
class QLabel;
class QPushButton;
class QStackedWidget;
class QScrollArea;

namespace pdfx {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void openPath(const QString& path);

private slots:
    void openPdf();
    void extractSelected();
    void runCommand();
    void onRowChanged();

private:
    void applyStyle();
    void fillTable();
    void showObject(const Object& o);
    void fillInfo();
    void log(const QString& text);
    QPushButton* openBtn_;
    QPushButton* extractBtn_;
    QTableWidget* table_;
    CodeEditor* preview_;
    CodeEditor* info_;
    QStackedWidget* previewStack_;
    QLabel* imageLabel_;
    QScrollArea* imageScroll_;
    QPlainTextEdit* console_;
    QLineEdit* command_;
    QLabel* status_;
    PdfFile pdf_;
    QString outDir_;
};

}  // namespace pdfx

#endif  // MAINWINDOW_H