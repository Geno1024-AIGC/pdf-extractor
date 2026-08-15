#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>

#include "codeeditor.h"
#include "pdfparser.h"

class QTableWidget;
class QLineEdit;
class QLabel;
class QPushButton;
class QStackedWidget;
class QScrollArea;
class QPixmap;
class QContextMenuEvent;
class QResizeEvent;

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
    void showTableMenu(const QPoint& pos);
    void showPreviewMenu(const QPoint& pos);
    void showImageMenu(const QPoint& pos);
    void saveContextObject();
    void savePreviewText();
    void saveDisplayedImage();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void applyStyle();
    void fillTable();
    void showObject(const Object& o);
    void fillInfo();
    void log(const QString& text);
    void updateImageLabel();
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
    int contextObjId_ = -1;
    QPixmap imagePixmap_;
};

}  // namespace pdfx

#endif  // MAINWINDOW_H