#include "ServerCore.h"
#include "DatabaseServer.h"
#include "ClientHandler.h"
#include <QDebug>

ServerCore::ServerCore(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &ServerCore::onNewConnection);
}

ServerCore::~ServerCore() {
    stop();
}

bool ServerCore::start(quint16 port) {
    if (!m_database.initialize("server.db")) {
        qCritical() << "Не удалось инициализировать базу данных";
        return false;
    }

    if (!m_server.listen(QHostAddress::Any, port)) {
        qCritical() << "Не удалось запустить сервер:" << m_server.errorString();
        return false;
    }

    qDebug() << "Сервер запущен на порту" << port;
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
