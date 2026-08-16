#ifndef PAGEDIAGRAM_H
#define PAGEDIAGRAM_H

#include <QWidget>
#include <QString>
#include <QRect>
#include <vector>

// A small schematic view of a PDF page object: draws the page rectangle to
// scale with its dimensions and rotation marker, or a thumbnail grid when a
// /Pages tree node is selected. The grid tiles are clickable and report the
// referenced page object id.
class PageDiagram : public QWidget {
    Q_OBJECT
public:
    // A rectangular region on the page in user-space coordinates (y up),
    // typically a `cm`-transformed area where a `/Name Do` paints an image
    // or form XObject. objId links it to the resolved target object.
    struct ContentBox {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        long long objId = 0;
        QString label;
    };

    explicit PageDiagram(QWidget* parent = nullptr);
    void setBox(double w, double h, int rotate, const QString& sizeLabel);
    // ratios[i] parallels kids[i] and holds a compact "612 x 792" size label
    // for the i-th page; empty entries are drawn without a size line.
    void setPageTree(int count, const std::vector<int>& kids,
                     const std::vector<QString>& ratios,
                     const QString& label);
    void setContentBoxes(const std::vector<ContentBox>& boxes);
    void clearDiagram();
    void setZoom(double zoom);
    void setZoomActual();
    double zoom() const { return zoom_; }

signals:
    void pageClicked(int objId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void paintRect(QPainter& p, const QRect& r, const QString& text,
                   const QString& sub = QString());
    void computeTiles();
    void applyTreeSize();

    double w_ = 0;
    double h_ = 0;
    double zoom_ = 1.0;
    int rotate_ = 0;
    QString sizeLabel_;
    bool treeMode_ = false;
    int treeCount_ = 0;
    std::vector<int> kids_;
    std::vector<QString> ratios_;  // parallel to kids_: page size text per tile
    std::vector<QRect> tiles_;  // grid tile geometry for hit-testing
    std::vector<ContentBox> contentBoxes_;  // user-space overlays
    std::vector<QRect> boxRects_;           // screen rects for hit-testing
};

#endif  // PAGEDIAGRAM_H
