#ifndef PAGEDIAGRAM_H
#define PAGEDIAGRAM_H

#include <QWidget>
#include <QString>
#include <vector>

// A small schematic view of a PDF page object: draws the page rectangle to
// scale with its dimensions and rotation marker, or a thumbnail grid when a
// /Pages tree node is selected.
class PageDiagram : public QWidget {
    Q_OBJECT
public:
    explicit PageDiagram(QWidget* parent = nullptr);
    void setBox(double w, double h, int rotate, const QString& sizeLabel);
    void setPageTree(int count, const std::vector<int>& kids,
                     const QString& label);
    void clearDiagram();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void paintRect(QPainter& p, const QRect& r, const QString& text);

    double w_ = 0;
    double h_ = 0;
    int rotate_ = 0;
    QString sizeLabel_;
    bool treeMode_ = false;
    int treeCount_ = 0;
    std::vector<int> kids_;
};

#endif  // PAGEDIAGRAM_H
