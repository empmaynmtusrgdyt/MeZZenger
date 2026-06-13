#include "ClientHandler.h"
#include "DatabaseServer.h"
#include "Protocol.h"
#include <QDebug>
#include <QJsonDocument>

ClientHandler::ClientHandler(QTcpSocket *socket, DatabaseServer *db, QObject *parent)
    : QObject(parent)
    , m_socket(socket)
    , m_database(db)
{
    connect(m_socket, &QTcpSocket::readyRead, this, &ClientHandler::readMessages);
    connect(m_socket, &QTcpSocket::disconnected, this, &ClientHandler::onDisconnected);
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::errorOccurred),
            this, &ClientHandler::onError);

    qDebug() << "Новое подключение:" << m_socket->peerAddress().toString();
}

ClientHandler::~ClientHandler() {
    if (m_socket) {
        m_socket->deleteLater();
    }
    qDebug() << "Отключение клиента:" << m_nick;
}

void ClientHandler::sendMessage(const QJsonObject &message) {
    if (!m_socket || !m_authenticated) {
        return;
    }

    QByteArray data = serializeMessage(message);
    qint64 sent = m_socket->write(data);
    if (sent < 0) {
        qWarning() << "Ошибка отправки сообщения:" << m_socket->errorString();
        return;
    }

    m_socket->flush();
    qDebug() << "Отправлено сообщение клиенту" << m_nick << ":" << message;
}

void ClientHandler::readMessages() {
    m_buffer += m_socket->readAll();

    while (m_buffer.size() >= static_cast<int>(HeaderSize)) {
        uint32_t messageLength = readHeader(m_buffer.left(HeaderSize));

        if (messageLength == 0 || messageLength > MaxMessageSize) {
            qWarning() << "Неверная длина сообщения";
            m_socket->disconnectFromHost();
            return;
        }

        if (m_buffer.size() < static_cast<int>(HeaderSize + messageLength)) {
            break;
        }

        QByteArray messageData = m_buffer.mid(HeaderSize, messageLength);
        m_buffer = m_buffer.mid(HeaderSize + messageLength);

        QJsonObject message = deserializeMessage(messageData);
        if (message.isEmpty()) {
            qWarning() << "Ошибка парсинга JSON";
            continue;
        }

        qDebug() << "Получено сообщение от клиента:" << message;
        processMessage(message);
    }
}

void ClientHandler::processMessage(const QJsonObject &message) {
    Protocol::MessageType type = ProtocolHelper::parseMessageType(message);
    qDebug() << "Тип сообщения:" << static_cast<int>(type);

    switch (type) {
        case Protocol::MessageType::Login:
            handleLogin(message);
            break;
        case Protocol::MessageType::Register:
            handleRegister(message);
            break;
        case Protocol::MessageType::Message:
            if (m_authenticated) {
                handleMessage(message);
            }
            break;
        case Protocol::MessageType::GetOfflineMessages:
            if (m_authenticated) {
                handleGetOfflineMessages();
            }
            break;
        case Protocol::MessageType::SearchUser:
            if (m_authenticated) {
                handleSearchUser(message);
            }
            break;
        case Protocol::MessageType::MarkMessagesRead:
            if (m_authenticated) {
                handleMarkMessagesRead(message);
            }
            break;
        default:
            qWarning() << "Неизвестный тип сообщения";
            break;
    }
}

void ClientHandler::handleLogin(const QJsonObject &message) {
    qDebug() << "handleLogin called";
    if (m_authenticated) return;

    QString nick = ProtocolHelper::getString(message, Protocol::NICK);
    QString password = ProtocolHelper::getString(message, Protocol::PASSWORD);
    qDebug() << "Login attempt, nick:" << nick;

    if (nick.isEmpty() || password.isEmpty()) {
        sendAuthResponse(false, "Неверный формат данных");
        return;
    }

    int userId = m_database->getUserIdByNick(nick);
    if (userId <= 0) {
        qDebug() << "User not found:" << nick;
        sendAuthResponse(false, "Такого пользователя не существует. Зарегистрируйтесь.");
        return;
    }

    if (!m_database->verifyPassword(userId, password)) {
        qDebug() << "Wrong password for:" << nick;
        sendAuthResponse(false, "Неверный пароль. Попробуйте ещё раз.");
        return;
    }

    m_userId = userId;
    m_nick = nick;
    m_authenticated = true;

    qDebug() << "Login successful for user:" << nick << "ID:" << userId;

    sendAuthResponse(true, "Успешно");
    sendUserList();

    emit authenticated(m_userId, this);
}

void ClientHandler::handleRegister(const QJsonObject &message) {
    qDebug() << "handleRegister called";
    if (m_authenticated) return;

    QString nick = ProtocolHelper::getString(message, Protocol::NICK);
    QString password = ProtocolHelper::getString(message, Protocol::PASSWORD);
    qDebug() << "Register attempt, nick:" << nick;

    if (nick.isEmpty() || password.isEmpty()) {
        sendAuthResponse(false, "Неверный формат данных");
        return;
    }

    if (m_database->userExists(nick)) {
        qDebug() << "User already exists:" << nick;
        sendAuthResponse(false, "Пользователь уже существует");
        return;
    }

    int userId = m_database->createUser(nick, password);
    if (userId <= 0) {
        qDebug() << "Failed to create user:" << nick;
        sendAuthResponse(false, "Ошибка регистрации");
        return;
    }

    m_userId = userId;
    m_nick = nick;
    m_authenticated = true;

    qDebug() << "Registration successful for user:" << nick << "ID:" << userId;

    sendAuthResponse(true, "Успешно");
    sendUserList();

    emit authenticated(m_userId, this);
}

void ClientHandler::handleMessage(const QJsonObject &message) {
    if (!m_authenticated) return;

    QString toNick = ProtocolHelper::getString(message, Protocol::TO_NICK);
    QString text = ProtocolHelper::getString(message, Protocol::TEXT);

    if (toNick.isEmpty() || text.isEmpty()) return;

    int toUserId = m_database->getUserIdByNick(toNick);
    if (toUserId <= 0) return;

    int messageId = m_database->saveMessage(m_userId, toUserId, text);
    forwardMessage(m_userId, toUserId, messageId, text);
}

void ClientHandler::sendAuthResponse(bool success, const QString &message) {
    QJsonObject response = ProtocolHelper::createAuthResponse(success, message);
    sendMessage(response);
}

void ClientHandler::sendUserList() {
    QList<QJsonObject> users = m_database->getUsersWithConversations(m_userId);
    qDebug() << "Sending conversation partners to" << m_nick << ", count:" << users.size();
    QJsonObject response = ProtocolHelper::createUserList(users);
    sendMessage(response);
}

void ClientHandler::deliverOfflineMessages() {
    QList<QJsonObject> messages = m_database->getUndeliveredMessages(m_userId);
    for (const auto &msg : messages) {
        int fromUserId = msg["from_user_id"].toInt();
        QString messageText = msg["message"].toString();
        QString timestamp = msg["timestamp"].toString();

        QString fromNick = m_database->getNickById(fromUserId);
        if (fromNick.isEmpty()) continue;

        QJsonObject response = ProtocolHelper::createMessageWithTimestamp(fromNick, messageText, timestamp);
        sendMessage(response);
    }

    QList<int> messageIds;
    for (const auto &msg : messages) messageIds.append(msg["id"].toInt());
    if (!messageIds.isEmpty()) {
        m_database->markMessagesAsDelivered(m_userId, messageIds);
    }
}

void ClientHandler::handleGetOfflineMessages() {
    QList<QJsonObject> rawMessages = m_database->getUndeliveredMessages(m_userId);
    qDebug() << "Отправка offline-сообщений для" << m_nick << ", количество:" << rawMessages.size();

    QList<int> messageIds;
    QList<QJsonObject> messages;
    for (const auto &msg : rawMessages) {
        int fromUserId = msg["from_user_id"].toInt();
        QString fromNick = m_database->getNickById(fromUserId);
        if (fromNick.isEmpty()) continue;

        messageIds.append(msg["id"].toInt());

        QJsonObject resolved;
        resolved["from_nick"] = fromNick;
        resolved["message"] = msg["message"];
        resolved["timestamp"] = msg["timestamp"];
        messages.append(resolved);
    }

    QJsonObject response = ProtocolHelper::createMessageHistory(messages);
    sendMessage(response);

    if (!messageIds.isEmpty()) {
        m_database->markMessagesAsDelivered(m_userId, messageIds);
    }
}

void ClientHandler::handleSearchUser(const QJsonObject &message) {
    QString nick = ProtocolHelper::getString(message, Protocol::NICK);
    qDebug() << "Search user request for:" << nick;

    if (nick.isEmpty()) {
        QJsonObject response = ProtocolHelper::createSearchUserResponse(false, QJsonObject(), "Введите ник пользователя");
        sendMessage(response);
        return;
    }

    QJsonObject user = m_database->searchUser(nick);
    if (user.isEmpty()) {
        QJsonObject response = ProtocolHelper::createSearchUserResponse(false, QJsonObject(), "Пользователь не найден");
        sendMessage(response);
        return;
    }

    QJsonObject response = ProtocolHelper::createSearchUserResponse(true, user);
    sendMessage(response);
}

void ClientHandler::handleMarkMessagesRead(const QJsonObject &message) {
    int fromUserId = message[Protocol::ID].toInt();
    qDebug() << "Mark messages as read from" << fromUserId << "to" << m_userId;

    if (fromUserId > 0) {
        m_database->markMessagesAsRead(fromUserId, m_userId);
    }
}

void ClientHandler::forwardMessage(int fromUserId, int toUserId, int messageId, const QString &text) {
    QJsonObject message = ProtocolHelper::createMessageWithTimestamp(
        m_nick, text, QDateTime::currentDateTime().toString(Qt::ISODate)
    );
    emit messageReady(toUserId, messageId, message);
}

void ClientHandler::onDisconnected() {
    qDebug() << "Клиент отключился:" << m_nick;
    emit clientDisconnected(m_userId);
}

void ClientHandler::onError() {
    qWarning() << "Ошибка соединения:" << m_socket->errorString();
}
