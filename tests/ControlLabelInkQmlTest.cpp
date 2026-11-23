#include <QtTest/QtTest>

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include <memory>

#include "app/AppController.h"
#include "auth/AuthManager.h"

namespace {

const char kScene[] = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 900
    height: 900
    visible: true
    function setTextScale(s) { AppTheme.textScale = s }
    Column {
        spacing: 8
        ChatBackgroundEditor { width: 520; scopeKind: "default"; showTitle: false }
        Row {
            spacing: 8
            AppButton { objectName: "smIcon"; size: "sm"; kind: "primary"; iconName: "image"; text: "Choose picture…" }
            AppButton { objectName: "mdIcon"; iconName: "check"; text: "Save changes" }
            AppButton { objectName: "lgIcon"; size: "lg"; kind: "primary"; iconName: "send"; text: "Send" }
            AppButton { objectName: "sm"; size: "sm"; text: "Cancel" }
            AppButton { objectName: "md"; kind: "danger"; text: "Remove" }
        }
        Row {
            spacing: 8
            SegmentedControl { objectName: "seg"; model: ["Day", "Week"]; current: "Day" }
            SegmentedControl { objectName: "segDense"; dense: true; model: ["Hide", "Show"]; current: "Show" }
            AppComboBox { objectName: "combo"; width: 200; model: ["Automatic", "Manual"] }
            StatusChip { objectName: "chip"; iconName: "verified_user"; label: "Verified" }
            VoiceSpeedChip {}
        }
    }
}
)QML";

const QList<int> kTextScales = { 75, 80, 85, 90, 95, 100, 105, 110, 115,
                                 120, 125, 130, 135, 140, 145, 150 };

QQuickItem *findItem(QQuickItem *p, const QString &name)
{
    if (!p)
        return nullptr;
    if (p->objectName() == name)
        return p;
    for (QQuickItem *c : p->childItems())
        if (QQuickItem *hit = findItem(c, name))
            return hit;
    return nullptr;
}

QQuickItem *contentOf(QQuickItem *control)
{
    return control ? control->property("contentItem").value<QQuickItem *>() : nullptr;
}

// Cap-height ink centre of a single-line label, in the control's coordinates.
qreal capInkY(QQuickItem *label, QQuickItem *control)
{
    const QFont font = label->property("font").value<QFont>();
    const QRectF cap = QFontMetricsF(font).tightBoundingRect(QStringLiteral("H"));
    const qreal y = label->property("baselineOffset").toReal() + cap.center().y();
    return label->mapToItem(control, QPointF(0, y)).y();
}

// Material Symbols glyphs are drawn on a square em grid: the em box centre is the
// designed centre (a tight rect of a hinted glyph is not).
qreal iconEmY(QQuickItem *icon, QQuickItem *control)
{
    const QFont font = icon->property("font").value<QFont>();
    const qreal y = icon->property("baselineOffset").toReal() - font.pixelSize() / 2.0;
    return icon->mapToItem(control, QPointF(0, y)).y();
}

struct Target {
    QString name;
    QQuickItem *control = nullptr;
    QQuickItem *label = nullptr;
    QQuickItem *icon = nullptr;
};

}

class ControlLabelInkQmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void labelInkSitsOnTheControlsCentreAtEveryTextSize()
    {
        for (const char *font : { "MaterialSymbolsRounded-subset.ttf", "Manrope[wght].ttf" })
            QFontDatabase::addApplicationFont(
                QStringLiteral(":/qt/qml/MatrixClient/data/fonts/") + QLatin1String(font));

        AppController controller(AppController::MockBackend);
        QSignalSpy loggedIn(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"), QStringLiteral("unused"));
        QVERIFY(loggedIn.wait(3000));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(kScene, QUrl(QStringLiteral("qrc:/control-label-ink.qml")));
        QVERIFY2(component.errors().isEmpty(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *win = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(win);
        QVERIFY(QTest::qWaitForWindowExposed(win));

        const qreal dpr = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1")).toDouble();
        QCOMPARE(win->devicePixelRatio(), dpr);

        QQuickItem *root = win->contentItem();
        QList<Target> targets;
        auto addButton = [&](const QString &name, QQuickItem *b) {
            QQuickItem *icon = findItem(b, QStringLiteral("buttonIcon"));
            targets.append({ name, b, findItem(b, QStringLiteral("buttonLabel")),
                             icon && icon->isVisible() ? icon : nullptr });
        };
        QQuickItem *choose = findItem(root, QStringLiteral("chatBackgroundChoose"));
        QVERIFY(choose);
        QCOMPARE(choose->property("text").toString(), QStringLiteral("Choose picture…"));
        addButton(QStringLiteral("chatBackgroundChoose"), choose);
        for (const char *n : { "smIcon", "mdIcon", "lgIcon", "sm", "md" })
            addButton(QLatin1String(n), findItem(root, QLatin1String(n)));
        for (const char *n : { "seg_Day", "seg_Week", "segDense_Hide", "segDense_Show",
                               "combo", "voiceSpeedChip" }) {
            QQuickItem *c = findItem(root, QLatin1String(n));
            targets.append({ QLatin1String(n), c, contentOf(c), nullptr });
        }
        QQuickItem *chip = findItem(root, QStringLiteral("chip"));
        targets.append({ QStringLiteral("chip"), chip,
                         findItem(chip, QStringLiteral("chipLabel")),
                         findItem(chip, QStringLiteral("chipIcon")) });

        int iconCount = 0;
        for (const Target &t : std::as_const(targets)) {
            QVERIFY2(t.control && t.label, qPrintable(t.name + u" not found"));
            QVERIFY2(t.control->isVisible() && t.label->isVisible()
                         && !t.label->property("text").toString().isEmpty(),
                     qPrintable(t.name + u": the fixture must show a labelled control"));
            if (t.icon)
                ++iconCount;
        }
        QCOMPARE(targets.size(), 13);
        QCOMPARE(iconCount, 5);

        int steps = 0;
        int checks = 0;
        QStringList misses;
        for (int scale : kTextScales) {
            QMetaObject::invokeMethod(owner.get(), "setTextScale", Q_ARG(QVariant, scale / 100.0));
            QTest::qWait(30);
            ++steps;
            for (const Target &t : std::as_const(targets)) {
                const qreal centre = t.control->height() / 2.0;
                const qreal ink = capInkY(t.label, t.control);
                // Device pixels: a half-pixel error at 1x is a whole one at 2x.
                const qreal labelOff = (ink - centre) * dpr;
                qreal iconOff = 0;
                qreal pairOff = 0;
                if (t.icon) {
                    const qreal em = iconEmY(t.icon, t.control);
                    iconOff = (em - centre) * dpr;
                    pairOff = (ink - em) * dpr;
                }
                ++checks;
                if (qAbs(labelOff) > 1.0 || qAbs(iconOff) > 1.0 || qAbs(pairOff) > 1.0)
                    misses << QStringLiteral("%1@%2%: label %3, icon %4, label-icon %5 device px")
                                  .arg(t.name).arg(scale).arg(labelOff, 0, 'f', 2)
                                  .arg(iconOff, 0, 'f', 2).arg(pairOff, 0, 'f', 2);
            }
        }
        QCOMPARE(steps, 16);
        QCOMPARE(checks, 16 * 13);
        QVERIFY2(misses.isEmpty(), qPrintable(misses.join(u"\n")));
    }
};

QTEST_MAIN(ControlLabelInkQmlTest)
#include "ControlLabelInkQmlTest.moc"
