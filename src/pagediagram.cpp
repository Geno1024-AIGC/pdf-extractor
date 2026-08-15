#include "pagediagram.h"

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
    update();
}

void PageDiagram::setPageTree(int count, const std::vector<int>& kids,
                              const QString& label) {
    treeMode_ = true;
    treeCount_ = count;
    kids_ = kids;
    sizeLabel_ = label;
    // Reserve enough vertical space for the thumbnail grid so that pages
    // don't overflow the widget (6 columns, 56px tiles + gap + label line).
    const int rows = (count + 5) / 6;
    const int need = rows * 56 + (rows - 1) * 8 + 34;
    const int setH = std::min(360, std::max(90, need));
    setMinimumHeight(setH);
    setMaximumHeight(setH);
    update();
}

void PageDiagram::clearDiagram() {
    sizeLabel_.clear();
    treeMode_ = false;
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
        const int cols = 6;
        const int rows = (treeCount_ + cols - 1) / cols;
        const int gap = 8;
        const int tileW = 46;
        const int tileH = 56;
        const int gw = cols * tileW + (cols - 1) * gap;
        const int gh = rows * tileH + (rows - 1) * gap;
        const int gx = (width() - gw) / 2;
        int gy = (height() - gh) / 2;
        // Keep the label visible at the bottom.
        gy = std::max(4, gy - 6);
        gy = std::min(gy, height() - gh - 18);
        for (int i = 0; i < treeCount_; ++i) {
            const int c = i % cols;
            const int r = i / cols;
            const int kid = i < static_cast<int>(kids_.size()) ? kids_[i] : 0;
            paintRect(p, QRect(gx + c * (tileW + gap),
                               gy + r * (tileH + gap), tileW, tileH),
                      kid ? QString("obj %1").arg(kid) : QString("#%1").arg(i + 1));
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
}