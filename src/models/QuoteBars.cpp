#include "models/QuoteBars.h"

#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>

QuoteBars::QuoteBars(QObject *parent)
    : QObject(parent)
{
}

void QuoteBars::setDocument(QQuickTextDocument *document)
{
    if (m_quickDocument == document)
        return;
    if (m_textDocument) {
        disconnect(m_textDocument, nullptr, this, nullptr);
        if (auto *layout = m_textDocument->documentLayout())
            disconnect(layout, nullptr, this, nullptr);
    }
    if (m_textEdit)
        disconnect(m_textEdit, nullptr, this, nullptr);
    m_quickDocument = document;
    m_textDocument = document ? document->textDocument() : nullptr;
    m_textEdit = document ? document->parent() : nullptr;
    // QQuickTextEdit re-adds a child frame's text on every partial repaint
    // without removing the old nodes, so each selection change draws the
    // quote once more on top of itself; repaint every block instead.
    if (m_textEdit
        && m_textEdit->metaObject()->indexOfSignal("selectedTextChanged()") >= 0)
        connect(m_textEdit, SIGNAL(selectedTextChanged()), this,
                SLOT(repaintFrames()));
    if (m_textDocument) {
        connect(m_textDocument, &QTextDocument::contentsChanged, this,
                &QuoteBars::update);
        connect(m_textDocument->documentLayout(),
                &QAbstractTextDocumentLayout::documentSizeChanged, this,
                &QuoteBars::update);
    }
    Q_EMIT documentChanged();
    update();
}

void QuoteBars::collect(QTextFrame *frame, QVariantList &out) const
{
    for (auto it = frame->begin(); !it.atEnd(); ++it) {
        QTextFrame *child = it.currentFrame();
        if (!child)
            continue;
        if (auto *table = qobject_cast<QTextTable *>(child)) {
            const QTextTableFormat format = table->format();
            if (table->rows() == 1 && table->columns() == 1
                && format.border() == 0
                && format.borderBrush().style() == Qt::SolidPattern) {
                const QRectF r =
                    m_textDocument->documentLayout()->frameBoundingRect(table);
                out.append(QVariantMap{
                    {QStringLiteral("x"),
                     r.x() - table->cellAt(0, 0).format()
                                 .toTableCellFormat().leftPadding()},
                    {QStringLiteral("y"), r.y()},
                    {QStringLiteral("height"), r.height()},
                    {QStringLiteral("color"),
                     format.borderBrush().color()},
                });
            }
        }
        collect(child, out);
    }
}

void QuoteBars::repaintFrames()
{
    if (!m_textDocument || m_textDocument->rootFrame()->childFrames().isEmpty())
        return;
    auto *layout = m_textDocument->documentLayout();
    for (QTextBlock b = m_textDocument->begin(); b.isValid(); b = b.next())
        Q_EMIT layout->updateBlock(b);
}

void QuoteBars::update()
{
    QVariantList next;
    if (m_textDocument)
        collect(m_textDocument->rootFrame(), next);
    if (next != m_bars) {
        m_bars = next;
        Q_EMIT barsChanged();
    }
}
