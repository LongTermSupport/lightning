#include "app/FontManager.h"

#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QRawFont>
#include <QTemporaryDir>
#include <QTextLayout>
#include <QtTest>

#include <utility>

namespace {

const QString kFlagFamily = QStringLiteral("Twemoji Country Flags");
const QString kFlaglessEmoji = QStringLiteral("Segoe UI Emoji");
const QString kFrance = QStringLiteral("\U0001F1EB\U0001F1F7");
const QString kEngland = QStringLiteral(
    "\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007F");
const QString kGrin = QStringLiteral("\U0001F600");

struct Drawn {
    QStringList families;
    int glyphs = 0;
};

Drawn draw(const QFont &font, const QString &text)
{
    QTextLayout layout(text, font);
    layout.beginLayout();
    layout.createLine();
    layout.endLayout();
    Drawn out;
    for (const QGlyphRun &run : layout.glyphRuns()) {
        int glyphs = 0;
        for (const quint32 glyph : run.glyphIndexes())
            glyphs += glyph != 0 ? 1 : 0;
        if (glyphs == 0)
            continue;
        out.glyphs += glyphs;
        const QString family = run.rawFont().familyName();
        if (!out.families.contains(family))
            out.families.append(family);
    }
    return out;
}

QByteArray describe(const QString &what, const Drawn &drawn)
{
    return QStringLiteral("%1: %2 glyph(s) from [%3]")
        .arg(what)
        .arg(drawn.glyphs)
        .arg(drawn.families.join(QStringLiteral(", ")))
        .toUtf8();
}

}

// The only emoji face here is a colour font named like Windows' that has the
// regional-indicator letters and no flag ligatures, exactly Segoe UI Emoji's shape.
class FlagEmojiFontTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        const QStringList before = QFontDatabase::families();
        QVERIFY2(!before.contains(QStringLiteral("Noto Color Emoji"), Qt::CaseInsensitive),
                 "the fontconfig isolation did not hold");
        QVERIFY(QFontDatabase::addApplicationFont(
                    QStringLiteral(SOURCE_DIR "/data/fonts/Manrope[wght].ttf")) >= 0);
        QVERIFY(QFontDatabase::addApplicationFont(
                    QStringLiteral(SOURCE_DIR "/tests/data/NoFlagColorEmoji.ttf")) >= 0);
        QVERIFY(QFontDatabase::families().contains(kFlaglessEmoji));
        const Drawn bare = draw(QFont(kFlaglessEmoji), kFrance);
        QVERIFY2(bare.glyphs == 2, describe(QStringLiteral("fixture France"), bare));
    }

    void theFlagFontShipsInTheApplicationResources()
    {
        QFile font(QStringLiteral(":/qt/qml/MatrixClient/data/fonts/TwemojiCountryFlags.ttf"));
        QVERIFY2(font.open(QIODevice::ReadOnly), "the flag font is not a Qt resource");
        QVERIFY(font.size() > 0);
    }

    void aFlaglessSystemFaceHandsFlagsToTheBundledFace()
    {
        QCOMPARE(FontManager::emojiFamily(), kFlagFamily);
        QVERIFY(QFontDatabase::families().contains(kFlagFamily));
        QVERIFY(FontManager::isNonTextFace(kFlagFamily));
    }

    void bodyTextDrawsFlagsFromTheFlagFace()
    {
        const QFont font = FontManager::withEmojiFallback(QStringLiteral("Manrope"), 32);
        for (const auto &[name, text] : { std::pair{ QStringLiteral("France"), kFrance },
                                          std::pair{ QStringLiteral("England"), kEngland } }) {
            const Drawn drawn = draw(font, text);
            QVERIFY2(drawn.glyphs == 1 && drawn.families == QStringList{ kFlagFamily },
                     describe(name, drawn));
        }
        const Drawn grin = draw(font, kGrin);
        QVERIFY2(grin.glyphs == 1 && grin.families == QStringList{ kFlaglessEmoji },
                 describe(QStringLiteral("grin"), grin));
        const Drawn letter = draw(font, QStringLiteral("A"));
        QVERIFY2(letter.families == QStringList{ QStringLiteral("Manrope") },
                 describe(QStringLiteral("A"), letter));
    }

    void theSingleQmlEmojiFamilyKeepsTheSystemFaceForOtherEmoji()
    {
        QFont font(FontManager::emojiFamily());
        font.setPixelSize(32);
        for (const auto &[name, text] : { std::pair{ QStringLiteral("France"), kFrance },
                                          std::pair{ QStringLiteral("England"), kEngland } }) {
            const Drawn drawn = draw(font, text);
            QVERIFY2(drawn.glyphs == 1 && drawn.families == QStringList{ kFlagFamily },
                     describe(name, drawn));
        }
        const Drawn grin = draw(font, kGrin);
        QVERIFY2(grin.glyphs == 1 && grin.families == QStringList{ kFlaglessEmoji },
                 describe(QStringLiteral("grin"), grin));
    }

    void onlyAFaceThatLigatesFlagsCountsAsDrawingThem()
    {
        QVERIFY(QFontDatabase::families().contains(kFlagFamily));
        QVERIFY(FontManager::drawsFlagSequences(kFlagFamily));
        QVERIFY(!FontManager::drawsFlagSequences(kFlaglessEmoji));
        QVERIFY(!FontManager::drawsFlagSequences(QStringLiteral("Manrope")));
        QVERIFY(!FontManager::drawsFlagSequences(QString()));
    }
};

int main(int argc, char **argv)
{
    // No system fonts at all, so the fixture is the only emoji face whatever
    // the machine has installed.
    QTemporaryDir fontconfig;
    if (!fontconfig.isValid())
        return 1;
    QDir().mkpath(fontconfig.filePath(QStringLiteral("fonts")));
    QFile config(fontconfig.filePath(QStringLiteral("fonts.conf")));
    if (!config.open(QIODevice::WriteOnly))
        return 1;
    config.write(QStringLiteral("<?xml version=\"1.0\"?>\n<fontconfig>\n"
                                "  <dir>%1</dir>\n  <cachedir>%2</cachedir>\n</fontconfig>\n")
                     .arg(fontconfig.filePath(QStringLiteral("fonts")),
                          fontconfig.filePath(QStringLiteral("cache")))
                     .toUtf8());
    config.close();
    qputenv("FONTCONFIG_FILE", QFile::encodeName(config.fileName()));

    QGuiApplication app(argc, argv);
    FlagEmojiFontTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "FlagEmojiFontTest.moc"
