#include "mainwindow.h"

#include <QAbstractItemView>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
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

// Modern dark theme, applied to the whole application.
const char* kStyle = R"QSS(
* {
    font-family: "Inter", "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
    font-size: 13px;
}
QMainWindow, QWidget {
    background-color: #1e1f29;
    color: #e8e9f0;
}
QPushButton {
    background-color: #2d2f43;
    border: 1px solid #3a3d55;
    border-radius: 6px;
    padding: 6px 14px;
    color: #e8e9f0;
}
QPushButton:hover {
    background-color: #3a3d55;
    border-color: #565a7d;
}
QPushButton:pressed { background-color: #23243a; }
QPushButton:disabled {
    background-color: #27283652;
    border-color: #30314a;
    color: #6f7180;
}
QTableWidget {
    background-color: #23243a;
    alternate-background-color: #292a44;
    border: 1px solid #2f3150;
    border-radius: 8px;
    gridline-color: #2f3150;
    selection-background-color: #4a6cf7;
    selection-color: #ffffff;
}
QTableWidget::item:hover { background-color: #3a3d55; }
QHeaderView::section {
    background-color: #2d2f43;
    color: #b9bce0;
    border: none;
    border-bottom: 2px solid #4a6cf7;
    padding: 6px 8px;
    font-weight: 600;
}
QTabWidget::pane { border: 1px solid #2f3150; border-radius: 8px; }
QTabBar::tab {
    background: transparent;
    padding: 6px 16px;
    color: #9aa0c3;
}
QTabBar::tab:selected {
    color: #ffffff;
    border-bottom: 2px solid #4a6cf7;
}
QPlainTextEdit {
    background-color: #16171f;
    border: 1px solid #2f3150;
    border-radius: 8px;
    color: #d7d9e6;
    selection-background-color: #4a6cf7;
}
QLineEdit {
    background-color: #16171f;
    border: 1px solid #2f3150;
    border-radius: 6px;
    padding: 6px 10px;
    color: #e8e9f0;
}
QLineEdit:focus { border-color: #4a6cf7; }
QScrollBar:vertical { background: transparent; width: 10px; }
QScrollBar::handle:vertical {
    background: #3a3d55; border-radius: 5px; min-height: 24px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QLabel { color: #9aa0c3; }
)QSS";

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

    imageLabel_ = new QLabel(this);
    imageLabel_->setAlignment(Qt::AlignCenter);
    auto* imageScroll = new QScrollArea(this);
    imageScroll->setWidget(imageLabel_);
    imageScroll->setWidgetResizable(true);
    imageLabel_->setTextInteractionFlags(Qt::NoTextInteraction);

    previewStack_ = new QStackedWidget(this);
    previewStack_->addWidget(preview_);
    previewStack_->addWidget(imageScroll);

    info_ = new QPlainTextEdit(this);
    info_->setReadOnly(true);

    command_ = new QLineEdit(this);
    command_->setPlaceholderText(
        "command: list, info, preview <n>, extract <n>, extractall, out <dir>");

    console_ = new QPlainTextEdit(this);
    console_->setReadOnly(true);
    console_->setMaximumBlockCount(2000);

    status_ = new QLabel(this);
    status_->setText("ready");

    auto* tabs = new QTabWidget(this);
    tabs->addTab(previewStack_, "Preview");
    tabs->addTab(info_, "Info");
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
    applyStyle();
    resize(1100, 640);
}

void MainWindow::applyStyle() {
    setStyleSheet(kStyle);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
}

void MainWindow::openPath(const QString& path) {
    if (!pdf_.load(path.toStdString())) {
        log("open failed: " + QString::fromStdString(pdf_.error));
        status_->setText("failed to open " + QFileInfo(path).fileName());
        return;
    }
    setWindowTitle("PDF Extractor - " + QFileInfo(path).fileName());
    fillTable();
    fillInfo();
    int nStream = 0;
    for (const auto& o : pdf_.objects)
        if (o.isStream) ++nStream;
    log("opened " + path + " (" + QString::number(pdf_.objects.size()) +
        " objects, " + QString::number(nStream) + " streams)");
    status_->setText(QString("opened %1  ·  %2 objects  ·  %3 streams")
                         .arg(QFileInfo(path).fileName())
                         .arg(pdf_.objects.size())
                         .arg(nStream));
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

void MainWindow::fillInfo() {
    QString text;
    const auto& t = pdf_.trailer;
    text += QString("XRef table: %1\n")
                .arg(t.hasXref ? "found" : "not found (skipped)");
    text += QString("Trailer /Size: %1\n").arg(t.size);
    text += QString("Trailer /Root: %1\n").arg(t.root);
    text += QString("Trailer /Info: %1\n").arg(t.info);
    text += QString("Trailer /Prev: %1\n").arg(t.prev);
    text += QString("XRef entries: %1\n").arg(pdf_.xref.size());
    for (const auto& e : pdf_.xref) {
        text += QString("  %1 %2 %3 %4\n")
                    .arg(e.id)
                    .arg(e.gen)
                    .arg(e.offset)
                    .arg(e.free ? "free" : "live");
    }
    // consistency: which scanned objects are referenced in the xref?
    int inXref = 0;
    for (const auto& o : pdf_.objects) {
        for (const auto& e : pdf_.xref) {
            if (!e.free && e.id == o.id) {
                ++inXref;
                break;
            }
        }
    }
    text += QString("Scanned objects matched by xref: %1/%2\n")
                .arg(inXref)
                .arg(pdf_.objects.size());
    info_->setPlainText(text);
}

void MainWindow::showObject(const Object& o) {
    if (!o.isStream) {
        previewStack_->setCurrentWidget(preview_);
        preview_->setPlainText(
            "object " + QString::number(o.id) + " is not a stream (type " +
            QString::fromStdString(o.type) + ")");
        return;
    }

    std::string decoded;
    if (pdf_.readStreamDecoded(o, decoded)) {
        // Try rendering image streams (DCTDecode passthrough arrives as JPEG).
        if (o.subtype == "Image") {
            QImage img;
            if (img.loadFromData(
                    reinterpret_cast<const uchar*>(decoded.data()),
                    static_cast<int>(decoded.size()))) {
                previewStack_->setCurrentWidget(imageLabel_->parentWidget());
                imageLabel_->setPixmap(
                    QPixmap::fromImage(img).scaled(
                        imageLabel_->size(), Qt::KeepAspectRatio,
                        Qt::SmoothTransformation));
                imageLabel_->adjustSize();
                return;
            }
        }
        previewStack_->setCurrentWidget(preview_);
        preview_->setPlainText(QString::fromStdString(makePreview(decoded)));
    } else {
        std::string raw;
        if (pdf_.readStream(o, raw)) {
            previewStack_->setCurrentWidget(preview_);
            preview_->setPlainText(
                "[decode failed, showing raw bytes]\n" +
                QString::fromStdString(makePreview(raw)));
        } else {
            previewStack_->setCurrentWidget(preview_);
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