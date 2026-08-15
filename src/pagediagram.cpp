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
    update();
}

void PageDiagram::clearDiagram() {
    sizeLabel_.clear();
    update();
}

void PageDiagram::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x2d, 0x2f, 0x43));
    if (sizeLabel_.isEmpty() || w_ <= 0 || h_ <= 0) {
        p.setPen(QColor(0x6f, 0x72, 0x8f));
        p.drawText(rect(), Qt::AlignCenter, "Select a /Type /Page object");
        return;
    }
    const int margin = 24;
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
    QFont f = p.font();
    f.setPixelSize(12);
    p.setFont(f);
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