#ifndef PAGEDIAGRAM_H
#define PAGEDIAGRAM_H

#include <QWidget>
#include <QString>

// A small schematic view of a PDF page object: draws the page rectangle to
// scale with its dimensions and rotation marker.
class PageDiagram : public QWidget {
    Q_OBJECT
public:
    explicit PageDiagram(QWidget* parent = nullptr);
    void setBox(double w, double h, int rotate, const QString& sizeLabel);
    void clearDiagram();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double w_ = 0;
    double h_ = 0;
    int rotate_ = 0;
    QString sizeLabel_;
};

#endif  // PAGEDIAGRAM_H
