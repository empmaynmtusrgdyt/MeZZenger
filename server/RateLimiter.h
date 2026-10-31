#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <QDateTime>
#include <algorithm>
#include <functional>

using MillisClock = std::function<qint64()>;

inline qint64 systemMillis() {
    return QDateTime::currentMSecsSinceEpoch();
}

// Ключ для учёта адреса: IPv4-mapped IPv6 сводится к IPv4, остальной IPv6 - к подсети /64
// (иначе обойти лимит можно, перебирая адреса из выданного провайдером диапазона).
inline QString ipKey(const QHostAddress &address) {
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        return address.toString();
    }

    const Q_IPV6ADDR a = address.toIPv6Address();
    bool mapped = a[10] == 0xff && a[11] == 0xff;
    for (int i = 0; i < 10 && mapped; ++i) {
        mapped = a[i] == 0;
    }
    if (mapped) {
        return QString("%1.%2.%3.%4").arg(a[12]).arg(a[13]).arg(a[14]).arg(a[15]);
    }

    QString key = "v6:";
    for (int i = 0; i < 8; ++i) {
        key += QString("%1").arg(a[i], 2, 16, QLatin1Char('0'));
    }
    return key;
}

// Не более maxFailures неудач за скользящее окно windowMs для одного ключа.
class RateLimiter {
public:
    RateLimiter(int maxFailures, qint64 windowMs, MillisClock clock = systemMillis)
        : m_maxFailures(maxFailures), m_windowMs(windowMs), m_clock(std::move(clock)) {}

    bool isBlocked(const QString &key) {
        return retryAfterMs(key) > 0;
    }

    // Через сколько миллисекунд ключ снова сможет пытаться (0 - можно сейчас)
    qint64 retryAfterMs(const QString &key) {
        auto it = m_failures.find(key);
        if (it == m_failures.end()) {
            return 0;
        }
        const qint64 now = m_clock();
        prune(it.value(), now);
        if (it.value().isEmpty()) {
            m_failures.erase(it);
            return 0;
        }
        if (it.value().size() < m_maxFailures) {
            return 0;
        }
        return it.value().first() + m_windowMs - now;
    }

    void recordFailure(const QString &key) {
        const qint64 now = m_clock();
        if (m_failures.size() >= MaxTrackedKeys) {
            pruneAll(now);
            if (m_failures.size() >= MaxTrackedKeys) {
                m_failures.clear();
            }
        }
        QList<qint64> &list = m_failures[key];
        prune(list, now);
        list.append(now);
        // Хвост длиннее лимита не нужен
        while (list.size() > m_maxFailures) {
            list.removeFirst();
        }
    }

    int trackedKeys() const { return m_failures.size(); }

private:
    static constexpr int MaxTrackedKeys = 10000;

    void prune(QList<qint64> &list, qint64 now) const {
        while (!list.isEmpty() && list.first() + m_windowMs <= now) {
            list.removeFirst();
        }
    }

    void pruneAll(qint64 now) {
        for (auto it = m_failures.begin(); it != m_failures.end();) {
            prune(it.value(), now);
            it = it.value().isEmpty() ? m_failures.erase(it) : std::next(it);
        }
    }

    int m_maxFailures;
    qint64 m_windowMs;
    MillisClock m_clock;
    QHash<QString, QList<qint64>> m_failures;
};

// Ведро жетонов: до capacity сообщений сразу, дальше perSecond в секунду.
class TokenBucket {
public:
    TokenBucket(int capacity, double perSecond, MillisClock clock = systemMillis)
        : m_capacity(capacity), m_perSecond(perSecond), m_clock(std::move(clock)),
          m_tokens(capacity), m_last(m_clock()) {}

    bool tryTake() {
        const qint64 now = m_clock();
        m_tokens = std::min<double>(m_capacity, m_tokens + (now - m_last) * m_perSecond / 1000.0);
        m_last = now;
        if (m_tokens < 1.0) {
            return false;
        }
        m_tokens -= 1.0;
        return true;
    }

private:
    int m_capacity;
    double m_perSecond;
    MillisClock m_clock;
    double m_tokens;
    qint64 m_last;
};

#endif
