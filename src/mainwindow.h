#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>

#include "codeeditor.h"
#include "pdfparser.h"

class QAction;
class QDragEnterEvent;
class QDropEvent;
class QLineEdit;
class QLabel;
class QMenu;
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
    void setLanguage(bool zh);
    void runCommand();
    void onRowChanged();
    void applyFilter();
    void updateRecentMenu();
    void removeRecent(const QString& path);
    void showTableMenu(const QPoint& pos);
    void showPreviewMenu(const QPoint& pos);
    void showImageMenu(const QPoint& pos);
    void showStructMenu(const QPoint& pos);
    void copySubtreeJson();
    void copySubtreeText();
    void jumpToRawOffset();
    void saveContextObject();
    void savePreviewText();
    void saveDisplayedImage();
    void toggleTheme();
    void zoomIn();
    void zoomOut();
    void zoomFit();
    void zoomOneToOne();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void applyStyle();
    void fillTable();
    void showObject(const Object& o);
    void populateFonts(const Object& o, const std::string& decoded);
    void fillInfo();
    void log(const QString& text);
    void updateImageLabel();
    void gotoRefItem(QTreeWidgetItem* item);
    QString tr_(const QString& en, const QString& zh) const;
    void retranslate();
    QMenu* fileMenu_ = nullptr;
    QMenu* exportMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QMenu* settingsMenu_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QTabWidget* previewTabs_;
    QTabWidget* outerTabs_ = nullptr;
    QTableWidget* table_;
    QLineEdit* filterEdit_ = nullptr;
    CodeEditor* preview_;
    CodeEditor* hexView_;
    CodeEditor* contentView_;
    CodeEditor* fontView_;
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
    QString currentFile_;
    int contextObjId_ = -1;
    QPixmap imagePixmap_;
    bool dark_ = true;
    bool zh_ = false;
};

}  // namespace pdfx

#endif  // MAINWINDOW_H