#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class QTableWidget;
class QPlainTextEdit;
class QLineEdit;
class QLabel;

namespace pdfx {
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void openPath(const QString& path);

private slots:
    void openPdf();
    void extractAll();
    void runCommand();

private:
    QTableWidget* table_;
    QPlainTextEdit* preview_;
    QPlainTextEdit* console_;
    QLineEdit* command_;
    QLabel* status_;
};

}  // namespace pdfx

#endif  // MAINWINDOW_H