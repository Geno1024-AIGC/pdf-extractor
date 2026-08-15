#ifndef CODEEDITOR_H
#define CODEEDITOR_H

#include <QColor>
#include <QPlainTextEdit>

class QPaintEvent;
class QResizeEvent;
class QSize;
class QWidget;

// A read-only QPlainTextEdit with a real line-number gutter that tracks the
// block layout, so line numbers stay aligned while scrolling.
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget* parent = nullptr);

    void lineNumberAreaPaintEvent(QPaintEvent* event);
    int lineNumberAreaWidth() const;

    // Adjust the line-number gutter colours for dark/light themes.
    void setLineNumberColors(const QColor& background, const QColor& text);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateLineNumberAreaWidth();
    QWidget* lineNumberArea_;
    QColor gutterBackground_ = QColor(0x2d, 0x2f, 0x43);
    QColor gutterText_ = QColor(0x6f, 0x72, 0x8f);
};

#endif  // CODEEDITOR_H
