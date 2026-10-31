#include <QtTest>
#include "RateLimiter.h"

class TestRateLimiter : public QObject {
    Q_OBJECT

private slots:
    void blocksAfterMaxFailuresAndRecovers() {
        qint64 now = 1000000;
        RateLimiter limiter(5, 600000, [&]() { return now; });

        QVERIFY(!limiter.isBlocked("a"));
        for (int i = 0; i < 4; ++i) limiter.recordFailure("a");
        QVERIFY(!limiter.isBlocked("a"));            // 4 из 5: ещё можно
        limiter.recordFailure("a");
        QVERIFY(limiter.isBlocked("a"));             // 5 из 5: блок
        QVERIFY(!limiter.isBlocked("b"));            // другой ключ не затронут
        QCOMPARE(limiter.retryAfterMs("a"), qint64(600000));

        now += 599999;
        QVERIFY(limiter.isBlocked("a"));             // за 1 мс до конца окна
        now += 1;
        QVERIFY(!limiter.isBlocked("a"));            // окно прошло
        QCOMPARE(limiter.trackedKeys(), 0);          // запись очищена
    }

    void windowSlidesOneFailureAtATime() {
        qint64 now = 0;
        RateLimiter limiter(5, 600000, [&]() { return now; });
        for (int i = 0; i < 3; ++i) { limiter.recordFailure("s"); now += 100000; }   // t = 0, 100k, 200k
        limiter.recordFailure("s"); now += 100000;                                    // t = 300k
        limiter.recordFailure("s");                                                   // t = 400k
        QVERIFY(limiter.isBlocked("s"));
        QCOMPARE(limiter.retryAfterMs("s"), qint64(600000 - 400000));                 // выпадет самая старая (t = 0)
    }

    void memoryStaysBounded() {
        qint64 now = 0;
        RateLimiter limiter(5, 600000, [&]() { return now; });
        for (int i = 0; i < 100; ++i) limiter.recordFailure("flood");
        QVERIFY(limiter.isBlocked("flood"));

        RateLimiter many(5, 600000, [&]() { return now; });
        for (int i = 0; i < 25000; ++i) many.recordFailure(QString::number(i));
        QVERIFY(many.trackedKeys() <= 10000);
    }

    void tokenBucketAllowsBurstThenRefills() {
        qint64 t = 0;
        TokenBucket bucket(60, 20.0, [&]() { return t; });
        auto drain = [&]() { int taken = 0; for (int i = 0; i < 100; ++i) taken += bucket.tryTake(); return taken; };

        QCOMPARE(drain(), 60);                       // всплеск
        t += 1000;
        QCOMPARE(drain(), 20);                       // за секунду накапало 20
        t += 60000;
        QCOMPARE(drain(), 60);                       // не копится сверх ёмкости
    }

    void ipKeyFoldsAddresses() {
        QCOMPARE(ipKey(QHostAddress("192.168.1.5")), QString("192.168.1.5"));
        QCOMPARE(ipKey(QHostAddress("::ffff:192.168.1.5")), QString("192.168.1.5"));          // mapped == IPv4
        QCOMPARE(ipKey(QHostAddress("2001:db8:1:2:aaaa:bbbb:cccc:dddd")),
                 ipKey(QHostAddress("2001:db8:1:2:1111:2222:3333:4444")));                    // один /64
        QVERIFY(ipKey(QHostAddress("2001:db8:1:2::1")) != ipKey(QHostAddress("2001:db8:1:3::1")));
        QVERIFY(ipKey(QHostAddress("::1")) != ipKey(QHostAddress("127.0.0.1")));
    }
};

QTEST_GUILESS_MAIN(TestRateLimiter)
#include "test_rate_limiter.moc"
