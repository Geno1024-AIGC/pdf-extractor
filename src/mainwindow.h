#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>

#include "codeeditor.h"
#include "pdfparser.h"

class QAction;
class QLineEdit;
class QLabel;
class QPushButton;
class QScrollArea;
class QPixmap;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class PageDiagram;

namespace pdfx {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void openPath(const QString& path);

private slots:
    void openPdf();
    void extractSelected();
    void exportAllImages();
    void runCommand();
    void onRowChanged();
    void showTableMenu(const QPoint& pos);
    void showPreviewMenu(const QPoint& pos);
    void showImageMenu(const QPoint& pos);
    void saveContextObject();
    void savePreviewText();
    void saveDisplayedImage();
    void toggleTheme();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void applyStyle();
    void fillTable();
    void showObject(const Object& o);
    void fillInfo();
    void log(const QString& text);
    void updateImageLabel();
    void gotoRefItem(QTreeWidgetItem* item);
    QTabWidget* previewTabs_;
    QTableWidget* table_;
    CodeEditor* preview_;
    CodeEditor* hexView_;
    CodeEditor* info_;
    QLabel* imageLabel_;
    QScrollArea* imageScroll_;
    QTreeWidget* structView_;
    PageDiagram* pageDiagram_;
    QScrollArea* pageScroll_;
    QWidget* pageBox_;
    QPlainTextEdit* console_;
    QLineEdit* command_;
    QLabel* status_;
    PdfFile pdf_;
    QString outDir_;
    int contextObjId_ = -1;
    QPixmap imagePixmap_;
    bool dark_ = true;
};

}  // namespace pdfx

#endif  // MAINWINDOW_H