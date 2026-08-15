#ifndef CODEEDITOR_H
#define CODEEDITOR_H

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

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateLineNumberAreaWidth();
    QWidget* lineNumberArea_;
};

#endif  // CODEEDITOR_H
