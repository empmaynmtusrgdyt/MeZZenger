#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <memory>
#include "DatabaseClient.h"

namespace {

void sqlExec(const QString &path, const QStringList &statements) {
    static int counter = 0;
    const QString name = QString("exec%1").arg(++counter);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        for (const QString &statement : statements) {
            QVERIFY2(query.exec(statement), qPrintable(statement + ": " + query.lastError().text()));
        }
    }
    QSqlDatabase::removeDatabase(name);
}

const QStringList OldSchema = {
    "CREATE TABLE messages (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL, user_nick TEXT NOT NULL, "
    "my_nick TEXT NOT NULL, message TEXT NOT NULL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
    "is_outgoing INTEGER NOT NULL DEFAULT 0, is_read INTEGER NOT NULL DEFAULT 0)",
    "INSERT INTO messages (user_id, user_nick, my_nick, message, is_outgoing) VALUES (2, 'bob', 'me', 'old message', 0)",
};

}

class TestDatabaseClient : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<DatabaseClient> m_db;
    QString m_previousDir;

    void closeDb() {
        m_db.reset();
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }

private slots:
    // Клиент хранит базу по относительному пути, поэтому каждый тест работает в своём каталоге
    void init() {
        m_previousDir = QDir::currentPath();
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        QVERIFY(QDir::setCurrent(m_dir->path()));
        m_db = std::make_unique<DatabaseClient>();
    }

    void cleanup() {
        closeDb();
        QDir::setCurrent(m_previousDir);
        m_dir.reset();
    }

    void usesTheGivenPath() {
        QVERIFY(m_db->initialize("custom.db"));
        QVERIFY(QFile::exists("custom.db"));
        QVERIFY(!QFile::exists("mezzenger_client.db"));
    }

    void incomingMessagesAreDeduplicatedByServerId() {
        QVERIFY(m_db->initialize("mezzenger_client.db"));
        using R = DatabaseClient::SaveResult;

        QCOMPARE(m_db->saveIncoming(2, "me", "bob", "hi", 7), R::Saved);
        QCOMPARE(m_db->saveIncoming(2, "me", "bob", "hi", 7), R::Duplicate);
        QCOMPARE(m_db->saveIncoming(2, "other", "bob", "hi", 7), R::Saved);      // другой аккаунт на этой машине
        QCOMPARE(m_db->saveIncoming(2, "me", "bob", "no id", 0), R::Saved);      // без серверного id не дедуплицируется
        QCOMPARE(m_db->saveIncoming(2, "me", "bob", "no id", 0), R::Saved);

        QCOMPARE(m_db->getMessageHistory(2, "me").size(), 3);
        QCOMPARE(m_db->getMessageHistory(2, "other").size(), 1);
    }

    void outgoingMessagesStayPendingUntilAcknowledged() {
        QVERIFY(m_db->initialize("mezzenger_client.db"));
        QVERIFY(m_db->saveOutgoing(2, "me", "bob", "first", "c1"));
        QVERIFY(m_db->saveOutgoing(2, "me", "bob", "second", "c2"));
        QVERIFY(m_db->saveOutgoing(3, "other", "carol", "foreign", "c9"));

        auto pending = m_db->getPendingOutgoing("me");
        QCOMPARE(pending.size(), 2);
        QCOMPARE(pending[0].message, QString("first"));
        QCOMPARE(pending[0].clientMsgId, QString("c1"));
        QCOMPARE(pending[0].peerNick, QString("bob"));
        QCOMPARE(pending[0].userId, 2);
        QCOMPARE(pending[1].message, QString("second"));

        auto history = m_db->getMessageHistory(2, "me");
        QCOMPARE(history.size(), 2);
        QVERIFY(history[0].pending && history[1].pending && history[0].isOutgoing);

        QVERIFY(m_db->markOutgoingSent("me", "c1", 10));
        pending = m_db->getPendingOutgoing("me");
        QCOMPARE(pending.size(), 1);
        QCOMPARE(pending[0].clientMsgId, QString("c2"));
        history = m_db->getMessageHistory(2, "me");
        QVERIFY(!history[0].pending && history[1].pending);

        QCOMPARE(m_db->getPendingOutgoing("other").size(), 1);                    // чужие очереди не смешиваются
        QCOMPARE(m_db->getPendingOutgoing("nobody").size(), 0);
    }

    void historyKeepsInsertionOrderWithinTheSameSecond() {
        QVERIFY(m_db->initialize("mezzenger_client.db"));
        for (int i = 0; i < 20; ++i) {
            QVERIFY(m_db->saveIncoming(2, "me", "bob", QString::number(i), i + 1) == DatabaseClient::SaveResult::Saved);
        }
        const auto history = m_db->getMessageHistory(2, "me");
        QCOMPARE(history.size(), 20);
        for (int i = 0; i < 20; ++i) QCOMPARE(history[i].message, QString::number(i));
    }

    void oldDatabaseIsMigratedAndKeepsHistory() {
        sqlExec("mezzenger_client.db", OldSchema);
        QVERIFY(m_db->initialize("mezzenger_client.db"));

        auto history = m_db->getMessageHistory(2, "me");
        QCOMPARE(history.size(), 1);
        QCOMPARE(history[0].message, QString("old message"));
        QVERIFY(!history[0].pending);                                              // старые записи считаются доставленными
        QVERIFY(m_db->getPendingOutgoing("me").isEmpty());

        QCOMPARE(m_db->saveIncoming(2, "me", "bob", "new", 5), DatabaseClient::SaveResult::Saved);
        QCOMPARE(m_db->saveIncoming(2, "me", "bob", "new", 5), DatabaseClient::SaveResult::Duplicate);
        QCOMPARE(m_db->getUnreadCount(2, "me"), 2);
    }

    void legacyDatabaseFileIsRenamedOnFirstStart() {
        sqlExec("messenger_client.db", OldSchema);
        QVERIFY(m_db->initialize("mezzenger_client.db"));

        QVERIFY(!QFile::exists("messenger_client.db"));
        QVERIFY(QFile::exists("mezzenger_client.db"));
        QCOMPARE(m_db->getMessageHistory(2, "me").size(), 1);
    }

    void existingNewDatabaseIsNotReplacedByLegacyOne() {
        sqlExec("messenger_client.db", OldSchema);
        QVERIFY(m_db->initialize("mezzenger_client.db"));       // переносит старую
        QVERIFY(m_db->saveOutgoing(2, "me", "bob", "mine", "c1"));
        closeDb();

        sqlExec("messenger_client.db", OldSchema);               // старый файл появился снова
        m_db = std::make_unique<DatabaseClient>();
        QVERIFY(m_db->initialize("mezzenger_client.db"));
        QVERIFY(QFile::exists("messenger_client.db"));           // второй раз не трогаем
        QCOMPARE(m_db->getPendingOutgoing("me").size(), 1);
    }

    void markReadAndUnreadCount() {
        QVERIFY(m_db->initialize("mezzenger_client.db"));
        m_db->saveIncoming(2, "me", "bob", "a", 1);
        m_db->saveIncoming(2, "me", "bob", "b", 2);
        m_db->saveOutgoing(2, "me", "bob", "mine", "c1");
        QCOMPARE(m_db->getUnreadCount(2, "me"), 2);              // свои сообщения не считаются
        QVERIFY(m_db->markMessagesAsRead(2, "me"));
        QCOMPARE(m_db->getUnreadCount(2, "me"), 0);
    }
};

QTEST_GUILESS_MAIN(TestDatabaseClient)
#include "test_database_client.moc"
