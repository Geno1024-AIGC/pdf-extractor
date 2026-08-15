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
    explicit PageDiagram(QWidget* parent = nullptr);
    void setBox(double w, double h, int rotate, const QString& sizeLabel);
    void setPageTree(int count, const std::vector<int>& kids,
                     const QString& label);
    void clearDiagram();

signals:
    void pageClicked(int objId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void paintRect(QPainter& p, const QRect& r, const QString& text);
    void computeTiles();

    double w_ = 0;
    double h_ = 0;
    int rotate_ = 0;
    QString sizeLabel_;
    bool treeMode_ = false;
    int treeCount_ = 0;
    std::vector<int> kids_;
    std::vector<QRect> tiles_;  // grid tile geometry for hit-testing
};

#endif  // PAGEDIAGRAM_H
