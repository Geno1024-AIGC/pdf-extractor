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
#include <QPainter>
#include <QPainterPath>
#include <QPen>
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
#include <QVariant>
#include <QVBoxLayout>

#include <cstdlib>
#include <functional>
#include <set>

#include "cli.h"
#include "extractor.h"
#include "pagediagram.h"

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

// Build a hex dump of `bytes`, capped at `maxBytes`. Pure byte stream, no
// offset column so the gutter line numbers stay the only frame of reference.
QString hexDump(const std::string& bytes, size_t maxBytes) {
    QString out;
    size_t shown = 0;
    for (unsigned char c : bytes) {
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

// --- Minimal PDF dict/array scanner for the structure view ------------------
// Tokenizes an object's source into a QTreeWidget tree. Recognises names,
// numbers, dicts, arrays, strings, booleans and "N G R" references. Coarse
// enough to be tolerant of the parser's sloppier object boundaries.
struct PdfToken {
    size_t end = 0;  // scan position after the token
    enum Kind { Name, Number, String, Boolean, Ref, DictOpen, DictClose,
                ArrayOpen, ArrayClose, Keyword, Other, End } kind = End;
    std::string text;   // name without '/' or raw text
    long long refId = 0;
};
// Caller parses at least the object body; scan of tokens is pulled through
// this cursor so nested structures can advance it.
using PdfCursor = size_t;

PdfToken nextPdfToken(const std::string& s, PdfCursor& c) {
    PdfToken t;
    const size_t n = s.size();
    // whitespace and comments
    while (c < n && (s[c] <= ' ' || s[c] == '%')) {
        if (s[c] == '%') {
            while (c < n && s[c] != '\n' && s[c] != '\r') ++c;
        } else {
            ++c;
        }
    }
    if (c >= n) {
        t.kind = PdfToken::End;
        t.end = c;
        return t;
    }
    const char ch = s[c];
    if (ch == '/') {
        size_t p = c + 1;
        while (p < n && s[p] > ' ' &&
               s[p] != '/' && s[p] != '(' && s[p] != ')' &&
               s[p] != '[' && s[p] != ']' && s[p] != '<' && s[p] != '>')
            ++p;
        t.kind = PdfToken::Name;
        t.text = s.substr(c + 1, p - c - 1);
        t.end = p;
        return t;
    }
    if (ch == '<') {
        if (c + 1 < n && s[c + 1] == '<') {
            t.kind = PdfToken::DictOpen;
            t.end = c + 2;
        } else {
            // hex string <...>
            size_t p = s.find('>', c + 1);
            t.kind = PdfToken::String;
            t.text = "hex<" +
                     (p == std::string::npos
                          ? s.substr(c + 1)
                          : s.substr(c + 1, p - c - 1)) + ">";
            t.end = p == std::string::npos ? n : p + 1;
        }
        return t;
    }
    if (ch == '>') {
        const bool close = c + 1 < n && s[c + 1] == '>';
        t.kind = close ? PdfToken::DictClose : PdfToken::Other;
        t.end = c + (close ? 2 : 1);
        return t;
    }
    if (ch == '[') {
        t.kind = PdfToken::ArrayOpen;
        t.end = c + 1;
        return t;
    }
    if (ch == ']') {
        t.kind = PdfToken::ArrayClose;
        t.end = c + 1;
        return t;
    }
    if (ch == '(') {
        size_t depth = 1;
        size_t p = c + 1;
        while (p < n && depth) {
            if (s[p] == '\\') p += 2;
            else if (s[p] == '(') {
                ++depth;
                ++p;
            } else if (s[p] == ')') {
                --depth;
                ++p;
            } else ++p;
        }
        t.kind = PdfToken::String;
        t.text = s.substr(c, std::min(p, n) - c);
        t.end = std::min(p, n);
        return t;
    }
    if (ch == '+' || ch == '-' || (ch >= '0' && ch <= '9') ||
        ch == '.' ) {
        size_t p = c;
        // collect "N G R" as a reference if followed by two ints + R
        auto isInt = [&](size_t pos, size_t& after) {
            while (pos < n && s[pos] == ' ') ++pos;
            if (pos >= n || !(s[pos] == '-' || (s[pos] >= '0' && s[pos] <= '9')))
                return false;
            after = pos + 1;
            while (after < n && (s[after] == '-' ||
                   (s[after] >= '0' && s[after] <= '9')))
                ++after;
            return true;
        };
        size_t numEnd = p;
        while (numEnd < n && (s[numEnd] == '+' || s[numEnd] == '-' ||
               s[numEnd] == '.' || (s[numEnd] >= '0' && s[numEnd] <= '9')))
            ++numEnd;
        const std::string first = s.substr(p, numEnd - p);
        if (!first.empty() && first.find('.') == std::string::npos) {
            size_t a1 = numEnd, a2 = 0;
            if (isInt(a1, a2)) {
                size_t gEnd = a2;
                while (gEnd < n && s[gEnd] == ' ') ++gEnd;
                size_t rEnd = gEnd;
                while (rEnd < n && (s[rEnd] == '-' ||
                       (s[rEnd] >= '0' && s[rEnd] <= '9'))) ++rEnd;
                if (rEnd < n && s[rEnd] == 'R') {
                    long long id = 0;
                    try { id = std::stoll(first); } catch (...) { }
                    t.kind = PdfToken::Ref;
                    t.refId = id;
                    t.text = first + " 0 R";
                    t.end = rEnd + 1;
                    return t;
                }
            }
        }
        t.kind = PdfToken::Number;
        t.text = first;
        t.end = numEnd;
        return t;
    }
    if (ch >= 'a' && ch <= 'z') {
        size_t p = c;
        while (p < n && ((s[p] >= 'a' && s[p] <= 'z') ||
               (s[p] >= 'A' && s[p] <= 'Z'))) ++p;
        t.kind = PdfToken::Keyword;
        t.text = s.substr(c, p - c);
        t.end = p;
        return t;
    }
    t.kind = PdfToken::Other;
    t.end = c + 1;
    return t;
}

// Populate `parent` with the structure of a dict/array starting at `c`.
// Returns the cursor after the consumed structure. `onRef` is invoked for
// every "N 0 R" reference found (id and the created item).
using RefCallback = std::function<void(long long, QTreeWidgetItem*)>;

void buildPdfValue(const std::string& s, PdfCursor& c, QTreeWidgetItem* parent,
                   const RefCallback& onRef) {
    auto addItem = [&](const QString& key, const QString& value) {
        auto* it = new QTreeWidgetItem(parent);
        it->setText(0, key);
        it->setText(1, value);
        parent->addChild(it);
        return it;
    };
    PdfToken t = nextPdfToken(s, c);
    c = t.end;
    if (t.kind == PdfToken::DictOpen) {
        while (true) {
            PdfToken k = nextPdfToken(s, c);
            c = k.end;
            if (k.kind == PdfToken::DictClose || k.kind == PdfToken::End) break;
            if (k.kind != PdfToken::Name) continue;  // tolerate junk
            // value
            PdfToken v = nextPdfToken(s, c);
            c = v.end;
            if (v.kind == PdfToken::DictOpen) {
                auto* sub = addItem("/" + QString::fromStdString(k.text), "dict");
                c = v.end - 2;  // rewind onto "<<"
                buildPdfValue(s, c, sub, onRef);
                continue;
            }
            if (v.kind == PdfToken::ArrayOpen) {
                auto* sub = addItem("/" + QString::fromStdString(k.text),
                                    "array");
                c = v.end - 1;  // rewind onto "["
                buildPdfValue(s, c, sub, onRef);
                continue;
            }
            if (v.kind == PdfToken::Ref) {
                auto* it =
                    addItem("/" + QString::fromStdString(k.text),
                            QString("%1 0 R").arg(v.refId));
                onRef(v.refId, it);
                continue;
            }
            QString val;
            switch (v.kind) {
                case PdfToken::Name:
                    val = "/" + QString::fromStdString(v.text);
                    break;
                case PdfToken::Number:
                    val = QString::fromStdString(v.text);
                    break;
                case PdfToken::String:
                    val = QString::fromStdString(v.text);
                    break;
                case PdfToken::Keyword:
                    val = QString::fromStdString(v.text);
                    break;
                default:
                    val = "?";
                    break;
            }
            addItem("/" + QString::fromStdString(k.text), val);
        }
        return;
    }
    if (t.kind == PdfToken::ArrayOpen) {
        int idx = 0;
        while (true) {
            PdfToken k = nextPdfToken(s, c);
            c = k.end;
            if (k.kind == PdfToken::ArrayClose || k.kind == PdfToken::End)
                break;
            const QString key = QString::number(idx);
            if (k.kind == PdfToken::DictOpen) {
                auto* sub = addItem(key, "dict");
                c = k.end - 2;  // rewind onto "<<"
                buildPdfValue(s, c, sub, onRef);
                ++idx;
                continue;
            }
            if (k.kind == PdfToken::ArrayOpen) {
                auto* sub = addItem(key, "array");
                c = k.end - 1;  // rewind onto "["
                buildPdfValue(s, c, sub, onRef);
                ++idx;
                continue;
            }
            if (k.kind == PdfToken::Ref) {
                auto* it =
                    addItem(key, QString("%1 0 R").arg(k.refId));
                onRef(k.refId, it);
                ++idx;
                continue;
            }
            QString val;
            switch (k.kind) {
                case PdfToken::Name: val = "/" + QString::fromStdString(k.text); break;
                case PdfToken::Number: val = QString::fromStdString(k.text); break;
                case PdfToken::String: val = QString::fromStdString(k.text); break;
                case PdfToken::Keyword: val = QString::fromStdString(k.text); break;
                default: val = "?"; break;
            }
            addItem(key, val);
            ++idx;
        }
        // Compact inline summary on the array node ("[0 0 400 600]").
        QString summary = "[";
        for (int i = 0; i < parent->childCount(); ++i) {
            const QString part = parent->child(i)->text(1);
            if (summary.size() + part.size() > 72) {
                summary += " …";
                break;
            }
            if (i) summary += " ";
            summary += part;
        }
        summary += "]";
        if (!parent->childCount()) summary = "[]";
        parent->setText(1, summary);
        return;
    }
}

// Find the first "<<" or "[" in object source and build its tree.
void buildObjectTree(const std::string& src, QTreeWidget* tree,
                     const RefCallback& onRef) {
    tree->clear();
    // locate start of dict or array body
    PdfCursor c = 0;
    while (c < src.size()) {
        PdfToken t = nextPdfToken(src, c);
        if (t.kind == PdfToken::DictOpen || t.kind == PdfToken::ArrayOpen) break;
        c = t.end;
    }
    auto* root = new QTreeWidgetItem(tree, QStringList{"Root"});
    tree->addTopLevelItem(root);
    buildPdfValue(src, c, root, onRef);
    root->setExpanded(true);
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

    pageDiagram_ = new PageDiagram(this);
    pageDiagram_->hide();
    pageBox_ = new QWidget(this);
    auto* pageBoxLay = new QVBoxLayout(pageBox_);
    pageBoxLay->setContentsMargins(0, 0, 0, 0);
    pageBoxLay->addWidget(pageDiagram_);

    previewTabs_ = new QTabWidget(this);
    previewTabs_->setTabPosition(QTabWidget::South);
    previewTabs_->addTab(structView_, "Structure");
    previewTabs_->addTab(preview_, "Text");
    previewTabs_->addTab(hexView_, "Hex");
    previewTabs_->addTab(pageBox_, "Page");
    previewTabs_->addTab(imageScroll_, "Image");
    previewTabs_->setTabVisible(3, false);
    previewTabs_->setTabEnabled(3, false);
    previewTabs_->setCurrentIndex(0);

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
    connect(structView_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* it, int) { gotoRefItem(it); });

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
    previewTabs_->setTabEnabled(4, false);
    previewTabs_->setTabVisible(4, false);

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
                previewTabs_->setTabVisible(4, true);
                previewTabs_->setTabEnabled(4, true);
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

    // Structure tab: parse the object source into a key/value tree with
    // clickable references, plus a page diagram for /Type /Page objects.
    structView_->clear();
    pageDiagram_->hide();
    previewTabs_->setTabVisible(3, false);
    previewTabs_->setTabEnabled(3, false);
    if (haveSource && !src.empty()) {
        buildObjectTree(src, structView_,
                        [this](long long id, QTreeWidgetItem* it) {
                            it->setData(0, Qt::UserRole,
                                        QVariant(static_cast<qulonglong>(id)));
                            QFont f = it->font(1);
                            f.setUnderline(true);
                            it->setFont(1, f);
                            if (dark_) it->setForeground(1, QColor(0x7c, 0x9c, 0xff));
                            else it->setForeground(1, QColor(0x2a, 0x50, 0xe0));
                            it->setToolTip(1,
                                           QString("object %1 — click to jump")
                                               .arg(id));
                        });
        // Detect /Type /Page and /Type /Pages for the diagram. Scan past any
        // "N G obj" header tokens to the first dict, collect /Kids, then walk
        // the /Parent chain so /Page inherits /MediaBox and /Rotate.
        PdfCursor c = 0;
        std::string type;
        std::vector<int> kids;
        long long parentRef = 0;
        int countDir = -1;       // /Count of the Pages node, if present
        double mb[4] = {0, 0, 612, 792};
        int rotate = 0;
        while (c < src.size()) {
            PdfToken t = nextPdfToken(src, c);
            if (t.kind == PdfToken::DictOpen) break;
            if (t.kind == PdfToken::End) break;
            c = t.end;
        }
        while (c < src.size()) {
            PdfToken t = nextPdfToken(src, c);
            if (t.kind == PdfToken::End || t.kind == PdfToken::DictClose)
                break;
            c = t.end;
            if (t.kind != PdfToken::Name) continue;
            const std::string key2 = t.text;
            if (key2 == "Type") {
                PdfToken v = nextPdfToken(src, c);
                c = v.end;
                if (v.kind == PdfToken::Name) type = v.text;
            } else if (key2 == "Count") {
                PdfToken v = nextPdfToken(src, c);
                c = v.end;
                try { countDir = std::stoi(v.text); } catch (...) { }
            } else if (key2 == "Rotate") {
                PdfToken v = nextPdfToken(src, c);
                c = v.end;
                try { rotate = std::stoi(v.text); } catch (...) { }
            } else if (key2 == "Parent") {
                PdfToken v = nextPdfToken(src, c);
                c = v.end;
                if (v.kind == PdfToken::Ref) parentRef = v.refId;
            } else if (key2 == "MediaBox") {
                PdfToken v = nextPdfToken(src, c);
                c = v.end;
                if (v.kind != PdfToken::ArrayOpen) continue;
                int vals = 0;
                while (vals < 4) {
                    PdfToken e = nextPdfToken(src, c);
                    c = e.end;
                    if (e.kind == PdfToken::ArrayClose ||
                        e.kind == PdfToken::End)
                        break;
                    try { mb[vals] = std::stod(e.text); } catch (...) { mb[vals] = 0; }
                    ++vals;
                }
            } else if (key2 == "Kids") {
                PdfToken v = nextPdfToken(src, c);
                c = v.end;
                if (v.kind != PdfToken::ArrayOpen) continue;
                while (true) {
                    PdfToken e = nextPdfToken(src, c);
                    c = e.end;
                    if (e.kind == PdfToken::ArrayClose ||
                        e.kind == PdfToken::End) break;
                    if (e.kind == PdfToken::Ref)
                        kids.push_back(static_cast<int>(e.refId));
                }
            }
        }
        bool isPage = type == "Page";
        bool isPages = type == "Pages";
        bool hasOwnBox = mb[2] != 0 || mb[3] != 0;
        if (!hasOwnBox && isPage && parentRef > 0) {
            // /Page usually omits MediaBox/Rotate; they live on the /Pages
            // ancestor. Follow the chain to inherit.
            std::set<long long> seen;
            long long up = parentRef;
            while (up > 0 && seen.insert(up).second) {
                bool found = false;
                for (const Object& po : pdf_.objects) {
                    if (po.id != up) continue;
                    found = true;
                    std::string psrc;
                    if (po.isStream || !pdf_.readObjectSource(po, psrc))
                        break;
                    PdfCursor q = 0;
                    double pband[2] = {0, 0};
                    int prot = 0;
                    long long pparent = 0;
                    bool gotBox = false;
                    while (q < psrc.size()) {
                        PdfToken u = nextPdfToken(psrc, q);
                        if (u.kind == PdfToken::DictOpen) break;
                        if (u.kind == PdfToken::End) break;
                        q = u.end;
                    }
                    while (q < psrc.size()) {
                        PdfToken u = nextPdfToken(psrc, q);
                        if (u.kind == PdfToken::End ||
                            u.kind == PdfToken::DictClose)
                            break;
                        q = u.end;
                        if (u.kind != PdfToken::Name) continue;
                        const std::string pk = u.text;
                        if (pk == "Rotate") {
                            PdfToken v = nextPdfToken(psrc, q);
                            q = v.end;
                            try { prot = std::stoi(v.text); } catch (...) { }
                        } else if (pk == "Parent") {
                            PdfToken v = nextPdfToken(psrc, q);
                            q = v.end;
                            if (v.kind == PdfToken::Ref) pparent = v.refId;
                        } else if (pk == "MediaBox") {
                            PdfToken v = nextPdfToken(psrc, q);
                            q = v.end;
                            if (v.kind != PdfToken::ArrayOpen) continue;
                            int vals = 0;
                            while (vals < 2) {
                                PdfToken e = nextPdfToken(psrc, q);
                                q = e.end;
                                if (e.kind == PdfToken::ArrayClose ||
                                    e.kind == PdfToken::End)
                                    break;
                                try { pband[vals] = std::stod(e.text); }
                                catch (...) { pband[vals] = 0; }
                                ++vals;
                            }
                            gotBox = true;
                        }
                    }
                    if (gotBox) {
                        mb[0] = 0; mb[1] = 0;
                        mb[2] = pband[0]; mb[3] = pband[1];
                        if (rotate == 0) rotate = prot;
                        break;
                    }
                    up = pparent;
                    break;
                }
                if (!found) break;
            }
        }
        if (isPage || isPages) {
            if (isPage) {
                pageDiagram_->setBox(mb[2] - mb[0], mb[3] - mb[1], rotate,
                                     QString("Page %1  pt").arg(o.id));
            } else {
                // Walk the whole subtree: from this /Pages node descend through
                // /Kids (page or nested Pages nodes) and collect every /Page.
                std::vector<int> pageIds;
                std::function<void(long long)> walk = [&](long long cur) {
                    for (const Object& po : pdf_.objects) {
                        if (po.id != cur) continue;
                        std::string psrc;
                        if (po.isStream || !pdf_.readObjectSource(po, psrc))
                            return;
                        PdfCursor q = 0;
                        std::string ptype;
                        std::vector<int> pkids;
                        while (q < psrc.size()) {
                            PdfToken u = nextPdfToken(psrc, q);
                            if (u.kind == PdfToken::DictOpen) break;
                            if (u.kind == PdfToken::End) break;
                            q = u.end;
                        }
                        while (q < psrc.size()) {
                            PdfToken u = nextPdfToken(psrc, q);
                            if (u.kind == PdfToken::End ||
                                u.kind == PdfToken::DictClose)
                                break;
                            q = u.end;
                            if (u.kind != PdfToken::Name) continue;
                            const std::string pk = u.text;
                            if (pk == "Type") {
                                PdfToken v = nextPdfToken(psrc, q);
                                q = v.end;
                                if (v.kind == PdfToken::Name) ptype = v.text;
                            } else if (pk == "Kids") {
                                PdfToken v = nextPdfToken(psrc, q);
                                q = v.end;
                                if (v.kind != PdfToken::ArrayOpen) continue;
                                while (true) {
                                    PdfToken e = nextPdfToken(psrc, q);
                                    q = e.end;
                                    if (e.kind == PdfToken::ArrayClose ||
                                        e.kind == PdfToken::End)
                                        break;
                                    if (e.kind == PdfToken::Ref)
                                        pkids.push_back(
                                            static_cast<int>(e.refId));
                                }
                            }
                        }
                        if (ptype == "Page") {
                            pageIds.push_back(static_cast<int>(cur));
                        } else {
                            for (int kid : pkids) walk(kid);
                        }
                        return;
                    }
                };
                for (int kid : kids) walk(kid);
                if (pageIds.empty() && countDir > 0) {
                    // Fall back to /Count if the tree could not be walked.
                    for (int i = 0; i < countDir; ++i)
                        pageIds.push_back(static_cast<int>(o.id));
                }
                pageDiagram_->setPageTree(static_cast<int>(pageIds.size()),
                                          pageIds,
                                          QString("Pages tree  obj %1")
                                              .arg(o.id));
            }
            // Show the diagram in its own Page tab.
            pageDiagram_->show();
            previewTabs_->setTabVisible(3, true);
            previewTabs_->setTabEnabled(3, true);
            previewTabs_->setCurrentWidget(pageBox_);
        }
    }
    previewTabs_->setTabEnabled(0, true);
}

void MainWindow::gotoRefItem(QTreeWidgetItem* item) {
    if (!item) return;
    const QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid()) return;
    const int id = v.toInt();
    for (int r = 0; r < table_->rowCount(); ++r) {
        if (table_->item(r, 0) &&
            table_->item(r, 0)->data(Qt::UserRole).toInt() == id) {
            table_->setCurrentCell(r, 0);
            return;
        }
    }
    status_->setText("referenced object " + QString::number(id) +
                     " not found in scan");
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