#include "pagediagram.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRect>

#include <algorithm>
#include <cmath>

PageDiagram::PageDiagram(QWidget* parent) : QWidget(parent) {}

void PageDiagram::setBox(double w, double h, int rotate,
                         const QString& sizeLabel) {
    w_ = w;
    h_ = h;
    rotate_ = rotate;
    sizeLabel_ = sizeLabel;
    treeMode_ = false;
    tiles_.clear();
    contentBoxes_.clear();
    boxRects_.clear();
    update();
}

void PageDiagram::setPageTree(int count, const std::vector<int>& kids,
                              const QString& label) {
    treeMode_ = true;
    treeCount_ = count;
    kids_ = kids;
    sizeLabel_ = label;
    // Do not cap the height here: the widget is hosted in a scroll area, so a
    // long tree can scroll instead of being clipped.
    const int rows = (count + 5) / 6;
    const int need = rows * 56 + (rows - 1) * 8 + 34;
    setMinimumHeight(std::max(90, need));
    setMaximumHeight(16777215);
    // Keep the 6-column grid fully visible horizontally once laid out.
    setMinimumWidth(6 * 46 + 5 * 8 + 48);
    setMaximumWidth(16777215);
    computeTiles();
    update();
}

void PageDiagram::clearDiagram() {
    sizeLabel_.clear();
    treeMode_ = false;
    tiles_.clear();
    contentBoxes_.clear();
    boxRects_.clear();
    update();
}

void PageDiagram::setContentBoxes(const std::vector<ContentBox>& boxes) {
    contentBoxes_ = boxes;
    boxRects_.clear();
    update();
}

void PageDiagram::paintRect(QPainter& p, const QRect& r, const QString& text) {
    p.setPen(QPen(QColor(0x9a, 0xa0, 0xc3), 1));
    p.setBrush(QColor(0x3a, 0x3d, 0x55));
    p.drawRect(r);
    p.setPen(QColor(0x9a, 0xa0, 0xc3));
    QFont f = p.font();
    f.setPixelSize(10);
    p.setFont(f);
    p.drawText(r, Qt::AlignCenter, text);
}

void PageDiagram::computeTiles() {
    tiles_.clear();
    if (!treeMode_) return;
    const int cols = 6;
    const int rows = (treeCount_ + cols - 1) / cols;
    const int gap = 8;
    const int tileW = 46;
    const int tileH = 56;
    const int gw = cols * tileW + (cols - 1) * gap;
    const int gh = rows * tileH + (rows - 1) * gap;
    const int gx = (width() - gw) / 2;
    int gy = (height() - gh) / 2;
    gy = std::max(4, gy - 6);
    gy = std::min(gy, height() - gh - 18);
    tiles_.resize(treeCount_);
    for (int i = 0; i < treeCount_; ++i) {
        const int c = i % cols;
        const int r = i / cols;
        tiles_[i] = QRect(gx + c * (tileW + gap), gy + r * (tileH + gap),
                          tileW, tileH);
    }
}

void PageDiagram::resizeEvent(QResizeEvent* event) {
    computeTiles();
    QWidget::resizeEvent(event);
}

void PageDiagram::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x2d, 0x2f, 0x43));
    if (sizeLabel_.isEmpty()) {
        p.setPen(QColor(0x6f, 0x72, 0x8f));
        p.drawText(rect(), Qt::AlignCenter,
                   "Select a /Type /Page or /Type /Pages object");
        return;
    }
    const int margin = 24;
    QFont small = p.font();
    small.setPixelSize(12);
    p.setFont(small);
    if (treeMode_) {
        // Draw a grid of thumbnail pages for a /Pages tree node.
        computeTiles();
        for (int i = 0; i < treeCount_; ++i) {
            const int kid =
                i < static_cast<int>(kids_.size()) ? kids_[i] : 0;
            paintRect(p, tiles_[i],
                      kid ? QString("obj %1").arg(kid)
                          : QString("#%1").arg(i + 1));
        }
        p.setPen(QColor(0x9a, 0xa0, 0xc3));
        p.drawText(rect(), Qt::AlignHCenter | Qt::AlignBottom,
                   sizeLabel_ + QString("  %1 pages").arg(treeCount_));
        return;
    }
    if (w_ <= 0 || h_ <= 0) {
        p.setPen(QColor(0x6f, 0x72, 0x8f));
        p.drawText(rect(), Qt::AlignCenter,
                   "Select a /Type /Page or /Type /Pages object");
        return;
    }
    const double scale = std::min(
        static_cast<double>(width() - margin * 2) / w_,
        static_cast<double>(height() - margin * 2) / h_);
    const int pw = static_cast<int>(w_ * scale);
    const int ph = static_cast<int>(h_ * scale);
    const int px = (width() - pw) / 2;
    const int py = (height() - ph) / 2;
    p.setPen(QPen(QColor(0x9a, 0xa0, 0xc3), 2));
    p.setBrush(QColor(0x3a, 0x3d, 0x55));
    p.drawRect(px, py, pw, ph);
    p.setPen(QColor(0x9a, 0xa0, 0xc3));
    p.drawText(QRect(px, py, pw, 18), Qt::AlignCenter, sizeLabel_);
    p.drawText(px + 6, py + ph - 4,
               QString("w=%1  h=%2  rot=%3")
                   .arg(w_, 0, 'g', 3)
                   .arg(h_, 0, 'g', 3)
                   .arg(rotate_));
    if (rotate_ != 0) {
        QPainterPath path;
        path.moveTo(px + pw, py);
        path.lineTo(px + pw - 16, py + 6);
        path.lineTo(px + pw - 6, py + 16);
        path.closeSubpath();
        p.setPen(QPen(QColor(0x4a, 0x6c, 0xf7), 1));
        p.setBrush(QColor(0x4a, 0x6c, 0xf7));
        p.drawPath(path);
    }
    // Overlay the `cm`-transformed content boxes on the page rectangle.
    boxRects_.clear();
    QFont boxF = p.font();
    boxF.setPixelSize(10);
    p.setFont(boxF);
    for (const ContentBox& b : contentBoxes_) {
        const int sx = px + static_cast<int>(b.x0 * scale);
        const int sy = py + ph - static_cast<int>(b.y1 * scale);
        const int sw = static_cast<int>((b.x1 - b.x0) * scale);
        const int sh = static_cast<int>((b.y1 - b.y0) * scale);
        if (sw <= 0 || sh <= 0) continue;
        boxRects_.push_back(QRect(sx, sy, sw, sh));
        // Inset a full-page box so its outline does not hide under the page
        // border and the "frame" stays visible.
        const int inset =
            (sw >= pw - 2 && sh >= ph - 2) ? 3 : 0;
        QRect r(sx + inset, sy + inset, sw - 2 * inset, sh - 2 * inset);
        p.setPen(QPen(QColor(0x4a, 0x6c, 0xf7), 1));
        p.setBrush(QColor(0x4a, 0x6c, 0xf7, 70));
        p.drawRect(r);
        QRect lr(r.x(), r.y() - 14, r.width(), 14);
        if (lr.top() < py + 2) lr.moveTop(r.y());
        p.setPen(b.objId > 0
                     ? QColor(0x7c, 0x9c, 0xff)
                     : QColor(0x9a, 0xa0, 0xc3));
        p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, b.label);
    }
}

void PageDiagram::mouseReleaseEvent(QMouseEvent* event) {
    const QPoint pos = event->pos();
    if (treeMode_) {
        for (int i = 0; i < static_cast<int>(tiles_.size()); ++i) {
            if (tiles_[i].contains(pos)) {
                const int kid =
                    i < static_cast<int>(kids_.size()) ? kids_[i] : 0;
                if (kid > 0) emit pageClicked(kid);
                break;
            }
        }
        QWidget::mouseReleaseEvent(event);
        return;
    }
    for (int i = 0; i < static_cast<int>(boxRects_.size()); ++i) {
        if (boxRects_[i].contains(pos)) {
            const long long obj =
                i < static_cast<int>(contentBoxes_.size())
                    ? contentBoxes_[i].objId
                    : 0;
            if (obj > 0) emit pageClicked(static_cast<int>(obj));
            break;
        }
    }
    QWidget::mouseReleaseEvent(event);
}