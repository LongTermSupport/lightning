#include "notifications/QuietHours.h"

#include <QTimeZone>
#include <QtTest/QtTest>

namespace {
quiethours::Schedule nightly()
{
    quiethours::Schedule s;
    s.enabled = true;
    s.startMinute = 22 * 60;
    s.endMinute = 8 * 60;
    return s;
}

QDateTime at(int h, int m, const QTimeZone &zone = QTimeZone::UTC)
{
    return QDateTime(QDate(2026, 3, 29), QTime(h, m), zone);
}
}

class QuietHoursTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void aScheduleAcrossMidnightCoversBothSides()
    {
        const auto s = nightly();
        QVERIFY(!quiethours::activeAt(s, at(21, 59)));
        QVERIFY(quiethours::activeAt(s, at(22, 0)));
        QVERIFY(quiethours::activeAt(s, at(23, 59)));
        QVERIFY(quiethours::activeAt(s, at(0, 0)));
        QVERIFY(quiethours::activeAt(s, at(7, 59)));
        QVERIFY(!quiethours::activeAt(s, at(8, 0)));
        QVERIFY(!quiethours::activeAt(s, at(12, 0)));
    }

    void aDaytimeScheduleIsOneSpan()
    {
        auto s = nightly();
        s.startMinute = 9 * 60;
        s.endMinute = 17 * 60 + 30;
        QVERIFY(!quiethours::activeAt(s, at(8, 59)));
        QVERIFY(quiethours::activeAt(s, at(9, 0)));
        QVERIFY(quiethours::activeAt(s, at(17, 29)));
        QVERIFY(!quiethours::activeAt(s, at(17, 30)));
        QVERIFY(!quiethours::activeAt(s, at(23, 0)));
    }

    void anOffOrEmptyScheduleIsNeverActive()
    {
        auto s = nightly();
        s.enabled = false;
        QVERIFY(!quiethours::activeAt(s, at(23, 0)));
        s.enabled = true;
        s.endMinute = s.startMinute;
        QVERIFY(!quiethours::activeAt(s, at(23, 0)));
        QVERIFY(!quiethours::activeAt(nightly(), QDateTime()));
    }

    void theWindowFollowsTheWallClockAcrossAnOffsetChange()
    {
        // One instant, read before and after a one-hour DST shift.
        const QDateTime instant = at(20, 30);
        const auto winter = QTimeZone::fromSecondsAheadOfUtc(1 * 3600);
        const auto summer = QTimeZone::fromSecondsAheadOfUtc(2 * 3600);
        QVERIFY(!quiethours::activeAt(nightly(), instant.toTimeZone(winter)));
        QVERIFY(quiethours::activeAt(nightly(), instant.toTimeZone(summer)));
    }

    void aRealDaylightSavingNightStaysQuietThroughTheJump()
    {
        const QTimeZone vilnius("Europe/Vilnius");
        if (!vilnius.isValid())
            QSKIP("no time-zone database on this machine");
        // 2026-03-29 03:00 local jumps to 04:00; quiet 22:00-08:00 holds
        // through it and ends at 08:00 local, not at 08:00 standard time.
        const QDateTime beforeJump(QDate(2026, 3, 29), QTime(2, 30), vilnius);
        const QDateTime afterJump = beforeJump.addSecs(3600);
        QCOMPARE(afterJump.time(), QTime(4, 30));
        QVERIFY(quiethours::activeAt(nightly(), beforeJump));
        QVERIFY(quiethours::activeAt(nightly(), afterJump));
        const QDateTime eight(QDate(2026, 3, 29), QTime(8, 0), vilnius);
        QVERIFY(!quiethours::activeAt(nightly(), eight));
        QVERIFY(quiethours::activeAt(nightly(), eight.addSecs(-60)));
    }

    void exceptionsLetOnlyWhatWasChosenThrough()
    {
        auto s = nightly();
        const QString room = QStringLiteral("!r:example.org");
        QVERIFY(!quiethours::lets(s, room, true, true));
        s.allowMentions = true;
        QVERIFY(quiethours::lets(s, room, false, true));
        QVERIFY(!quiethours::lets(s, room, true, false));
        s.allowDirect = true;
        QVERIFY(quiethours::lets(s, room, true, false));
        QVERIFY(!quiethours::lets(s, room, false, false));
        s.allowedRooms << room;
        QVERIFY(quiethours::lets(s, room, false, false));
        QVERIFY(!quiethours::lets(s, QStringLiteral("!x:example.org"), false, false));
    }
};

QTEST_GUILESS_MAIN(QuietHoursTest)
#include "QuietHoursTest.moc"
