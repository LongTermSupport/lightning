#include <QtTest/QtTest>

#include <QDesktopServices>
#include <QScopeGuard>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

#include <memory>

namespace {
const QString hostile = QStringLiteral("<b>x</b><a href='https://e'>y</a>");

class OpenSpy : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *media READ media CONSTANT)
public:
    QObject *media() { return this; }
    Q_INVOKABLE void openWebUrl(const QUrl &) { ++opened; }
    int opened = 0;
public Q_SLOTS:
    void handleUrl(const QUrl &) { ++opened; }
};

void collectText(QQuickItem *item, QList<QQuickItem *> &out)
{
    if (!item)
        return;
    if (item->isVisible() && item->inherits("QQuickText"))
        out.append(item);
    for (QQuickItem *child : item->childItems())
        collectText(child, out);
}
}

class SpaceCommunityPanelQmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void hostileGuideAndProposalTextRendersLiterallyAndOpensNothing()
    {
        OpenSpy spy;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &spy, "handleUrl");
        QDesktopServices::setUrlHandler(QStringLiteral("http"), &spy, "handleUrl");
        auto cleanup = qScopeGuard([] {
            QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
            QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
        });

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &spy);
        engine.rootContext()->setContextProperty(QStringLiteral("hostile"), hostile);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient
Item {
    width: 700; height: 900
    QtObject {
        id: fake
        property string activeSpaceId: "!s:x"
        property string communityRoomId: "!s:x"
        property bool communityBusy: false
        property string communityMessage: ""
        property var communityState: ({
            guide: { welcome: hostile, rules: hostile, rooms: [], links: [hostile] },
            can_edit_guide: false, can_propose: false, can_review: true, can_enable: false,
            proposals: [{ id: "@a:x", author: "@a:x", body: hostile, revision: "$1",
                          authorized: true, published: false }] })
        property var communityReview: ({
            approval: "t", body: hostile, author: "@a:x", publisher: "@m:x", revision: "$1",
            audience: { room_id: "!t:x", name: hostile, join_rule: "invite", allow: [],
                        history: "shared", joined_count: 2 } })
        signal communityChanged()
        function loadCommunity(id) {}
        function communityAction(action, payload) {}
        function invalidateCommunityReview() {}
        function guideRooms(sections) { return [] }
        function audiencePreview(rule, history, allowed) { return "" }
    }
    SpaceCommunityPanel {
        objectName: "panelUnderTest"
        width: 660
        manager: fake
    }
}
)"), QUrl(QStringLiteral("qrc:/spacecommunitypaneltest.qml")));
        QVERIFY2(component.errors().isEmpty(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *root = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(root);
        QQuickWindow window;
        window.resize(700, 900);
        root->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto *panel = root->findChild<QQuickItem *>(QStringLiteral("panelUnderTest"));
        QVERIFY(panel);
        QVERIFY(panel->isVisible());
        QObject *dialog = panel->findChild<QObject *>(QStringLiteral("spaceAnnouncementDialog"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());

        QList<QQuickItem *> texts;
        collectText(window.contentItem(), texts);
        int exact = 0;
        int carrying = 0;
        for (QQuickItem *text : std::as_const(texts)) {
            const QString shown = text->property("text").toString();
            if (!shown.contains(QStringLiteral("<b>")))
                continue;
            ++carrying;
            if (shown == hostile)
                ++exact;
            QVERIFY2(text->property("textFormat").toInt() == Qt::PlainText,
                     qPrintable(QStringLiteral("rich or styled text: ") + shown));
            for (int y = 1; y < int(text->height()); y += 3) {
                for (int x = 1; x < int(text->width()); x += 3) {
                    QString link;
                    QVERIFY(QMetaObject::invokeMethod(text, "linkAt", Q_RETURN_ARG(QString, link),
                                                      Q_ARG(qreal, x), Q_ARG(qreal, y)));
                    QVERIFY2(link.isEmpty(), qPrintable(shown));
                }
            }
            QMetaObject::invokeMethod(text, "linkActivated", Q_ARG(QString, QStringLiteral("https://e")));
            QTest::mouseClick(&window, Qt::LeftButton, {},
                              text->mapToScene(QPointF(text->width() / 2, text->height() / 2)).toPoint());
        }
        // Welcome, rules, link and the reviewed announcement text show the value and nothing else.
        QVERIFY2(exact >= 4, qPrintable(QString::number(exact)));
        QVERIFY(carrying >= exact);
        QCoreApplication::processEvents();
        QCOMPARE(spy.opened, 0);
    }
};

QTEST_MAIN(SpaceCommunityPanelQmlTest)
#include "SpaceCommunityPanelQmlTest.moc"
