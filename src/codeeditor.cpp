#include "codeeditor.h"

#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QRect>
#include <QSize>
#include <QTextBlock>
#include <QWidget>

namespace {

class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor* editor) : QWidget(editor), editor_(editor) {}

    QSize sizeHint() const override {
        return QSize(editor_->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        editor_->lineNumberAreaPaintEvent(event);
    }

private:
    CodeEditor* editor_;
};

}  // namespace

CodeEditor::CodeEditor(QWidget* parent) : QPlainTextEdit(parent) {
    lineNumberArea_ = new LineNumberArea(this);
    setReadOnly(true);

    connect(this, &QPlainTextEdit::blockCountChanged, this,
            [this](int) { updateLineNumberAreaWidth(); });
    connect(this, &QPlainTextEdit::updateRequest, this,
            [this](const QRect& r, int dy) {
                if (dy) {
                    lineNumberArea_->scroll(0, dy);
                } else {
                    lineNumberArea_->update(0, r.y(), lineNumberArea_->width(),
                                             r.height());
                }
                if (r.contains(viewport()->rect())) updateLineNumberAreaWidth();
            });

    updateLineNumberAreaWidth();
}

int CodeEditor::lineNumberAreaWidth() const {
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    int space = 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    return space;
}

void CodeEditor::updateLineNumberAreaWidth() {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect contents = contentsRect();
    lineNumberArea_->setGeometry(
        QRect(contents.left(), contents.top(), lineNumberAreaWidth(),
              contents.height()));
}

void CodeEditor::setLineNumberColors(const QColor& background,
                                     const QColor& text) {
    gutterBackground_ = background;
    gutterText_ = text;
    lineNumberArea_->update();
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent* event) {
    QPainter painter(lineNumberArea_);
    painter.fillRect(event->rect(), gutterBackground_);

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(
        blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(gutterText_);
            painter.drawText(0, top, lineNumberArea_->width() -
                                          fontMetrics().horizontalAdvance('9'),
                             fontMetrics().height(), Qt::AlignRight,
                             QString::number(blockNumber + 1));
        }
        block = block.next();
        if (!block.isValid()) break;
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++blockNumber;
    }
}