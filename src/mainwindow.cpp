#include "mainwindow.h"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
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
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPlainTextEdit>
#include <QPoint>
#include <QProgressDialog>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QVBoxLayout>
#include <QUrl>

#include <cstdlib>
#include <algorithm>
#include <functional>
#include <map>
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
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
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

// Parse the first dict of object source `s` into a map of top-level key ->
// value tokens. Nested dicts/arrays inside values are consumed whole; only a
// value's direct Name/Number/Boolean/Ref/Keyword tokens are kept, so callers
// can read /Type, /Count, /Kids refs, /MediaBox numbers, etc. even when those
// keys come after a nested structure (e.g. /Resources << ... >>) in the dict.
std::map<std::string, std::vector<PdfToken>>
parseTopDict(const std::string& s) {
    std::map<std::string, std::vector<PdfToken>> m;
    PdfCursor c = 0;
    PdfToken t = nextPdfToken(s, c);
    c = t.end;
    while (t.kind != PdfToken::DictOpen && t.kind != PdfToken::End) {
        t = nextPdfToken(s, c);
        c = t.end;
    }
    if (t.kind != PdfToken::DictOpen) return m;
    while (true) {
        PdfToken k = nextPdfToken(s, c);
        c = k.end;
        if (k.kind == PdfToken::DictClose || k.kind == PdfToken::End) break;
        if (k.kind != PdfToken::Name) continue;
        std::vector<PdfToken>& vv = m[k.text];
        PdfToken v = nextPdfToken(s, c);
        c = v.end;
        if (v.kind == PdfToken::DictOpen) {
            int depth = 1;
            while (depth > 0) {
                PdfToken u = nextPdfToken(s, c);
                c = u.end;
                if (u.kind == PdfToken::End) break;
                if (u.kind == PdfToken::DictOpen) ++depth;
                else if (u.kind == PdfToken::DictClose) --depth;
            }
            continue;  // nested dict contents are not top-level entries
        }
        if (v.kind == PdfToken::ArrayOpen) {
            int depth = 1;
            while (depth > 0) {
                PdfToken u = nextPdfToken(s, c);
                c = u.end;
                if (u.kind == PdfToken::End) break;
                if (u.kind == PdfToken::ArrayOpen ||
                    u.kind == PdfToken::DictOpen)
                    ++depth;
                else if (u.kind == PdfToken::ArrayClose ||
                         u.kind == PdfToken::DictClose)
                    --depth;
                else if (depth == 1)
                    vv.push_back(u);  // direct array elements
            }
            continue;
        }
        if (v.kind == PdfToken::End) break;
        vv.push_back(v);
    }
    return m;
}

// --- Content-stream scanning and XObject resolution ------------------------
// Parse the first dict of an object and recover every value, *including*
// nested dict/array bodies which parseTopDict deliberately drops. The raw
// substring is kept so the value can be re-parsed with parseTopDict.
struct TopValue {
    enum Kind { None, Ref, Num, Dict, Array, Other } kind = None;
    long long refId = 0;
    std::string raw;
};

std::map<std::string, TopValue> parseTopValues(const std::string& s) {
    std::map<std::string, TopValue> m;
    PdfCursor c = 0;
    PdfToken t = nextPdfToken(s, c);
    c = t.end;
    while (t.kind != PdfToken::DictOpen && t.kind != PdfToken::End) {
        t = nextPdfToken(s, c);
        c = t.end;
    }
    if (t.kind != PdfToken::DictOpen) return m;
    while (true) {
        PdfToken k = nextPdfToken(s, c);
        c = k.end;
        if (k.kind == PdfToken::DictClose || k.kind == PdfToken::End) break;
        if (k.kind != PdfToken::Name) continue;
        const size_t before = c;
        TopValue tv;
        PdfToken v = nextPdfToken(s, c);
        c = v.end;
        if (v.kind == PdfToken::DictOpen || v.kind == PdfToken::ArrayOpen) {
            int depth = 1;
            while (depth > 0) {
                PdfToken u = nextPdfToken(s, c);
                c = u.end;
                if (u.kind == PdfToken::End) break;
                if (u.kind == PdfToken::DictOpen ||
                    u.kind == PdfToken::ArrayOpen)
                    ++depth;
                else if (u.kind == PdfToken::DictClose ||
                         u.kind == PdfToken::ArrayClose)
                    --depth;
            }
            tv.kind =
                v.kind == PdfToken::DictOpen ? TopValue::Dict : TopValue::Array;
            tv.raw = s.substr(before, c - before);
        } else if (v.kind == PdfToken::Ref) {
            tv.kind = TopValue::Ref;
            tv.refId = v.refId;
        } else if (v.kind == PdfToken::Number) {
            tv.kind = TopValue::Num;
            tv.raw = v.text;
        } else if (v.kind != PdfToken::End) {
            tv.kind = TopValue::Other;
            tv.raw = v.text;
        }
        m[k.text] = tv;
    }
    return m;
}

// Textual source of a non-stream object, empty if not found.
std::string objectSource(const PdfFile& pdf, long long id) {
    for (const Object& o : pdf.objects)
        if (o.id == id && !o.isStream) {
            std::string s;
            if (pdf.readObjectSource(o, s)) return s;
        }
    return std::string();
}

// Resolve the `/Resources -> /XObject` chain of a /Page dict and fill `out`
// with each XObject name -> object id, so a `/Name Do` in a content stream
// can be linked to the object it paints.
void resolveXObjectMap(const PdfFile& pdf, const std::string& pgSrc,
                       std::map<std::string, long long>& out) {
    auto pg = parseTopValues(pgSrc);
    auto resIt = pg.find("Resources");
    if (resIt == pg.end()) return;
    std::string resSrc;
    if (resIt->second.kind == TopValue::Ref)
        resSrc = objectSource(pdf, resIt->second.refId);
    else if (resIt->second.kind == TopValue::Dict)
        resSrc = resIt->second.raw;
    if (resSrc.empty()) return;
    auto rv = parseTopValues(resSrc);
    auto xoIt = rv.find("XObject");
    if (xoIt == rv.end()) return;
    std::string xoSrc;
    if (xoIt->second.kind == TopValue::Ref)
        xoSrc = objectSource(pdf, xoIt->second.refId);
    else if (xoIt->second.kind == TopValue::Dict)
        xoSrc = xoIt->second.raw;
    if (xoSrc.empty()) return;
    auto xv = parseTopValues(xoSrc);
    for (const auto& kv : xv)
        if (kv.second.kind == TopValue::Ref) out[kv.first] = kv.second.refId;
}

// A `cm`-transformed region that a `/Name Do` paints over, in user space.
struct ContentBoxHit {
    std::string name;  // XObject name, without '/'
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Token-scan a content stream, track the CTM (`q`/`Q` stack + `cm`), and
// record a box for every `/Name Do` as the bounding rect of the unit square
// transformed through the current matrix.
std::vector<ContentBoxHit> scanContentBoxes(const std::string& content) {
    std::vector<ContentBoxHit> hits;
    PdfCursor c = 0;
    std::vector<double> current = {1, 0, 0, 1, 0, 0};
    std::vector<std::vector<double>> saved;
    std::vector<double> pending;
    std::string lastName;
    while (true) {
        PdfToken t = nextPdfToken(content, c);
        c = t.end;
        if (t.kind == PdfToken::End) break;
        if (t.kind == PdfToken::Number) {
            try { pending.push_back(std::stod(t.text)); }
            catch (...) { }
            continue;
        }
        if (t.kind == PdfToken::Name) {
            lastName = t.text;
            continue;
        }
        if (t.kind != PdfToken::Keyword) continue;
        if (t.text == "q") {
            saved.push_back(current);
        } else if (t.text == "Q") {
            if (!saved.empty()) {
                current = saved.back();
                saved.pop_back();
            }
        } else if (t.text == "cm") {
            const size_t n = pending.size();
            if (n >= 6) {
                const double A = pending[n - 6], B = pending[n - 5],
                             C = pending[n - 4], D = pending[n - 3],
                             E = pending[n - 2], F = pending[n - 1];
                const double a = current[0], b = current[1], cc = current[2],
                             d = current[3], e = current[4], f = current[5];
                current = {a * A + cc * B, b * A + d * B, a * C + cc * D,
                           b * C + d * D, a * E + cc * F + e,
                           b * E + d * F + f};
            }
            pending.clear();
        } else if (t.text == "Do" && !lastName.empty()) {
            const double a = current[0], b = current[1], cc = current[2],
                         d = current[3], e = current[4], f = current[5];
            const double cor[8] = {e,     f, a + e, b + f, cc + e, d + f,
                                   a + cc + e, b + d + f};
            ContentBoxHit h;
            h.name = lastName;
            h.x0 = h.x1 = cor[0];
            h.y0 = h.y1 = cor[1];
            for (int i = 0; i < 4; ++i) {
                h.x0 = std::min(h.x0, cor[i * 2]);
                h.x1 = std::max(h.x1, cor[i * 2]);
                h.y0 = std::min(h.y0, cor[i * 2 + 1]);
                h.y1 = std::max(h.y1, cor[i * 2 + 1]);
            }
            hits.push_back(h);
            lastName.clear();
        }
    }
    return hits;
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
QPushButton#zoomBtn {
    background-color: #2d2f43;
    border: 1px solid #3a3d55;
    border-radius: 6px;
    padding: 0 6px;
    min-width: 26px;
    min-height: 26px;
    font-size: 15px;
    font-weight: 700;
    color: #e8e9f0;
}
QPushButton#zoomBtn:hover {
    background-color: #3a3d55;
    border-color: #565a7d;
}
QPushButton#zoomBtn:pressed { background-color: #23243a; }
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
QProgressDialog {
    background-color: #1e1f29;
    color: #e8e9f0;
}
QProgressBar {
    background-color: #16171f;
    border: 1px solid #2f3150;
    border-radius: 6px;
    text-align: center;
    color: #9aa0c3;
}
QProgressBar::chunk {
    background-color: #4a6cf7;
    border-radius: 5px;
}
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
    background-color: #eef0f6;
    border-color: #d4d8e5;
    color: #a8acbb;
}
QPushButton#zoomBtn {
    background-color: #ffffff;
    border: 1px solid #c9cddd;
    border-radius: 6px;
    padding: 0 6px;
    min-width: 26px;
    min-height: 26px;
    font-size: 15px;
    font-weight: 700;
    color: #22242e;
}
QPushButton#zoomBtn:hover {
    background-color: #eef0f6;
    border-color: #a9aec9;
}
QPushButton#zoomBtn:pressed { background-color: #e2e5f0; }
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
QProgressDialog {
    background-color: #f5f6fa;
    color: #22242e;
}
QProgressBar {
    background-color: #ffffff;
    border: 1px solid #c9cddd;
    border-radius: 6px;
    text-align: center;
    color: #44485c;
}
QProgressBar::chunk {
    background-color: #4a6cf7;
    border-radius: 5px;
}
)QSS";

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("PDF Extractor");
    outDir_ = QDir::currentPath();
    zh_ = QSettings().value("language", "en").toString() == "zh";

    table_ = new QTableWidget(this);
    table_->setColumnCount(7);
    table_->setHorizontalHeaderLabels(
        {"#", "Type", "/Type", "Subtype", "Filter", "Size", "Offset"});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setStretchLastSection(true);

    preview_ = new CodeEditor(this);
    hexView_ = new CodeEditor(this);
    contentView_ = new CodeEditor(this);

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
    pageScroll_ = new QScrollArea(this);
    pageScroll_->setWidget(pageDiagram_);
    pageScroll_->setWidgetResizable(false);
    pageScroll_->setAlignment(Qt::AlignTop);
    pageScroll_->hide();
    pageBox_ = new QWidget(this);
    auto* pageBoxLay = new QVBoxLayout(pageBox_);
    pageBoxLay->setContentsMargins(0, 0, 0, 0);
    auto* zoomRow = new QHBoxLayout;
    QPushButton* zoomInBtn = new QPushButton("+", pageBox_);
    QPushButton* zoomOutBtn = new QPushButton(QString::fromUtf8("\u2212"), pageBox_);
    QPushButton* zoomFitBtn = new QPushButton("1:1", pageBox_);
    zoomInBtn->setObjectName("zoomBtn");
    zoomOutBtn->setObjectName("zoomBtn");
    zoomFitBtn->setObjectName("zoomBtn");
    zoomInBtn->setFixedWidth(30);
    zoomOutBtn->setFixedWidth(30);
    zoomFitBtn->setFixedWidth(48);
    zoomInBtn->setToolTip("Zoom in");
    zoomOutBtn->setToolTip("Zoom out");
    zoomFitBtn->setToolTip("Reset to actual size");
    connect(zoomInBtn, &QPushButton::clicked, this, &MainWindow::zoomIn);
    connect(zoomOutBtn, &QPushButton::clicked, this,
            &MainWindow::zoomOut);
    connect(zoomFitBtn, &QPushButton::clicked, this,
            &MainWindow::zoomOneToOne);
    zoomRow->addWidget(zoomInBtn);
    zoomRow->addWidget(zoomOutBtn);
    zoomRow->addWidget(zoomFitBtn);
    zoomRow->addStretch();
    pageBoxLay->addLayout(zoomRow);
    pageBoxLay->addWidget(pageScroll_);

    previewTabs_ = new QTabWidget(this);
    previewTabs_->setTabPosition(QTabWidget::South);
    previewTabs_->addTab(pageBox_, "Page");
    previewTabs_->addTab(structView_, "Structure");
    previewTabs_->addTab(preview_, "Text");
    previewTabs_->addTab(hexView_, "Hex");
    previewTabs_->addTab(imageScroll_, "Image");
    previewTabs_->addTab(contentView_, "Content");
    previewTabs_->setTabVisible(0, false);
    previewTabs_->setTabEnabled(0, false);
    previewTabs_->setCurrentIndex(1);

    info_ = new CodeEditor(this);

    command_ = new QLineEdit(this);
    command_->setPlaceholderText(
        "command: list, info, preview <n>, extract <n>, extractall, out <dir>");

    console_ = new QPlainTextEdit(this);
    console_->setReadOnly(true);
    console_->setMaximumBlockCount(2000);

    status_ = new QLabel(this);
    status_->setText("ready");
    setAcceptDrops(true);

    outerTabs_ = new QTabWidget(this);
    outerTabs_->addTab(previewTabs_, "Preview");
    outerTabs_->addTab(info_, "Info");
    outerTabs_->addTab(console_, "Console");

    auto* cols = new QHBoxLayout;
    auto* tableCol = new QVBoxLayout;
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(
        tr_("Filter objects…", QString::fromUtf8("过滤对象…")));
    tableCol->addWidget(filterEdit_);
    tableCol->addWidget(table_, 1);
    cols->addLayout(tableCol, 3);
    cols->addWidget(outerTabs_, 2);

    fileMenu_ = menuBar()->addMenu("&File");
    QAction* openAct = fileMenu_->addAction(tr_("&Open PDF…", "打开 PDF…(&O)"),
                                            this, &MainWindow::openPdf);
    openAct->setShortcut(QKeySequence::Open);
    recentMenu_ = fileMenu_->addMenu(tr_("Recent Files", QString::fromUtf8("最近打开")));
    fileMenu_->addSeparator();
    fileMenu_->addAction(tr_("E&xit", "退出(&E)"), qApp,
                         &QApplication::quit);
    updateRecentMenu();
    exportMenu_ = menuBar()->addMenu("&Export");
    exportMenu_->addAction(tr_("&All Images…", "全部图片…(&A)"), this,
                           &MainWindow::exportAllImages);
    exportMenu_->addAction(tr_("&Selected…", "选中项…(&S)"), this,
                           &MainWindow::extractSelected);
    viewMenu_ = menuBar()->addMenu("&View");
    viewMenu_->addAction(tr_("Toggle &Dark / Light", "切换深色 / 浅色(&D)"),
                         this, &MainWindow::toggleTheme);
    settingsMenu_ = menuBar()->addMenu(tr_("&Settings", "设置(&S)"));
    auto* langMenu = settingsMenu_->addMenu(tr_("&Language", "语言(&L)"));
    QActionGroup* langGroup = new QActionGroup(this);
    langGroup->setExclusive(true);
    QAction* enAct = langMenu->addAction("English");
    QAction* zhAct = langMenu->addAction("简体中文");
    enAct->setCheckable(true);
    zhAct->setCheckable(true);
    langGroup->addAction(enAct);
    langGroup->addAction(zhAct);
    (zh_ ? zhAct : enAct)->setChecked(true);
    connect(enAct, &QAction::triggered, this,
            [this] { setLanguage(false); });
    connect(zhAct, &QAction::triggered, this,
            [this] { setLanguage(true); });

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
    connect(filterEdit_, &QLineEdit::textChanged, this,
            &MainWindow::applyFilter);
    connect(table_, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::onRowChanged);
    connect(table_, &QTableWidget::itemActivated,
            [this](QTableWidgetItem*) { onRowChanged(); });
    connect(structView_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* it, int) { gotoRefItem(it); });
    structView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(structView_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showStructMenu);
    connect(pageDiagram_, &PageDiagram::pageClicked, this,
            [this](int id) {
                for (int r = 0; r < table_->rowCount(); ++r) {
                    if (table_->item(r, 0) &&
                        table_->item(r, 0)
                            ->data(Qt::UserRole)
                            .toInt() == id) {
                        table_->setCurrentCell(r, 0);
                        return;
                    }
                }
                status_->setText("page object " + QString::number(id) +
                                 " not found in scan");
            });

    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showTableMenu);
    preview_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(preview_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showPreviewMenu);
    contentView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(contentView_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showPreviewMenu);
    imageLabel_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(imageLabel_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showImageMenu);
    imageLabel_->installEventFilter(this);

    applyStyle();
    retranslate();
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
    contentView_->setLineNumberColors(dark_ ? QColor(0x2d, 0x2f, 0x43)
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

QString MainWindow::tr_(const QString& en, const QString& zh) const {
    return zh_ ? zh : en;
}

void MainWindow::retranslate() {
    setWindowTitle(tr_("PDF Extractor", QString::fromUtf8("PDF 提取器")));
    fileMenu_->setTitle(tr_("&File", "文件(&F)"));
    exportMenu_->setTitle(tr_("&Export", "导出(&E)"));
    viewMenu_->setTitle(tr_("&View", "查看(&V)"));
    if (settingsMenu_)
        settingsMenu_->setTitle(tr_("&Settings", "设置(&S)"));
    if (filterEdit_)
        filterEdit_->setPlaceholderText(
            tr_("Filter objects…", QString::fromUtf8("过滤对象…")));
    if (recentMenu_)
        recentMenu_->setTitle(tr_("Recent Files", QString::fromUtf8("最近打开")));
    for (QAction* a : fileMenu_->actions()) {
        if (a->text().contains("Open PDF")) {
            a->setText(tr_("&Open PDF…", "打开 PDF…(&O)"));
        } else if (a->text().contains("Exit")) {
            a->setText(tr_("E&xit", "退出(&E)"));
        }
    }
    for (QAction* a : exportMenu_->actions()) {
        if (a->text().contains("All Images")) {
            a->setText(tr_("&All Images…", "全部图片…(&A)"));
        } else if (a->text().contains("Selected")) {
            a->setText(tr_("&Selected…", "选中项…(&S)"));
        }
    }
    for (QAction* a : viewMenu_->actions()) {
        if (a->text().contains("Dark")) {
            a->setText(tr_("Toggle &Dark / Light", "切换深色 / 浅色(&D)"));
        }
    }
    previewTabs_->setTabText(0, tr_("Page", "页面"));
    previewTabs_->setTabText(1, tr_("Structure", "结构"));
    previewTabs_->setTabText(2, tr_("Text", "文本"));
    previewTabs_->setTabText(3, tr_("Hex", "十六进制"));
    previewTabs_->setTabText(4, tr_("Image", "图像"));
    previewTabs_->setTabText(5, tr_("Content", "内容"));
    if (outerTabs_) {
        outerTabs_->setTabText(0, tr_("Preview", "预览"));
        outerTabs_->setTabText(1, tr_("Info", "信息"));
        outerTabs_->setTabText(2, tr_("Console", "控制台"));
    }
    status_->setText(tr_("ready", QString::fromUtf8("就绪")));
}

void MainWindow::setLanguage(bool zh) {
    zh_ = zh;
    QSettings s;
    s.setValue("language", zh ? "zh" : "en");
    s.sync();
    retranslate();
}

void MainWindow::openPath(const QString& path) {
    QProgressDialog dlg(
        QString::fromUtf8("Opening %1…").arg(QFileInfo(path).fileName()),
        QString(), 0, 100, this);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setCancelButton(nullptr);
    dlg.setMinimumDuration(400);
    const bool ok = pdf_.load(path.toStdString(), [&dlg](int done, int total) {
        dlg.setValue(total > 0 ? done * 100 / total : 100);
        QCoreApplication::processEvents();
    });
    dlg.setValue(100);
    dlg.close();
    if (!ok) {
        log("open failed: " + QString::fromStdString(pdf_.error));
        status_->setText("failed to open " + QFileInfo(path).fileName());
        return;
    }
    currentFile_ = path;
    setWindowTitle("PDF Extractor - " + QFileInfo(path).fileName());
    {
        QStringList rec = QSettings().value("recent", QStringList())
                              .toStringList();
        rec.removeAll(path);
        rec.prepend(path);
        while (rec.size() > 10) rec.removeLast();
        QSettings s;
        s.setValue("recent", rec);
        s.sync();
    }
    updateRecentMenu();
    fillTable();
    fillInfo();
    // Drop the previous file's inspector state so a fresh load starts clean.
    table_->clearSelection();
    table_->setCurrentCell(-1, -1);
    structView_->clear();
    pageDiagram_->clearDiagram();
    pageScroll_->hide();
    previewTabs_->setTabVisible(0, false);
    previewTabs_->setTabEnabled(0, false);
    preview_->clear();
    hexView_->clear();
    contentView_->clear();
    previewTabs_->setTabVisible(4, false);
    previewTabs_->setTabEnabled(4, false);
    previewTabs_->setTabVisible(5, false);
    previewTabs_->setTabEnabled(5, false);
    imagePixmap_ = QPixmap();
    updateImageLabel();
    previewTabs_->setCurrentIndex(1);
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

void MainWindow::zoomIn() {
    pageDiagram_->setZoom(pageDiagram_->zoom() * 1.15);
}

void MainWindow::zoomOut() {
    pageDiagram_->setZoom(pageDiagram_->zoom() / 1.15);
}

void MainWindow::zoomFit() {
    pageDiagram_->setZoom(1.0);
}

void MainWindow::zoomOneToOne() {
    pageDiagram_->setZoomActual();
}

void MainWindow::applyFilter() {
    const QString q = filterEdit_->text().trimmed().toLower();
    for (int r = 0; r < table_->rowCount(); ++r) {
        bool hit = q.isEmpty();
        if (!hit) {
            for (int c = 0; c < table_->columnCount() && !hit; ++c) {
                QTableWidgetItem* it = table_->item(r, c);
                if (it && it->text().toLower().contains(q)) hit = true;
            }
        }
        table_->setRowHidden(r, !hit);
    }
}

void MainWindow::updateRecentMenu() {
    if (!recentMenu_) return;
    recentMenu_->clear();
    const QStringList rec = QSettings().value("recent", QStringList())
                                .toStringList();
    for (const QString& p : rec) {
        if (p.isEmpty()) continue;
        QAction* a = recentMenu_->addAction(QFileInfo(p).fileName());
        a->setToolTip(p);
        connect(a, &QAction::triggered, this, [this, p] {
            if (p != currentFile_) openPath(p);
        });
    }
    if (recentMenu_->isEmpty()) {
        recentMenu_->setEnabled(false);
        return;
    }
    recentMenu_->addSeparator();
    // Remove one entry.
    QMenu* removeMenu = recentMenu_->addMenu(
        tr_("Remove entry…", QString::fromUtf8("移除一条…")));
    for (const QString& p : rec) {
        if (p.isEmpty()) continue;
        QAction* r = removeMenu->addAction(QFileInfo(p).fileName());
        r->setToolTip(p);
        connect(r, &QAction::triggered, this, [this, p] { removeRecent(p); });
    }
    // Remove all entries.
    QAction* clearAct = recentMenu_->addAction(
        tr_("Clear recent files…", QString::fromUtf8("清除全部历史…")));
    connect(clearAct, &QAction::triggered, this, [this] {
        QSettings s;
        s.setValue("recent", QStringList());
        s.sync();
        updateRecentMenu();
    });
    recentMenu_->setEnabled(true);
}

// Remove one path from the persisted recent-files list.
void MainWindow::removeRecent(const QString& path) {
    QStringList rec = QSettings().value("recent", QStringList())
                          .toStringList();
    rec.removeAll(path);
    QSettings s;
    s.setValue("recent", rec);
    s.sync();
    updateRecentMenu();
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
    // Prefer the original /XObject name (e.g. "/Im0") as the file stem. Scan
    // page-level /Resources -> /XObject maps and collect obj id -> name.
    std::map<long long, std::string> xname;
    for (const auto& o : pdf_.objects) {
        if (o.isStream) continue;
        std::string src;
        if (!pdf_.readObjectSource(o, src)) continue;
        std::map<std::string, long long> xo;
        resolveXObjectMap(pdf_, src, xo);
        for (const auto& kv : xo)
            if (!xname.count(kv.second)) xname[kv.second] = kv.first;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Export all images to directory", outDir_);
    if (dir.isEmpty()) return;
    outDir_ = dir;
    QFileInfo fi(currentFile_);
    const QString sub =
        fi.baseName().trimmed().isEmpty() ? QString("images") : fi.baseName();
    const QString outDir = QDir::cleanPath(dir + "/" + sub);
    QDir().mkpath(outDir);
    int total = 0;
    for (const auto& o : pdf_.objects)
        if (o.isStream && o.subtype == "Image") ++total;
    QProgressDialog dlg(
        total == 0 ? QString("Exporting images…")
                   : QString::fromUtf8("Exporting %1 image(s)…").arg(total),
        QString(), 0, total > 0 ? total : 1, this);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setCancelButton(nullptr);
    dlg.setMinimumDuration(400);
    dlg.show();
    int saved = 0;
    int done = 0;
    for (const auto& o : pdf_.objects) {
        if (!o.isStream || o.subtype != "Image") continue;
        ++done;
        dlg.setValue(total > 0 ? done : 1);
        QCoreApplication::processEvents();
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
        const auto nit = xname.find(o.id);
        const QString stem =
            nit != xname.end()
                ? QString::fromStdString(nit->second)
                : QString("image_") + QString::number(o.id);
        const QString path = outDir + "/" + stem + "." + ext;
        if (img.save(path, fmt.toLatin1().constData())) {
            ++saved;
            log("exported obj " + QString::number(o.id) + " -> " + path);
        } else {
            log("obj " + QString::number(o.id) + ": save failed");
        }
    }
    dlg.close();
    status_->setText(QString("exported %1 image(s)").arg(saved));
}

void MainWindow::fillTable() {
    table_->setRowCount(static_cast<int>(pdf_.objects.size()));
    for (int r = 0; r < static_cast<int>(pdf_.objects.size()); ++r) {
        const Object& o = pdf_.objects[static_cast<size_t>(r)];
        QStringList cells = {
            QString::number(o.id),
            QString::fromStdString(o.type),
            o.typeName.empty() ? QString("-") : "/" + QString::fromStdString(o.typeName),
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
    // Referential integrity: xref live entries with no scanned object, and
    // scanned objects unreachable from the xref.
    std::set<int> scanned;
    for (const auto& o : pdf_.objects) scanned.insert(o.id);
    std::set<int> liveXref;
    for (const auto& e : pdf_.xref)
        if (!e.free) liveXref.insert(e.id);
    std::vector<int> dangling;
    for (int id : liveXref)
        if (!scanned.count(id)) dangling.push_back(id);
    std::vector<int> orphans;
    for (int id : scanned)
        if (!liveXref.count(id)) orphans.push_back(id);
    text += QString("Consistency:\n");
    if (t.hasXref) {
        text += QString("  xref live entries not found in scan: %1\n")
                    .arg(dangling.size());
        for (int id : dangling)
            text += QString("    xref %1 has no scanned object\n").arg(id);
        text += QString("  scanned objects absent from xref: %1\n")
                    .arg(orphans.size());
        for (int id : orphans)
            text += QString("    obj %1 not in xref\n").arg(id);
        if (t.root > 0 && !scanned.count(t.root))
            text += "  /Root " + QString::number(t.root) +
                    " is not a scanned object\n";
        if (t.info > 0 && !scanned.count(t.info))
            text += "  /Info " + QString::number(t.info) +
                    " is not a scanned object\n";
        text += "  (orphans are generally harmless — streams are skipped\n";
        text += "   during scan when the xref points past 'endstream')";
    } else {
        text += "  (no classic xref to check against)";
    }
    // /Info dictionary: title, author, etc. when present.
    if (t.info > 0) {
        for (const auto& o : pdf_.objects) {
            if (o.id != t.info) continue;
            std::string src;
            if (!o.isStream && pdf_.readObjectSource(o, src)) {
                const auto dm = parseTopDict(src);
                auto get = [&](const char* key) -> QString {
                    const auto it = dm.find(key);
                    if (it == dm.end()) return QString();
                    for (const PdfToken& v : it->second) {
                        if (v.kind == PdfToken::String)
                            return QString::fromStdString(v.text);
                        if (v.kind == PdfToken::Name)
                            return QString("/") +
                                   QString::fromStdString(v.text);
                    }
                    return QString();
                };
                text += QString("\n/Info metadata:\n");
                const struct { const char* k; } keys[] = {
                    {"Title"}, {"Author"}, {"Subject"}, {"Creator"},
                    {"Producer"}, {"CreationDate"}, {"ModDate"}};
                for (const auto& k : keys) {
                    const QString v = get(k.k);
                    if (!v.isEmpty())
                        text += QString("  /%1: %2\n").arg(k.k).arg(v);
                }
            }
            break;
        }
    }
    info_->setPlainText(text);
}

void MainWindow::showObject(const Object& o) {
    contextObjId_ = o.id;

    // Raw source text for every object kind.
    std::string src;
    const bool haveSource = pdf_.readObjectSource(o, src);

    // Text tab: decoded stream (text streams) or raw object source (others).
    preview_->clear();
    contentView_->clear();
    previewTabs_->setTabEnabled(4, false);
    previewTabs_->setTabVisible(4, false);
    previewTabs_->setTabEnabled(5, false);
    previewTabs_->setTabVisible(5, false);

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
    pageScroll_->hide();
    previewTabs_->setTabVisible(0, false);
    previewTabs_->setTabEnabled(0, false);
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
        // Detect /Type /Page and /Type /Pages for the diagram. Parse the
        // object's top-level dict, collect /Kids, then follow the /Parent
        // chain so /Page inherits /MediaBox and /Rotate.
        const auto dm = parseTopDict(src);
        std::string type;
        std::vector<int> kids;
        std::vector<int> contentsRefs;
        long long parentRef = 0;
        int countDir = -1;       // /Count of the Pages node, if present
        double mb[4] = {0, 0, 612, 792};
        int rotate = 0;
        for (const auto& kv : dm) {
            const std::string& key2 = kv.first;
            const auto& val = kv.second;
            if (key2 == "Type") {
                for (const PdfToken& v : val)
                    if (v.kind == PdfToken::Name) { type = v.text; break; }
            } else if (key2 == "Count") {
                for (const PdfToken& v : val) {
                    if (v.kind == PdfToken::Number) {
                        try { countDir = std::stoi(v.text); } catch (...) { }
                        break;
                    }
                }
            } else if (key2 == "Rotate") {
                for (const PdfToken& v : val) {
                    if (v.kind == PdfToken::Number) {
                        try { rotate = std::stoi(v.text); } catch (...) { }
                        break;
                    }
                }
            } else if (key2 == "Parent") {
                for (const PdfToken& v : val)
                    if (v.kind == PdfToken::Ref) { parentRef = v.refId; break; }
            } else if (key2 == "MediaBox") {
                int vals = 0;
                for (const PdfToken& v : val) {
                    if (v.kind != PdfToken::Number) continue;
                    if (vals < 4) {
                        try { mb[vals] = std::stod(v.text); }
                        catch (...) { mb[vals] = 0; }
                        ++vals;
                    }
                }
            } else if (key2 == "Kids") {
                for (const PdfToken& v : val)
                    if (v.kind == PdfToken::Ref)
                        kids.push_back(static_cast<int>(v.refId));
            } else if (key2 == "Contents") {
                for (const PdfToken& v : val)
                    if (v.kind == PdfToken::Ref)
                        contentsRefs.push_back(static_cast<int>(v.refId));
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
                    const auto pm = parseTopDict(psrc);
                    double pband[2] = {0, 0};
                    int prot = 0;
                    long long pparent = 0;
                    bool gotBox = false;
                    for (const auto& kv : pm) {
                        const std::string& pk = kv.first;
                        const auto& val = kv.second;
                        if (pk == "Rotate") {
                            for (const PdfToken& v : val) {
                                if (v.kind != PdfToken::Number) continue;
                                try { prot = std::stoi(v.text); }
                                catch (...) { }
                                break;
                            }
                        } else if (pk == "Parent") {
                            for (const PdfToken& v : val)
                                if (v.kind == PdfToken::Ref) {
                                    pparent = v.refId;
                                    break;
                                }
                        } else if (pk == "MediaBox") {
                            int vals = 0;
                            for (const PdfToken& v : val) {
                                if (v.kind != PdfToken::Number) continue;
                                if (vals < 2) {
                                    try { pband[vals] = std::stod(v.text); }
                                    catch (...) { pband[vals] = 0; }
                                    ++vals;
                                }
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
                        const auto pm = parseTopDict(psrc);
                        std::string ptype;
                        std::vector<int> pkids;
                        for (const auto& kv : pm) {
                            const std::string& pk = kv.first;
                            const auto& val = kv.second;
                            if (pk == "Type") {
                                for (const PdfToken& v : val)
                                    if (v.kind == PdfToken::Name) {
                                        ptype = v.text;
                                        break;
                                    }
                            } else if (pk == "Kids") {
                                for (const PdfToken& v : val)
                                    if (v.kind == PdfToken::Ref)
                                        pkids.push_back(
                                            static_cast<int>(v.refId));
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
            pageScroll_->show();
            previewTabs_->setTabVisible(0, true);
            previewTabs_->setTabEnabled(0, true);
            previewTabs_->setCurrentIndex(0);
        }
        // Content tab: resolve the page's /Contents stream(s), concatenate the
        // decoded operators and show them here so a /Page double-click jumps
        // straight to the content stream without hunting for the ref. Any
        // `/Name Do` operators are also picked up: the region each one paints
        // over is drawn on the page diagram as a box labelled with the XObject
        // name, and clicking the box jumps to the resolved target object.
        if (!contentsRefs.empty()) {
            std::string merged;
            std::map<std::string, long long> xobj;
            resolveXObjectMap(pdf_, src, xobj);
            std::vector<PageDiagram::ContentBox> boxes;
            for (int ref : contentsRefs) {
                for (const Object& co : pdf_.objects) {
                    if (co.id != ref) continue;
                    std::string dec;
                    std::string mark;
                    if (pdf_.readStreamDecoded(co, dec)) {
                        mark = "\n--- obj " + std::to_string(ref) +
                               " (decoded) ---\n";
                    } else if (pdf_.readStream(co, dec)) {
                        mark = "\n--- obj " + std::to_string(ref) +
                               " (raw, decode failed) ---\n";
                    } else {
                        mark = "\n--- obj " + std::to_string(ref) +
                               " (no stream data) ---\n";
                        dec.clear();
                    }
                    merged += mark + makePreview(dec, 65536, false);
                    for (const ContentBoxHit& hh :
                         scanContentBoxes(dec)) {
                        const auto it = xobj.find(hh.name);
                        PageDiagram::ContentBox cb;
                        cb.x0 = hh.x0;
                        cb.y0 = hh.y0;
                        cb.x1 = hh.x1;
                        cb.y1 = hh.y1;
                        cb.objId = it == xobj.end() ? 0 : it->second;
                        cb.label =
                            QString("/%1").arg(QString::fromStdString(hh.name));
                        boxes.push_back(cb);
                    }
                    break;
                }
            }
            if (!boxes.empty())
                pageDiagram_->setContentBoxes(boxes);
            if (!merged.empty()) {
                contentView_->setPlainText(QString::fromStdString(merged));
                previewTabs_->setTabVisible(5, true);
                previewTabs_->setTabEnabled(5, true);
            }
        }
    }
    previewTabs_->setTabEnabled(1, true);
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
    menu.addAction("Show raw bytes at offset…", this,
                   &MainWindow::jumpToRawOffset);
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void MainWindow::jumpToRawOffset() {
    int id = contextObjId_;
    if (id <= 0) {
        const int row = table_->currentRow();
        if (row >= 0 && row < table_->rowCount() && table_->item(row, 0))
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
    if (!found || found->offset < 0) return;
    std::string raw;
    if (!pdf_.readRawBytes(static_cast<size_t>(found->offset), 1024, raw))
        return;
    hexView_->setPlainText(hexDump(raw, 4096));
    previewTabs_->setCurrentIndex(3);
    status_->setText(QString("raw bytes at offset %1 (obj %2)")
                         .arg(found->offset)
                         .arg(id));
}

void MainWindow::showStructMenu(const QPoint& pos) {
    QTreeWidgetItem* item = structView_->itemAt(pos);
    if (!item) return;
    structView_->setCurrentItem(item);
    QMenu menu(this);
    menu.addAction("Copy subtree as JSON", this, &MainWindow::copySubtreeJson);
    menu.addAction("Copy subtree as text", this, &MainWindow::copySubtreeText);
    menu.exec(structView_->viewport()->mapToGlobal(pos));
}

namespace {
// Recursively render a structure-tree item to a JSON fragment.
void itemToJson(QTreeWidgetItem* it, bool isArray, QString& out) {
    if (it->childCount() == 0) {
        const QString val = it->text(1);
        if (val.isEmpty() == false && val[0] >= '0' && val[0] <= '9' &&
            val[0].isDigit())
            out += val;  // bare number
        else
            out += "\"" + val + "\"";
        return;
    }
    // Does this node mirror an array (numeric keys) or a dict?
    bool arr = true;
    for (int i = 0; i < it->childCount(); ++i) {
        bool ok = false;
        it->child(i)->text(0).toInt(&ok);
        if (!ok) {
            arr = false;
            break;
        }
    }
    if (!isArray) {
        if (arr) {
            out += "[";
            for (int i = 0; i < it->childCount(); ++i) {
                if (i) out += ", ";
                itemToJson(it->child(i), true, out);
            }
            out += "]";
        } else {
            out += "{";
            for (int i = 0; i < it->childCount(); ++i) {
                if (i) out += ", ";
                out += "\"" + it->child(i)->text(0) + "\": ";
                itemToJson(it->child(i), false, out);
            }
            out += "}";
        }
    } else {
        // Inside an array the children carry numeric keys; emit each value.
        for (int i = 0; i < it->childCount(); ++i) {
            if (i) out += ", ";
            itemToJson(it->child(i), false, out);
        }
    }
}

void itemToText(QTreeWidgetItem* it, int depth, QString& out) {
    for (int i = 0; i < it->childCount(); ++i) {
        QTreeWidgetItem* c = it->child(i);
        out += QString(depth * 2, ' ') + c->text(0) + " = " + c->text(1) +
               "\n";
        itemToText(c, depth + 1, out);
    }
}
}  // namespace

void MainWindow::copySubtreeJson() {
    QTreeWidgetItem* item = structView_->currentItem();
    if (!item) return;
    QString out;
    itemToJson(item, false, out);
    QApplication::clipboard()->setText(out);
    status_->setText("copied subtree as JSON");
}

void MainWindow::copySubtreeText() {
    QTreeWidgetItem* item = structView_->currentItem();
    if (!item) return;
    QString out;
    itemToText(item, 0, out);
    QApplication::clipboard()->setText(out);
    status_->setText("copied subtree as text");
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

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl& u : urls) {
        if (!u.isLocalFile()) continue;
        if (QFileInfo(u.toLocalFile()).suffix().compare("pdf",
                                                        Qt::CaseInsensitive) == 0) {
            event->acceptProposedAction();
            return;
        }
    }
    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl& u : urls) {
        if (!u.isLocalFile()) continue;
        const QString path = u.toLocalFile();
        if (QFileInfo(path).suffix().compare("pdf", Qt::CaseInsensitive) == 0) {
            if (path != currentFile_) openPath(path);
            event->acceptProposedAction();
            return;
        }
    }
    QMainWindow::dropEvent(event);
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