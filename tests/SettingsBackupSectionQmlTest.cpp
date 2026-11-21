#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

QObject *findNamed(QQuickItem *item, const QString &name)
{
    if (!item)
        return nullptr;
    if (item->objectName() == name)
        return item;
    for (QQuickItem *child : item->childItems()) {
        if (QObject *hit = findNamed(child, name))
            return hit;
    }
    return nullptr;
}

}

class SettingsBackupSectionQmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aDroppedActiveThemeIsShownInTheImportPreview()
    {
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        warnings << e.toString();
                });
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("SettingsBackupSection"));
        if (created.isEmpty())
            QVERIFY(created.wait(8000));
        QObject *root = engine.rootObjects().value(0);
        QVERIFY2(root, qPrintable(warnings.join(QLatin1Char('\n'))));

        const QString name = QStringLiteral("importPreviewActiveThemeDropped");
        QObject *line = root->findChild<QObject *>(name);
        if (!line) {
            QObject *dialog = root->findChild<QObject *>(QStringLiteral("importSettingsPreview"));
            QVERIFY(dialog);
            line = findNamed(dialog->property("contentItem").value<QQuickItem *>(), name);
        }
        QVERIFY(line);
        QVERIFY(line->property("text").toString().contains(QStringLiteral("theme")));

        QVERIFY(!root->property("previewActiveThemeDropped").toBool());
        QVERIFY(root->setProperty("preview",
                                  QVariantMap{ { QStringLiteral("ok"), true },
                                               { QStringLiteral("activeThemeDropped"), true } }));
        QVERIFY(root->property("previewActiveThemeDropped").toBool());
        QVERIFY(root->setProperty("preview",
                                  QVariantMap{ { QStringLiteral("ok"), true } }));
        QVERIFY(!root->property("previewActiveThemeDropped").toBool());
    }
};

QTEST_MAIN(SettingsBackupSectionQmlTest)
#include "SettingsBackupSectionQmlTest.moc"
