#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <memory>
#include <QPasswordDigestor>
#include "DatabaseServer.h"
#include "Protocol.h"

namespace {

// Второе подключение к тому же файлу: смотрим и подменяем содержимое в обход DatabaseServer
QVariant sqlValue(const QString &path, const QString &sql) {
    static int counter = 0;
    const QString name = QString("peek%1").arg(++counter);
    QVariant result;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(sql) && query.next()) {
                result = query.value(0);
            }
        }
    }
    QSqlDatabase::removeDatabase(name);
    return result;
}

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

QString sha256Hex(const QString &text) {
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

}

class TestDatabaseServer : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<DatabaseServer> m_db;
    QString m_path;

    void closeDb() {
        m_db.reset();
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }

private slots:
    void init() {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_path = m_dir->filePath("server.db");
        m_db = std::make_unique<DatabaseServer>();
        QVERIFY(m_db->initialize(m_path));
    }

    void cleanup() {
        closeDb();
        m_dir.reset();
    }

    // ---- пароли

    void passwordsAreStoredAsSaltedPbkdf2() {
        const int alice = m_db->createUser("alice", "secret1");
        const int bob = m_db->createUser("bob", "secret1");
        QVERIFY(alice > 0 && bob > 0);

        const QString h1 = sqlValue(m_path, "SELECT password_hash FROM users WHERE nickname='alice'").toString();
        const QString h2 = sqlValue(m_path, "SELECT password_hash FROM users WHERE nickname='bob'").toString();
        QVERIFY(h1.startsWith("pbkdf2-sha256$600000$"));
        QCOMPARE(h1.split('$').size(), 4);
        QVERIFY(!h1.contains("secret1"));
        QVERIFY2(h1 != h2, "одинаковые пароли должны давать разные хеши (соль)");

        QVERIFY(m_db->verifyPassword(alice, "secret1"));
        QVERIFY(!m_db->verifyPassword(alice, "secret2"));
        QVERIFY(!m_db->verifyPassword(alice, ""));
        QVERIFY(!m_db->verifyPassword(-1, "secret1"));
        QVERIFY(!m_db->verifyPassword(alice, QString(Protocol::MaxPasswordLength + 1, 'x')));
    }

    void legacySha256HashIsUpgradedOnlyOnSuccessfulLogin() {
        sqlExec(m_path, {QString("INSERT INTO users (nickname, password_hash) VALUES ('old', '%1')").arg(sha256Hex("oldpass"))});
        const int id = m_db->getUserIdByNick("old");
        QVERIFY(id > 0);
        const auto hash = [&]() { return sqlValue(m_path, "SELECT password_hash FROM users WHERE nickname='old'").toString(); };

        QVERIFY(!m_db->verifyPassword(id, "wrong"));
        QCOMPARE(hash(), sha256Hex("oldpass"));                 // не тронут

        QVERIFY(m_db->verifyPassword(id, "oldpass"));
        QVERIFY(hash().startsWith("pbkdf2-sha256$600000$"));    // обновлён

        QVERIFY(m_db->verifyPassword(id, "oldpass"));           // новый хеш работает
        QVERIFY(!m_db->verifyPassword(id, "wrong"));
    }

    void weakerIterationCountIsUpgraded() {
        // Запись с малым числом итераций (как после будущего усиления параметров) обновляется при входе
        const int id = m_db->createUser("carol", "secret1");
        const QByteArray salt = "0123456789abcdef";
        const QByteArray weak = QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, "secret1", salt, 1000, 32);
        sqlExec(m_path, {QString("UPDATE users SET password_hash='pbkdf2-sha256$1000$%1$%2' WHERE id=%3")
                             .arg(QString::fromLatin1(salt.toBase64()), QString::fromLatin1(weak.toBase64())).arg(id)});

        QVERIFY(m_db->verifyPassword(id, "secret1"));
        QVERIFY(sqlValue(m_path, QString("SELECT password_hash FROM users WHERE id=%1").arg(id)).toString()
                    .startsWith("pbkdf2-sha256$600000$"));
        QVERIFY(m_db->verifyPassword(id, "secret1"));
    }

    void unknownHashFormatNeverMatches() {
        const int id = m_db->createUser("dave", "secret1");
        sqlExec(m_path, {QString("UPDATE users SET password_hash='garbage' WHERE id=%1").arg(id)});
        QVERIFY(!m_db->verifyPassword(id, "secret1"));
        QVERIFY(!m_db->verifyPassword(id, "garbage"));
    }

    void duplicateNicknameIsRejected() {
        QVERIFY(m_db->createUser("eve", "secret1") > 0);
        QCOMPARE(m_db->createUser("eve", "other-pass"), -1);
    }

    // ---- приглашения

    void inviteCodeFormatAndStorage() {
        const QString code = m_db->createInvite();
        QVERIFY2(QRegularExpression("^[A-HJKMNP-TV-Z2-9]{4}(-[A-HJKMNP-TV-Z2-9]{4}){3}$").match(code).hasMatch(),
                 qPrintable(code));
        QVERIFY(m_db->createInvite() != code);

        const QString stored = sqlValue(m_path, "SELECT code_hash FROM invites ORDER BY rowid LIMIT 1").toString();
        QCOMPARE(stored.size(), 64);
        QVERIFY(stored != code && !stored.contains(code.left(4)));
    }

    void inviteMatchingIgnoresCaseAndSeparators() {
        const QString code = m_db->createInvite();
        QVERIFY(m_db->inviteAvailable(code));
        QVERIFY(m_db->inviteAvailable(code.toLower()));
        QVERIFY(m_db->inviteAvailable(QString(code).replace('-', ' ')));
        QVERIFY(m_db->inviteAvailable("  " + code + "  "));
        QVERIFY(!m_db->inviteAvailable(code + "X"));
        QVERIFY(!m_db->inviteAvailable(code.left(code.size() - 1)));
        QVERIFY(!m_db->inviteAvailable(""));
        QVERIFY(!m_db->inviteAvailable("----"));
        QVERIFY(!m_db->inviteAvailable("AAAA-BBBB-CCCC-DDDD"));
    }

    void inviteIsSingleUseAndSurvivesTakenNick() {
        const QString first = m_db->createInvite();
        const QString second = m_db->createInvite();

        QVERIFY(m_db->createUserWithInvite("frank", "secret1", first) > 0);
        QVERIFY(!m_db->inviteAvailable(first));
        QCOMPARE(m_db->createUserWithInvite("grace", "secret1", first), -1);    // код уже использован
        QCOMPARE(m_db->getUserIdByNick("grace"), -1);

        QCOMPARE(m_db->createUserWithInvite("frank", "secret1", second), -1);   // ник занят
        QVERIFY2(m_db->inviteAvailable(second), "код не должен сгорать при неудачной регистрации");
        QVERIFY(m_db->createUserWithInvite("grace", "secret1", second) > 0);
    }

    // ---- сообщения

    void clientMessageIdDeduplicatesPerSender() {
        const int a = m_db->createUser("a", "secret1");
        const int b = m_db->createUser("b", "secret1");
        bool duplicate = true;

        const int id1 = m_db->saveMessage(a, b, "hi", "cid-1", &duplicate);
        QVERIFY(id1 > 0);
        QVERIFY(!duplicate);

        QCOMPARE(m_db->saveMessage(a, b, "hi", "cid-1", &duplicate), id1);
        QVERIFY(duplicate);

        const int other = m_db->saveMessage(b, a, "yo", "cid-1", &duplicate);       // тот же id, другой отправитель
        QVERIFY(other > 0 && other != id1);
        QVERIFY(!duplicate);

        const int n1 = m_db->saveMessage(a, b, "x", QString(), &duplicate);          // без client_msg_id не дедуплицируется
        const int n2 = m_db->saveMessage(a, b, "x", QString(), &duplicate);
        QVERIFY(n1 > 0 && n2 > 0 && n1 != n2);
        QVERIFY(!duplicate);

        QCOMPARE(m_db->saveMessage(0, b, "bad"), -1);
        QCOMPARE(m_db->saveMessage(a, -5, "bad"), -1);
    }

    void onlyTheRecipientCanMarkMessagesDelivered() {
        const int a = m_db->createUser("a", "secret1");
        const int b = m_db->createUser("b", "secret1");
        const int m1 = m_db->saveMessage(a, b, "one", "c1");
        const int m2 = m_db->saveMessage(a, b, "two", "c2");

        QVERIFY(m_db->markMessagesAsDelivered(a, {m1}));       // отправитель не может подтвердить чужую доставку
        QCOMPARE(m_db->getUndeliveredMessages(b).size(), 2);

        QVERIFY(m_db->markMessagesAsDelivered(b, {m1}));
        const auto left = m_db->getUndeliveredMessages(b);
        QCOMPARE(left.size(), 1);
        QCOMPARE(left[0]["id"].toInt(), m2);
        QCOMPARE(left[0]["message"].toString(), QString("two"));
        QCOMPARE(left[0]["from_user_id"].toInt(), a);
    }

    void undeliveredComeInIdOrderInBatchesOf100() {
        const int a = m_db->createUser("a", "secret1");
        const int b = m_db->createUser("b", "secret1");
        for (int i = 0; i < 130; ++i) {
            QVERIFY(m_db->saveMessage(a, b, QString::number(i), QString("c%1").arg(i)) > 0);
        }

        auto batch = m_db->getUndeliveredMessages(b);
        QCOMPARE(batch.size(), 100);
        for (int i = 0; i < 100; ++i) QCOMPARE(batch[i]["message"].toString(), QString::number(i));

        QList<int> ids;
        for (const auto &m : batch) ids.append(m["id"].toInt());
        QVERIFY(m_db->markMessagesAsDelivered(b, ids));

        batch = m_db->getUndeliveredMessages(b);
        QCOMPARE(batch.size(), 30);
        QCOMPARE(batch.first()["message"].toString(), QString("100"));
    }

    // ---- миграция схемы

    void oldDatabaseIsMigratedAndKeepsData() {
        closeDb();
        const QString oldPath = m_dir->filePath("old.db");
        sqlExec(oldPath, {
            "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, nickname TEXT UNIQUE NOT NULL, password_hash TEXT NOT NULL)",
            "CREATE TABLE messages (id INTEGER PRIMARY KEY AUTOINCREMENT, from_user_id INTEGER, to_user_id INTEGER, "
            "message TEXT, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, delivered INTEGER DEFAULT 0)",
            QString("INSERT INTO users (nickname, password_hash) VALUES ('u1', '%1'), ('u2', '%1')").arg(sha256Hex("pw")),
            "INSERT INTO messages (from_user_id, to_user_id, message, delivered) VALUES (1, 2, 'old message', 0)",
        });

        m_db = std::make_unique<DatabaseServer>();
        QVERIFY(m_db->initialize(oldPath));

        const auto undelivered = m_db->getUndeliveredMessages(2);
        QCOMPARE(undelivered.size(), 1);
        QCOMPARE(undelivered[0]["message"].toString(), QString("old message"));

        bool duplicate = true;
        const int id = m_db->saveMessage(1, 2, "new", "cid", &duplicate);
        QVERIFY(id > 0 && !duplicate);
        QCOMPARE(m_db->saveMessage(1, 2, "new", "cid", &duplicate), id);
        QVERIFY(duplicate);

        QVERIFY(m_db->markMessagesAsRead(1, 2));                                    // колонка read добавлена
        QVERIFY(m_db->verifyPassword(1, "pw"));                                     // старые пароли работают
        QVERIFY(m_db->createInvite().size() > 0);                                   // таблица invites создана
    }
};

QTEST_GUILESS_MAIN(TestDatabaseServer)
#include "test_database_server.moc"
