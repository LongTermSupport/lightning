#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QThread>

#include <memory>

#include "app/GuiStallTracer.h"

class GuiStallTraceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        stalltrace::install(200);
    }

    void cleanupTestCase() { stalltrace::shutdown(); }

    void healthyLoopRecordsNothing()
    {
        QTest::qWait(400);
        QCOMPARE(stalltrace::stallCount(), 0);
    }

    void blockedLoopIsDetectedAndAttributed()
    {
        QTest::qWait(100);
        {
            stalltrace::Scope scope("test-block");
            QThread::msleep(500);
        }
        QTRY_COMPARE_WITH_TIMEOUT(stalltrace::stallCount(), 1, 2000);
        QVERIFY2(stalltrace::lastStallMs() >= 400,
                 qPrintable(QStringLiteral("measured %1 ms")
                                .arg(stalltrace::lastStallMs())));
        QCOMPARE(stalltrace::lastStallCategory(),
                 QByteArrayLiteral("test-block"));
    }

    void unattributedBlockReportsEmptyCategory()
    {
        QTest::qWait(100);
        QThread::msleep(500);
        QTRY_COMPARE_WITH_TIMEOUT(stalltrace::stallCount(), 2, 2000);
        QVERIFY(stalltrace::lastStallMs() >= 400);
        QCOMPARE(stalltrace::lastStallCategory(), QByteArray());
    }

    void nestedScopeRestoresOuterCategory()
    {
        QTest::qWait(100);
        {
            stalltrace::Scope outer("outer-section");
            {
                stalltrace::Scope inner("inner-section");
                QThread::msleep(500);
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(stalltrace::stallCount(), 3, 2000);
        QCOMPARE(stalltrace::lastStallCategory(),
                 QByteArrayLiteral("inner-section"));
    }

    void guiWorkIsCountedOnlyOnTheGuiThread()
    {
        stalltrace::resetGuiWork();
        stalltrace::noteGuiWork("probe", 100);
        stalltrace::noteGuiWork("probe", 23);
        std::unique_ptr<QThread> worker(QThread::create(
            [] { stalltrace::noteGuiWork("probe", 1000); }));
        worker->start();
        QVERIFY(worker->wait(5000));
        QCOMPARE(stalltrace::guiWorkCalls("probe"), 2);
        QCOMPARE(stalltrace::guiWorkBytes("probe"), qint64(123));
        QCOMPARE(stalltrace::guiWorkCalls("other"), 0);
        stalltrace::resetGuiWork();
        QCOMPARE(stalltrace::guiWorkBytes("probe"), qint64(0));
    }
};

QTEST_GUILESS_MAIN(GuiStallTraceTest)
#include "GuiStallTraceTest.moc"
