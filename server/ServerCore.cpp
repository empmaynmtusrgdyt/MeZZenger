#include "ServerCore.h"
#include "DatabaseServer.h"
#include "ClientHandler.h"
#include <QDebug>
#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QCryptographicHash>

ServerCore::ServerCore(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::pendingConnectionAvailable, this, &ServerCore::onNewConnection);
    connect(&m_server, &QSslServer::sslErrors, this, [](QSslSocket *socket, const QList<QSslError> &errors) {
        qWarning() << "Ошибка TLS от" << socket->peerAddress().toString() << ":" << errors;
    });
    connect(&m_server, &QSslServer::errorOccurred, this, [](QSslSocket *socket, QAbstractSocket::SocketError) {
        qWarning() << "Не удалось установить TLS-соединение с" << socket->peerAddress().toString()
                   << ":" << socket->errorString();
    });
}

ServerCore::~ServerCore() {
    stop();
}

bool ServerCore::start(quint16 port, const QString &certPath, const QString &keyPath) {
    if (!QSslSocket::supportsSsl()) {
        qCritical() << "В этой сборке Qt нет поддержки TLS";
        return false;
    }

    QFile certFile(certPath);
    QFile keyFile(keyPath);
    if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) {
        qCritical().noquote() << "Не найден сертификат или ключ TLS (" << certPath << "," << keyPath << ")."
                              << "Создайте их командой: scripts/gen-cert.sh";
        return false;
    }

    QSslCertificate certificate(&certFile, QSsl::Pem);
    QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem);
    if (key.isNull()) {
        keyFile.seek(0);
        key = QSslKey(&keyFile, QSsl::Ec, QSsl::Pem);
    }
    if (certificate.isNull() || key.isNull()) {
        qCritical() << "Сертификат или ключ TLS повреждены или в неподдерживаемом формате";
        return false;
    }

    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setLocalCertificate(certificate);
    tls.setPrivateKey(key);
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    tls.setPeerVerifyMode(QSslSocket::VerifyNone);
    m_server.setSslConfiguration(tls);
    m_server.setHandshakeTimeout(5000);

    if (!m_database.initialize("server.db")) {
        qCritical() << "Не удалось инициализировать базу данных";
        return false;
    }

    if (!m_server.listen(QHostAddress::Any, port)) {
        qCritical() << "Не удалось запустить сервер:" << m_server.errorString();
        return false;
    }

    qDebug() << "Сервер запущен на порту" << port << "(TLS)";
    qDebug().noquote() << "Отпечаток сертификата SHA-256:"
                       << QCryptographicHash::hash(certificate.toDer(), QCryptographicHash::Sha256).toHex(':').toUpper();
    qDebug() << "База данных: server.db";
    return true;
}

void ServerCore::stop() {
    for (auto *client : m_clients) {
        client->deleteLater();
    }
    m_clients.clear();
    m_server.close();
    qDebug() << "Сервер остановлен";
}

void ServerCore::onNewConnection() {
    while (m_server.hasPendingConnections()) {
        QTcpSocket *socket = m_server.nextPendingConnection();
        ClientHandler *handler = new ClientHandler(socket, &m_database, this);

        connect(handler, &ClientHandler::messageReady, this, [this](int toUserId, int messageId, const QJsonObject &message) {
            sendMessageToUser(toUserId, messageId, message);
        });

        connect(handler, &ClientHandler::clientDisconnected, this, [this, handler](int userId) {
            if (m_clients.contains(userId) && m_clients[userId] == handler) {
                m_clients.remove(userId);
                qDebug() << "Клиент" << userId << "удалён из карты";
            }
        });

        connect(handler, &ClientHandler::authenticated, this, [this](int userId, ClientHandler *handler) {
            QMutableMapIterator<int, ClientHandler*> it(m_clients);
            while (it.findNext(handler)) {
                it.remove();
            }
            m_clients[userId] = handler;
            qDebug() << "Клиент" << userId << "аутентифицирован и добавлен в карту";
        });

        m_clients.insert(-1, handler);
    }
}

void ServerCore::sendMessageToUser(int userId, int messageId, const QJsonObject &message) {
    if (m_clients.contains(userId)) {
        ClientHandler *handler = m_clients[userId];
        if (handler->isAuthenticated()) {
            handler->sendMessage(message);
            if (messageId > 0) {
                m_database.markMessagesAsDelivered(userId, {messageId});
            }
            qDebug() << "Сообщение доставлено пользователю" << userId;
            return;
        }
    }
    qDebug() << "Пользователь" << userId << "оффлайн, сообщение сохранено в БД";
}
