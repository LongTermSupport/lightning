#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextCursor>
#include <QtTest>

#include "models/MentionHighlighter.h"

class MentionHighlighterEmojiTest : public QObject
{
    Q_OBJECT
private:
    static QTextCharFormat formatAt(QTextDocument &doc, int pos)
    {
        const QTextBlock block = doc.findBlock(pos);
        if (!block.isValid() || !block.layout())
            return {};
        const int inBlock = pos - block.position();
        for (const QTextLayout::FormatRange &r : block.layout()->formats()) {
            if (inBlock >= r.start && inBlock < r.start + r.length)
                return r.format;
        }
        return {};
    }

private Q_SLOTS:
    void emojiRunCarriesTheNamedFaceAndTextDoesNot()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("hi \U0001F600 there"));

        MentionHighlighter highlighter;
        highlighter.setEmojiFontFamily(QStringLiteral("Noto Color Emoji"));
        highlighter.QSyntaxHighlighter::setDocument(&doc);
        highlighter.rehighlight();

        const QTextCharFormat onEmoji = formatAt(doc, 3);
        QVERIFY2(onEmoji.fontFamilies().isValid(),
                 "the emoji run carries no font family at all");
        QCOMPARE(onEmoji.fontFamilies().toStringList(),
                 QStringList{ QStringLiteral("Noto Color Emoji") });
        const QTextCharFormat onLetter = formatAt(doc, 0);
        QVERIFY2(!onLetter.fontFamilies().isValid(),
                 "plain text was given the emoji face");
    }

    void emojiAreFormattedWithNoMentionRangesSet()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("\U0001F389"));
        MentionHighlighter highlighter;
        highlighter.setEmojiFontFamily(QStringLiteral("Noto Color Emoji"));
        highlighter.QSyntaxHighlighter::setDocument(&doc);
        highlighter.rehighlight();
        QVERIFY(highlighter.ranges().isEmpty());
        QCOMPARE(formatAt(doc, 0).fontFamilies().toStringList(),
                 QStringList{ QStringLiteral("Noto Color Emoji") });
    }

    void aSubdivisionFlagIsOneEmojiRun()
    {
        QTextDocument doc;
        const QString england = QStringLiteral(
            "\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007F");
        doc.setPlainText(QStringLiteral("a ") + england + QStringLiteral(" b"));
        MentionHighlighter highlighter;
        highlighter.setEmojiFontFamily(QStringLiteral("Noto Color Emoji"));
        highlighter.QSyntaxHighlighter::setDocument(&doc);
        highlighter.rehighlight();
        for (int pos = 2; pos < 2 + england.size(); ++pos) {
            QVERIFY2(formatAt(doc, pos).fontFamilies().toStringList()
                         == QStringList{ QStringLiteral("Noto Color Emoji") },
                     qPrintable(QStringLiteral("utf-16 offset %1 left the emoji run").arg(pos)));
        }
        QVERIFY(!formatAt(doc, 2 + england.size()).fontFamilies().isValid());
    }

    void noFamilyLeavesTheDocumentAlone()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("\U0001F600"));
        MentionHighlighter highlighter;
        highlighter.QSyntaxHighlighter::setDocument(&doc);
        highlighter.rehighlight();
        QVERIFY(!formatAt(doc, 0).fontFamilies().isValid());
    }
};

QTEST_MAIN(MentionHighlighterEmojiTest)
#include "MentionHighlighterEmojiTest.moc"
