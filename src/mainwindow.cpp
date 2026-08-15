#include "mainwindow.h"

#include <QAbstractItemView>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "cli.h"
#include "extractor.h"

namespace pdfx {

namespace {
QString filterText(const Object& o) {
    QString f;
    for (const auto& s : o.filters) {
        if (!f.isEmpty()) f += ",";
        f += QString::fromStdString(s);
    }
    return f;
}
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("PDF Extractor");
    outDir_ = QDir::currentPath();

    openBtn_ = new QPushButton("Open PDF", this);
    extractBtn_ = new QPushButton("Extract Selected", this);
    extractBtn_->setEnabled(false);

    table_ = new QTableWidget(this);
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels(
        {"#", "Type", "Subtype", "Filter", "Size", "Offset"});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setStretchLastSection(true);

    preview_ = new QPlainTextEdit(this);
    preview_->setReadOnly(true);

    command_ = new QLineEdit(this);
    command_->setPlaceholderText(
        "command: list, preview <n>, extract <n>, extractall, out <dir>");

    console_ = new QPlainTextEdit(this);
    console_->setReadOnly(true);
    console_->setMaximumBlockCount(2000);

    status_ = new QLabel(this);
    status_->setText("ready");

    auto* tabs = new QTabWidget(this);
    tabs->addTab(preview_, "Preview");
    tabs->addTab(console_, "Console");

    auto* cols = new QHBoxLayout;
    cols->addWidget(table_, 3);
    cols->addWidget(tabs, 2);

    auto* toolbar = new QHBoxLayout;
    toolbar->addWidget(openBtn_);
    toolbar->addWidget(extractBtn_);

    auto* cmdRow = new QHBoxLayout;
    cmdRow->addWidget(new QLabel(QString::fromUtf8("\u203a"), this));
    cmdRow->addWidget(command_);

    auto* root = new QVBoxLayout;
    root->addLayout(toolbar);
    root->addLayout(cols, 1);
    root->addLayout(cmdRow);
    root->addWidget(status_);

    auto* central = new QWidget(this);
    central->setLayout(root);
    setCentralWidget(central);

    connect(openBtn_, &QPushButton::clicked, this, &MainWindow::openPdf);
    connect(extractBtn_, &QPushButton::clicked, this,
            &MainWindow::extractSelected);
    connect(command_, &QLineEdit::returnPressed, this, &MainWindow::runCommand);
    connect(table_, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::onRowChanged);
    connect(table_, &QTableWidget::itemActivated,
            [this](QTableWidgetItem*) { onRowChanged(); });
    resize(1100, 640);
}

void MainWindow::openPath(const QString& path) {
    if (!pdf_.load(path.toStdString())) {
        log("open failed: " + QString::fromStdString(pdf_.error));
        status_->setText("failed to open " + QFileInfo(path).fileName());
        return;
    }
    setWindowTitle("PDF Extractor - " + QFileInfo(path).fileName());
    fillTable();
    log("opened " + path + " (" + QString::number(pdf_.objects.size()) +
        " objects)");
    status_->setText("opened " + QFileInfo(path).fileName());
    extractBtn_->setEnabled(true);
}

void MainWindow::openPdf() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Open PDF", {}, "PDF files (*.pdf)");
    if (path.isEmpty()) return;
    openPath(path);
}

void MainWindow::extractSelected() {
    const int row = table_->currentRow();
    if (row < 0) return;
    const Object& o = pdf_.objects[static_cast<size_t>(row)];
    if (!o.isStream) {
        log("object " + QString::number(o.id) + " is not a stream");
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Extract to directory", outDir_);
    if (dir.isEmpty()) return;
    outDir_ = dir;
    const std::string f = extractStream(pdf_, o, outDir_.toStdString());
    if (!f.empty()) {
        log("extracted obj " + QString::number(o.id) + " -> " +
            QString::fromStdString(f));
        status_->setText("extracted obj " + QString::number(o.id));
    } else {
        log("extraction failed for obj " + QString::number(o.id));
    }
}

void MainWindow::fillTable() {
    table_->setRowCount(static_cast<int>(pdf_.objects.size()));
    for (int r = 0; r < static_cast<int>(pdf_.objects.size()); ++r) {
        const Object& o = pdf_.objects[static_cast<size_t>(r)];
        QStringList cells = {
            QString::number(o.id),
            QString::fromStdString(o.type),
            QString::fromStdString(o.subtype),
            filterText(o),
            o.isStream ? QString::number(o.rawLength) : QString("-"),
            QString::number(o.offset)};
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells[c]);
            if (c == 0) item->setData(Qt::UserRole, o.id);
            table_->setItem(r, c, item);
        }
    }
}

void MainWindow::onRowChanged() {
    const int row = table_->currentRow();
    if (row < 0 || row >= static_cast<int>(pdf_.objects.size())) return;
    showObject(pdf_.objects[static_cast<size_t>(row)]);
}

void MainWindow::showObject(const Object& o) {
    if (!o.isStream) {
        preview_->setPlainText(
            "object " + QString::number(o.id) + " is not a stream (type " +
            QString::fromStdString(o.type) + ")");
        return;
    }
    std::string decoded;
    if (pdf_.readStreamDecoded(o, decoded)) {
        preview_->setPlainText(QString::fromStdString(makePreview(decoded)));
    } else {
        std::string raw;
        if (pdf_.readStream(o, raw)) {
            preview_->setPlainText(
                "[decode failed, showing raw bytes]\n" +
                QString::fromStdString(makePreview(raw)));
        } else {
            preview_->setPlainText("could not read stream data");
        }
    }
}

void MainWindow::runCommand() {
    const QString cmd = command_->text().trimmed();
    if (cmd.isEmpty()) return;
    command_->clear();
    console_->appendPlainText("> " + cmd);

    const QString lower = cmd.toLower();
    const bool toPreview = lower.startsWith("preview ");

    int rc = execCommand(&pdf_, cmd.toStdString(), [&](const std::string& s) {
        if (toPreview) {
            preview_->setPlainText(QString::fromStdString(s));
        } else {
            console_->appendPlainText(QString::fromStdString(s));
        }
        status_->setText(QString::fromStdString(s));
    });

    if (lower.startsWith("open ") && !pdf_.objects.empty()) fillTable();
    if (rc == 2) close();
}

void MainWindow::log(const QString& text) {
    console_->appendPlainText(text);
}

}  // namespace pdfx