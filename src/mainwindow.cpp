#include "mainwindow.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
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

// Render an image stream object to a QImage, handling both embedded file
// formats (JPEG/PNG data) and raw pixel samples (FlateDecode + predictor).
QImage renderObjectImage(const PdfFile& pdf, const Object& o) {
    QImage out;
    std::string decoded;
    if (!pdf.readStreamDecoded(o, decoded)) return out;
    if (out.loadFromData(
            reinterpret_cast<const uchar*>(decoded.data()),
            static_cast<int>(decoded.size())))
        return out;
    std::vector<unsigned char> samples;
    if (!applyPredictor(o, decoded, samples) || o.width <= 0 ||
        o.height <= 0 || o.bitsPerComponent != 8)
        return out;
    QImage::Format fmt = QImage::Format_Invalid;
    switch (o.components) {
        case 1: fmt = QImage::Format_Grayscale8; break;
        case 3: fmt = QImage::Format_RGB888; break;
        case 4: fmt = QImage::Format_RGB32; break;
        default: return out;
    }
    const int stride = o.width * o.components;
    if (samples.size() <
        static_cast<size_t>(o.height) * static_cast<size_t>(stride))
        return out;
    if (o.components == 4) {
        for (size_t i = 0; i + 4 <= samples.size(); i += 4) {
            const int c = samples[i], m = samples[i + 1];
            const int y = samples[i + 2], k = samples[i + 3];
            samples[i] = static_cast<unsigned char>((255 - c) * (255 - k) / 255);
            samples[i + 1] = static_cast<unsigned char>((255 - m) * (255 - k) / 255);
            samples[i + 2] = static_cast<unsigned char>((255 - y) * (255 - k) / 255);
            samples[i + 3] = 255;
        }
        return QImage(samples.data(), o.width, o.height, stride,
                      QImage::Format_RGB32);
    }
    return QImage(samples.data(), o.width, o.height, stride, fmt, nullptr,
                  nullptr);
}

// Build a hex dump of `bytes`, capped at `maxBytes`.
QString hexDump(const std::string& bytes, size_t maxBytes) {
    QString out;
    size_t shown = 0;
    for (unsigned char c : bytes) {
        if (shown == 0) {
            char line[16];
            std::snprintf(line, sizeof(line), "%04zx: ", shown);
            out += QString::fromLatin1(line);
        }
        char h[4];
        std::snprintf(h, sizeof(h), "%02x ", c);
        out += QString::fromLatin1(h);
        if (++shown % 16 == 0) out += "\n";
        if (shown >= maxBytes) {
            out += "…\n[truncated]";
            break;
        }
    }
    if (out.isEmpty()) out = "(empty)";
    return out;
}

// Dark theme, applied to the whole application.
const char* kDarkStyle = R"QSS(
* {
    font-family: "JetBrains Mono", "Cascadia Mono", "Consolas", "Menlo",
                 "DejaVu Sans Mono", monospace;
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

// Light theme, applied when the user toggles the colour mode.
const char* kLightStyle = R"QSS(
* {
    font-family: "JetBrains Mono", "Cascadia Mono", "Consolas", "Menlo",
                 "DejaVu Sans Mono", monospace;
    font-size: 13px;
}
QMainWindow, QWidget {
    background-color: #f5f6fa;
    color: #22242e;
}
QPushButton {
    background-color: #e8eaf2;
    border: 1px solid #c9cddd;
    border-radius: 6px;
    padding: 6px 14px;
    color: #22242e;
}
QPushButton:hover {
    background-color: #dde0ea;
    border-color: #aab0c9;
}
QPushButton:pressed { background-color: #cfd3e0; }
QPushButton:disabled {
    background-color: #f0f1f6;
    border-color: #d8dae4;
    color: #a3a6b4;
}
QTableWidget {
    background-color: #ffffff;
    alternate-background-color: #eef0f7;
    border: 1px solid #d0d3e0;
    border-radius: 8px;
    gridline-color: #d0d3e0;
    selection-background-color: #4a6cf7;
    selection-color: #ffffff;
}
QTableWidget::item:hover { background-color: #e2e5f0; }
QHeaderView::section {
    background-color: #e8eaf2;
    color: #4a4e63;
    border: none;
    border-bottom: 2px solid #4a6cf7;
    padding: 6px 8px;
    font-weight: 600;
}
QTabWidget::pane { border: 1px solid #d0d3e0; border-radius: 8px; }
QTabBar::tab {
    background: transparent;
    padding: 6px 16px;
    color: #5a5e72;
}
QTabBar::tab:selected {
    color: #22242e;
    border-bottom: 2px solid #4a6cf7;
}
QPlainTextEdit {
    background-color: #ffffff;
    border: 1px solid #d0d3e0;
    border-radius: 8px;
    color: #22242e;
    selection-background-color: #4a6cf7;
}
QLineEdit {
    background-color: #ffffff;
    border: 1px solid #c9cddd;
    border-radius: 6px;
    padding: 6px 10px;
    color: #22242e;
}
QLineEdit:focus { border-color: #4a6cf7; }
QScrollBar:vertical { background: transparent; width: 10px; }
QScrollBar::handle:vertical {
    background: #c3c7d6; border-radius: 5px; min-height: 24px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QLabel { color: #44485c; }
)QSS";

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("PDF Extractor");
    outDir_ = QDir::currentPath();

    table_ = new QTableWidget(this);
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels(
        {"#", "Type", "Subtype", "Filter", "Size", "Offset"});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setStretchLastSection(true);

    preview_ = new CodeEditor(this);
    hexView_ = new CodeEditor(this);

    imageLabel_ = new QLabel(this);
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageScroll_ = new QScrollArea(this);
    imageScroll_->setWidget(imageLabel_);
    imageScroll_->setWidgetResizable(true);
    imageLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
    imageScroll_->hide();

    structView_ = new QTreeWidget(this);
    structView_->setHeaderLabels({"Key", "Value"});
    structView_->hide();

    previewTabs_ = new QTabWidget(this);
    previewTabs_->setTabPosition(QTabWidget::South);
    previewTabs_->addTab(preview_, "Text");
    previewTabs_->addTab(hexView_, "Hex");
    previewTabs_->addTab(imageScroll_, "Image");
    previewTabs_->addTab(structView_, "Structure");
    previewTabs_->setTabEnabled(2, false);
    previewTabs_->setTabEnabled(3, false);

    info_ = new CodeEditor(this);

    command_ = new QLineEdit(this);
    command_->setPlaceholderText(
        "command: list, info, preview <n>, extract <n>, extractall, out <dir>");

    console_ = new QPlainTextEdit(this);
    console_->setReadOnly(true);
    console_->setMaximumBlockCount(2000);

    status_ = new QLabel(this);
    status_->setText("ready");

    auto* tabs = new QTabWidget(this);
    tabs->addTab(previewTabs_, "Preview");
    tabs->addTab(info_, "Info");
    tabs->addTab(console_, "Console");

    auto* cols = new QHBoxLayout;
    cols->addWidget(table_, 3);
    cols->addWidget(tabs, 2);

    auto* fileMenu = menuBar()->addMenu("&File");
    QAction* openAct = fileMenu->addAction("&Open PDF…", this,
                                          &MainWindow::openPdf);
    openAct->setShortcut(QKeySequence::Open);
    fileMenu->addAction("&Export All Images…", this,
                        &MainWindow::exportAllImages);
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", qApp, &QApplication::quit);
    auto* extractMenu = menuBar()->addMenu("E&xtract");
    extractMenu->addAction("Extract &Selected…", this,
                           &MainWindow::extractSelected);
    auto* viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction("Toggle &Dark / Light", this,
                        &MainWindow::toggleTheme);

    auto* cmdRow = new QHBoxLayout;
    cmdRow->addWidget(new QLabel(QString::fromUtf8("\u203a"), this));
    cmdRow->addWidget(command_);

    auto* root = new QVBoxLayout;
    root->addLayout(cols, 1);
    root->addLayout(cmdRow);
    root->addWidget(status_);

    auto* central = new QWidget(this);
    central->setLayout(root);
    setCentralWidget(central);

    connect(command_, &QLineEdit::returnPressed, this, &MainWindow::runCommand);
    connect(table_, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::onRowChanged);
    connect(table_, &QTableWidget::itemActivated,
            [this](QTableWidgetItem*) { onRowChanged(); });

    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showTableMenu);
    preview_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(preview_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showPreviewMenu);
    imageLabel_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(imageLabel_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showImageMenu);
    imageLabel_->installEventFilter(this);

    applyStyle();
    resize(1100, 640);
}

void MainWindow::applyStyle() {
    setStyleSheet(dark_ ? kDarkStyle : kLightStyle);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    preview_->setLineNumberColors(dark_ ? QColor(0x2d, 0x2f, 0x43)
                                        : QColor(0xe8, 0xea, 0xf2),
                                  dark_ ? QColor(0x6f, 0x72, 0x8f)
                                        : QColor(0x5a, 0x5e, 0x72));
    info_->setLineNumberColors(dark_ ? QColor(0x2d, 0x2f, 0x43)
                                     : QColor(0xe8, 0xea, 0xf2),
                               dark_ ? QColor(0x6f, 0x72, 0x8f)
                                     : QColor(0x5a, 0x5e, 0x72));
}

void MainWindow::toggleTheme() {
    dark_ = !dark_;
    applyStyle();
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

void MainWindow::exportAllImages() {
    if (pdf_.objects.empty()) {
        status_->setText("no file loaded");
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Export all images to directory", outDir_);
    if (dir.isEmpty()) return;
    outDir_ = dir;
    int saved = 0;
    for (const auto& o : pdf_.objects) {
        if (!o.isStream || o.subtype != "Image") continue;
        const QImage img = renderObjectImage(pdf_, o);
        if (img.isNull()) {
            log("obj " + QString::number(o.id) + ": image render failed");
            continue;
        }
        QString fmt = "PNG";
        QString ext = "png";
        for (const auto& f : o.filters) {
            if (f == "DCTDecode") {
                fmt = "JPEG";
                ext = "jpg";
                break;
            }
            if (f == "JPXDecode") {
                fmt = "JPEG2000";
                ext = "jp2";
                break;
            }
        }
        const QString path =
            dir + "/image_" + QString::number(o.id) + "." + ext;
        if (img.save(path, fmt.toLatin1().constData())) {
            ++saved;
            log("exported obj " + QString::number(o.id) + " -> " + path);
        } else {
            log("obj " + QString::number(o.id) + ": save failed");
        }
    }
    status_->setText(QString("exported %1 image(s)").arg(saved));
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
    contextObjId_ = o.id;

    // Raw source text for every object kind.
    std::string src;
    const bool haveSource = pdf_.readObjectSource(o, src);

    // Text tab: decoded stream (text streams) or raw object source (others).
    preview_->clear();
    previewTabs_->setTabEnabled(2, false);
    previewTabs_->setTabEnabled(3, false);
    previewTabs_->setTabVisible(3, false);
    previewTabs_->setTabVisible(2, false);

    if (o.isStream) {
        std::string decoded;
        if (pdf_.readStreamDecoded(o, decoded)) {
            preview_->setPlainText(
                QString::fromStdString(makePreview(decoded, 8192, false)));
            hexView_->setPlainText(hexDump(decoded, 65536));
        } else {
            std::string raw;
            if (pdf_.readStream(o, raw)) {
                preview_->setPlainText(
                    "[decode failed, showing raw bytes]\n" +
                    QString::fromStdString(makePreview(raw, 8192, false)));
                hexView_->setPlainText(hexDump(raw, 65536));
            } else {
                preview_->setPlainText("could not read stream data");
            }
        }
        if (o.subtype == "Image") {
            QImage img = renderObjectImage(pdf_, o);
            if (!img.isNull()) {
                previewTabs_->setTabVisible(2, true);
                previewTabs_->setTabEnabled(2, true);
                previewTabs_->setCurrentWidget(imageScroll_);
                imagePixmap_ = QPixmap::fromImage(img);
                updateImageLabel();
            }
        }
    } else {
        preview_->setPlainText(
            haveSource ? QString::fromStdString(src)
                       : QString("object %1 is not a stream (type %2); "
                                 "no source text found")
                             .arg(o.id)
                             .arg(QString::fromStdString(o.type)));
    }

    // Structure tab mirrors the source syntax tree for dict/array sources.
    structView_->clear();
    QString type = QString::fromStdString(o.type);
    QString subtype = QString::fromStdString(o.subtype);
    auto insertPair = [&](const QString& k, const QString& v) {
        auto* it = new QTreeWidgetItem(structView_);
        it->setText(0, k);
        it->setText(1, v);
        structView_->addTopLevelItem(it);
    };
    if (o.isStream) {
        insertPair("Object", QString::number(o.id));
        insertPair("Type", "stream");
        if (type != "stream") insertPair("Declared /Type", type);
        if (!subtype.isEmpty()) insertPair("Subtype", subtype);
        insertPair("Length", QString::number(o.rawLength));
        insertPair("Filters", filterText(o));
        if (o.width > 0) insertPair("Width", QString::number(o.width));
        if (o.height > 0) insertPair("Height", QString::number(o.height));
        if (o.bitsPerComponent > 0)
            insertPair("BitsPerComponent", QString::number(o.bitsPerComponent));
        if (!o.colorspace.empty())
            insertPair("ColorSpace", QString::fromStdString(o.colorspace));
        insertPair("Predictor", QString::number(o.predictor));
        previewTabs_->setTabVisible(3, true);
        previewTabs_->setTabEnabled(3, true);
    } else {
        // Non-stream object: show the scalar value (name/number/string/dict).
        insertPair("Object", QString::number(o.id));
        insertPair("Type", type);
        previewTabs_->setTabVisible(3, true);
        previewTabs_->setTabEnabled(3, true);
        if (haveSource && !src.empty()) {
            insertPair("Source", QString::fromStdString(src).trimmed());
        }
    }
}

void MainWindow::updateImageLabel() {
    if (imagePixmap_.isNull()) return;
    const QSize prev = imageLabel_->size();
    const QSize want = prev.isEmpty()
                           ? imagePixmap_.size()
                           : imagePixmap_.size().scaled(
                                 prev, Qt::KeepAspectRatio);
    imageLabel_->setPixmap(
        imagePixmap_.scaled(want, Qt::KeepAspectRatio,
                            Qt::SmoothTransformation));
}

void MainWindow::showTableMenu(const QPoint& pos) {
    QTableWidgetItem* item = table_->itemAt(pos);
    if (!item) return;
    contextObjId_ = item->data(Qt::UserRole).toInt();
    table_->setCurrentItem(item);
    QMenu menu(this);
    menu.addAction("Save raw stream…", this, &MainWindow::saveContextObject);
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void MainWindow::showPreviewMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.addAction("Save shown text…", this, &MainWindow::savePreviewText);
    menu.addAction("Save raw stream…", this, &MainWindow::saveContextObject);
    menu.exec(preview_->viewport()->mapToGlobal(pos));
}

void MainWindow::showImageMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.addAction("Save image…", this, &MainWindow::saveDisplayedImage);
    menu.exec(imageLabel_->mapToGlobal(pos));
}

void MainWindow::saveDisplayedImage() {
    if (imagePixmap_.isNull()) return;
    const QString path = QFileDialog::getSaveFileName(
        this, "Save image", outDir_ + "/image.png", "PNG image (*.png)");
    if (path.isEmpty()) return;
    if (imagePixmap_.save(path)) {
        log("saved image -> " + path);
        status_->setText("saved image");
    }
}

void MainWindow::savePreviewText() {
    const QString path = QFileDialog::getSaveFileName(
        this, "Save text", outDir_ + "/preview.txt", "Text files (*.txt)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(preview_->toPlainText().toUtf8());
        log("saved text -> " + path);
        status_->setText("saved text");
    }
}

void MainWindow::saveContextObject() {
    int id = contextObjId_;
    if (id <= 0) {
        const int row = table_->currentRow();
        if (row >= 0 && row < table_->rowCount() &&
            table_->item(row, 0))
            id = table_->item(row, 0)->data(Qt::UserRole).toInt();
    }
    if (id <= 0) return;
    const Object* found = nullptr;
    for (const auto& o : pdf_.objects) {
        if (o.id == id) {
            found = &o;
            break;
        }
    }
    if (!found) return;
    const QString base = "obj_" + QString::number(id)
                             + "." + QString::fromStdString(found->subtype);
    const QString path = QFileDialog::getSaveFileName(
        this, "Save raw stream", outDir_ + "/" + base);
    if (path.isEmpty()) return;
    std::string bytes;
    const bool ok = pdf_.readStream(*found, bytes);
    if (!ok) {
        status_->setText("object " + QString::number(id) + " has no raw stream");
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        status_->setText("cannot write " + path);
        return;
    }
    f.write(bytes.data(), static_cast<qint64>(bytes.size()));
    log(QString("saved raw stream obj %1 -> %2").arg(id).arg(path));
    status_->setText("saved obj " + QString::number(id));
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == imageLabel_ && event->type() == QEvent::Resize)
        updateImageLabel();
    return QMainWindow::eventFilter(obj, event);
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
    }, false);

    if (lower.startsWith("open ") && !pdf_.objects.empty()) fillTable();
    if (rc == 2) close();
}

void MainWindow::log(const QString& text) {
    console_->appendPlainText(text);
}

}  // namespace pdfx