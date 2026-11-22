#include "models/MessageHtml.h"
#include "models/QuoteBars.h"

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QtTest/QtTest>

// Quick's text paints a table cell's background only behind its own lines and
// never its border, so the bar must come from QuoteBars; this renders a real
// TextEdit with it.
class QuoteRenderTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void theQuoteBarIsPaintedByARealTextEdit()
    {
        const MessageHtml::MentionStyle style{QString(), QString(), QString(),
                                              false, QStringLiteral("#cc3333"),
                                              QStringLiteral("#777777")};
        const QString html = MessageHtml::sanitize(
            QStringLiteral("<p>head</p><blockquote>quoted<br>text"
                           "<blockquote>nested</blockquote></blockquote>"),
            nullptr, QString(), style);

        QQmlEngine engine;
        QQmlComponent comp(&engine);
        qmlRegisterType<QuoteBars>("QuoteTest", 1, 0, "QuoteBars");
        comp.setData(
            "import QtQuick\nimport QuoteTest\n"
            "TextEdit { id: e; width: 300; readOnly: true\n"
            "  textFormat: TextEdit.RichText; wrapMode: TextEdit.Wrap\n"
            "  QuoteBars { id: q; document: e.textDocument }\n"
            "  Repeater { model: q.bars\n"
            "    Rectangle { x: modelData.x; y: modelData.y; width: 3\n"
            "      height: modelData.height; color: modelData.color } } }",
            QUrl());
        QQuickWindow window;
        window.setColor(Qt::white);
        window.resize(300, 200);
        auto *edit = qobject_cast<QQuickItem *>(comp.create());
        QVERIFY2(edit, qPrintable(comp.errorString()));
        edit->setParentItem(window.contentItem());
        edit->setProperty("text", html);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QImage img = window.grabWindow().convertToFormat(QImage::Format_RGB32);

        int barRows = 0;
        for (int y = 0; y < img.height(); ++y)
            if (QColor(img.pixel(1, y)) == QColor("#cc3333"))
                ++barRows;
        QVERIFY2(barRows >= 50, qPrintable(QString::number(barRows)));

        int nestedBarRows = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 9; x < 17; ++x)
                if (QColor(img.pixel(x, y)) == QColor("#cc3333")) {
                    ++nestedBarRows;
                    break;
                }
        QVERIFY2(nestedBarRows >= 8, qPrintable(QString::number(nestedBarRows)));
    }

    void selectingBesideAQuoteDoesNotThickenIt()
    {
        const MessageHtml::MentionStyle style{QString(), QString(), QString(),
                                              false, QStringLiteral("#cc3333"),
                                              QStringLiteral("#777777")};
        const QString html = MessageHtml::sanitize(
            QStringLiteral("<blockquote>quoted words here</blockquote>"
                           "<p>first line after the quote</p>"
                           "<p>second line after the quote</p>"),
            nullptr, QString(), style);

        QQmlEngine engine;
        QQmlComponent comp(&engine);
        qmlRegisterType<QuoteBars>("QuoteTest", 1, 0, "QuoteBars");
        comp.setData(
            "import QtQuick\nimport QuoteTest\n"
            "TextEdit { id: e; width: 300; readOnly: true; selectByMouse: true\n"
            "  textFormat: TextEdit.RichText; wrapMode: TextEdit.Wrap\n"
            "  color: 'black'; selectionColor: 'navy'; selectedTextColor: 'white'\n"
            "  property alias bars: q.bars\n"
            "  QuoteBars { id: q; document: e.textDocument } }",
            QUrl());
        QQuickWindow window;
        window.setColor(Qt::white);
        window.resize(300, 200);
        auto *edit = qobject_cast<QQuickItem *>(comp.create());
        QVERIFY2(edit, qPrintable(comp.errorString()));
        edit->setParentItem(window.contentItem());
        edit->setProperty("text", html);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const QVariantList bars = edit->property("bars").toList();
        QCOMPARE(bars.size(), 1);
        const QVariantMap bar = bars.first().toMap();
        const QRect quote(4, int(bar.value(QStringLiteral("y")).toReal()),
                          290, int(bar.value(QStringLiteral("height")).toReal()));
        const QImage before = window.grabWindow().copy(quote);

        const int start = edit->property("length").toInt() - 40;
        for (int end = start + 2; end <= start + 30; end += 4) {
            QMetaObject::invokeMethod(edit, "select", Q_ARG(int, start),
                                      Q_ARG(int, end));
            QCOMPARE(window.grabWindow().copy(quote), before);
        }
        QMetaObject::invokeMethod(edit, "deselect");
        QCOMPARE(window.grabWindow().copy(quote), before);
    }

    void copyingAcrossAQuoteAddsNoBlankLine()
    {
        QTextDocument doc;
        doc.setHtml(MessageHtml::sanitize(
            QStringLiteral("<p>head</p><blockquote>quoted</blockquote>"
                           "<p>tail</p>"),
            nullptr, QString(), {}));
        QTextCursor cursor(&doc);
        cursor.select(QTextCursor::Document);
        const QString text = cursor.selection().toPlainText();
        QVERIFY2(text.contains(QStringLiteral("head\nquoted")),
                 qPrintable(text));
    }
};

QTEST_MAIN(QuoteRenderTest)
#include "QuoteRenderTest.moc"
