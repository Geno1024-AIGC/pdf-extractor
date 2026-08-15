#include "mainwindow.h"

#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pdfx {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("PDF Extractor");

    auto* openBtn = new QPushButton("Open PDF", this);
    auto* extractBtn = new QPushButton("Extract All", this);

    table_ = new QTableWidget(this);
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels(
        {"#", "Type", "Subtype", "Filter", "Size", "Offset"});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);

    preview_ = new QPlainTextEdit(this);
    preview_->setReadOnly(true);

    command_ = new QLineEdit(this);
    command_->setPlaceholderText("command: list, extract, extractall, open <file>");

    status_ = new QLabel(this);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(preview_, "Preview");
    console_ = new QPlainTextEdit(this);
    console_->setReadOnly(true);
    tabs->addTab(console_, "Console");

    auto* cols = new QHBoxLayout;
    cols->addWidget(table_, 2);
    cols->addWidget(tabs, 1);

    auto* cmdRow = new QHBoxLayout;
    cmdRow->addWidget(new QLabel(QString::fromUtf8("\u203a"), this));
    cmdRow->addWidget(command_);

    auto* finalCol = new QVBoxLayout;
    finalCol->addLayout(cols);
    finalCol->addLayout(cmdRow);
    finalCol->addWidget(status_);

    auto* finalCentral = new QWidget(this);
    finalCentral->setLayout(finalCol);
    setCentralWidget(finalCentral);

    connect(openBtn, &QPushButton::clicked, this, &MainWindow::openPdf);
    connect(extractBtn, &QPushButton::clicked, this, &MainWindow::extractAll);
    connect(command_, &QLineEdit::returnPressed, this, &MainWindow::runCommand);
    resize(1000, 600);
}

void MainWindow::openPdf() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Open PDF", {}, "PDF files (*.pdf)");
    if (path.isEmpty()) return;
    openPath(path);
}

void MainWindow::openPath(const QString& path) {
    command_->setText("open \"" + path + "\"");
    runCommand();
}

void MainWindow::extractAll() {
    status_->setText("Not implemented yet.");
}

void MainWindow::runCommand() {
    const QString cmd = command_->text().trimmed();
    if (cmd.isEmpty()) return;
    status_->setText("Command run: " + cmd);
}

}  // namespace pdfx