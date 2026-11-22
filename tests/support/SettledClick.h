// Clicking an item of a just-opened Popup/Menu/Dialog: Qt places a new popup
// in the next frame's polish, so a point read in the same event-loop turn can
// be stale by the popup's offset and the press lands outside it.
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace testsupport {

// The scene point at `fraction` of `item`, once two consecutive rendered
// frames agree on it. Records a test failure and returns false otherwise.
inline bool settledScenePoint(QQuickWindow *window, QQuickItem *item, QPointF *out,
                              const QPointF &fraction = QPointF(0.5, 0.5),
                              int timeoutMs = 3000)
{
    const auto fail = [&](const QByteArray &why) {
        QTest::qFail(why.constData(), __FILE__, __LINE__);
        return false;
    };
    if (!window || !item)
        return fail("settledScenePoint: no window or no item");
    const QByteArray name = item->objectName().isEmpty()
        ? QByteArray(item->metaObject()->className())
        : item->objectName().toUtf8();
    const auto at = [&] {
        return item->mapToScene(QPointF(item->width() * fraction.x(),
                                        item->height() * fraction.y()));
    };
    QElapsedTimer timer;
    timer.start();
    QPointF last = at();
    for (int frames = 0;; ++frames) {
        QSignalSpy swapped(window, &QQuickWindow::frameSwapped);
        window->update();
        if (swapped.count() == 0 && !swapped.wait(timeoutMs))
            return fail("'" + name + "': the window rendered no frame, so its "
                        "position cannot be trusted");
        const QPointF now = at();
        if (frames > 0 && now == last) {
            *out = now;
            return true;
        }
        last = now;
        if (timer.elapsed() > timeoutMs)
            return fail("'" + name + "' never held still across two rendered frames "
                        "(last at " + QByteArray::number(now.x()) + ","
                        + QByteArray::number(now.y()) + ")");
    }
}

inline bool clickWhenSettled(QQuickWindow *window, QQuickItem *item,
                             const QPointF &fraction = QPointF(0.5, 0.5),
                             Qt::MouseButton button = Qt::LeftButton,
                             Qt::KeyboardModifiers modifiers = {})
{
    QPointF at;
    if (!settledScenePoint(window, item, &at, fraction))
        return false;
    QTest::mouseClick(window, button, modifiers, at.toPoint());
    return true;
}

} // namespace testsupport
