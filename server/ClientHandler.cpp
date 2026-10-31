#include "ClientHandler.h"
#include "DatabaseServer.h"
#include "Protocol.h"
#include "ServerLimits.h"
#include <QDebug>
#include <QJsonDocument>
#include <QChar>

namespace {

// Описание проблемы с ником или пустая строка, если ник допустим
QString nickProblem(const QString &nick) {
    if (nick.size() < Protocol::MinNickLength || nick.size() > Protocol::MaxNickLength) {
        return QString("Ник должен быть от %1 до %2 символов").arg(Protocol::MinNickLength).arg(Protocol::MaxNickLength);
    }
    if (nick != nick.trimmed()) {
        return "Ник не должен начинаться или заканчиваться пробелом";
    }
    for (uint codePoint : nick.toUcs4()) {
        if (!QChar::isPrint(codePoint)) {
            return "Ник содержит недопустимые символы";
        }
    }
    return QString();
}

}

ClientHandler::ClientHandler(QTcpSocket *socket, DatabaseServer *db, RateLimiter *authLimiter,
                             QObject *parent)
    : QObject(parent)
    , m_socket(socket)
    , m_database(db)
    , m_authLimiter(authLimiter)
    , m_ipKey(ipKey(socket->peerAddress()))
    , m_bucket(Limits::MessageBurst, Limits::MessagesPerSecond)
    , m_authTimer(new QTimer(this))
{
    // Соединение без входа не должно висеть вечно
    m_authTimer->setSingleShot(true);
    connect(m_authTimer, &QTimer::timeout, this, [this]() {
        if (!m_authenticated) {
            qWarning() << "Нет входа за" << Limits::AuthTimeoutMs / 1000 << "с, закрываем соединение с" << m_ipKey;
            m_socket->abort();
        }
    });
    m_authTimer->start(Limits::AuthTimeoutMs);

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
    // Без проверки m_authenticated: иначе клиент никогда не узнает об отказе во входе.
    // Маршрутизация сообщений другим пользователям проверяет isAuthenticated() в ServerCore.
    if (!m_socket) {
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

        const uint32_t maxLength = m_authenticated ? MaxMessageSize : Limits::MaxUnauthFrameSize;
        if (messageLength == 0 || messageLength > maxLength) {
            qWarning() << "Неверная длина сообщения:" << messageLength << "от" << m_ipKey;
            m_socket->abort();
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

        // Пароль и код приглашения не должны попадать в журнал
        QJsonObject loggable = message;
        for (const QString &secret : {Protocol::PASSWORD, Protocol::INVITE}) {
            if (loggable.contains(secret)) {
                loggable[secret] = "***";
            }
        }
        qDebug() << "Получено сообщение от клиента:" << loggable;
        processMessage(message);
    }
}

void ClientHandler::processMessage(const QJsonObject &message) {
    Protocol::MessageType type = ProtocolHelper::parseMessageType(message);
    qDebug() << "Тип сообщения:" << static_cast<int>(type);

    // Ограничение частоты запросов от вошедшего пользователя
    if (m_authenticated) {
        if (!m_bucket.tryTake()) {
            if (!m_throttled) {
                m_throttled = true;
                qWarning() << "Превышен лимит частоты сообщений:" << m_nick;
            }
            return;
        }
        m_throttled = false;
    }

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
        case Protocol::MessageType::DeliveryAck:
            if (m_authenticated) {
                handleDeliveryAck(message);
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
    if (!checkProtocolVersion(message)) return;
    if (authBlocked()) return;

    QString nick = ProtocolHelper::getString(message, Protocol::NICK);
    QString password = ProtocolHelper::getString(message, Protocol::PASSWORD);
    qDebug() << "Login attempt, nick:" << nick;

    if (nick.isEmpty() || password.isEmpty()
        || nick.size() > Protocol::MaxNickLength || password.size() > Protocol::MaxPasswordLength) {
        sendAuthResponse(false, "Неверный формат данных");
        return;
    }

    int userId = m_database->getUserIdByNick(nick);
    if (userId <= 0) {
        qDebug() << "User not found:" << nick;
        recordAuthFailure();
        sendAuthResponse(false, "Такого пользователя не существует. Зарегистрируйтесь.");
        return;
    }

    if (!m_database->verifyPassword(userId, password)) {
        qDebug() << "Wrong password for:" << nick;
        recordAuthFailure();
        sendAuthResponse(false, "Неверный пароль. Попробуйте ещё раз.");
        return;
    }

    m_userId = userId;
    m_nick = nick;

    qDebug() << "Login successful for user:" << nick << "ID:" << userId;

    onAuthenticated();
    sendAuthResponse(true, "Успешно");
    sendUserList();

    emit authenticated(m_userId, this);
}

void ClientHandler::handleRegister(const QJsonObject &message) {
    qDebug() << "handleRegister called";
    if (m_authenticated) return;
    if (!checkProtocolVersion(message)) return;
    if (authBlocked()) return;

    QString nick = ProtocolHelper::getString(message, Protocol::NICK);
    QString password = ProtocolHelper::getString(message, Protocol::PASSWORD);
    qDebug() << "Register attempt, nick:" << nick;

    if (nick.isEmpty() || password.isEmpty()) {
        sendAuthResponse(false, "Неверный формат данных");
        return;
    }

    // Код проверяется первым, чтобы без него нельзя было выяснять, какие ники заняты
    QString invite = ProtocolHelper::getString(message, Protocol::INVITE);
    if (!m_database->inviteAvailable(invite)) {
        qDebug() << "Register rejected, invalid invite for:" << nick;
        recordAuthFailure();
        sendAuthResponse(false, "Неверный или уже использованный код приглашения");
        return;
    }

    const QString problem = nickProblem(nick);
    if (!problem.isEmpty()) {
        sendAuthResponse(false, problem);
        return;
    }
    if (password.size() < Protocol::MinPasswordLength || password.size() > Protocol::MaxPasswordLength) {
        sendAuthResponse(false, QString("Пароль должен быть от %1 до %2 символов")
                                    .arg(Protocol::MinPasswordLength).arg(Protocol::MaxPasswordLength));
        return;
    }

    if (m_database->userExists(nick)) {
        qDebug() << "User already exists:" << nick;
        sendAuthResponse(false, "Пользователь уже существует");
        return;
    }

    int userId = m_database->createUserWithInvite(nick, password, invite);
    if (userId <= 0) {
        qDebug() << "Failed to create user:" << nick;
        sendAuthResponse(false, "Ошибка регистрации");
        return;
    }

    m_userId = userId;
    m_nick = nick;

    qDebug() << "Registration successful for user:" << nick << "ID:" << userId;

    onAuthenticated();
    sendAuthResponse(true, "Успешно");
    sendUserList();

    emit authenticated(m_userId, this);
}

void ClientHandler::handleMessage(const QJsonObject &message) {
    if (!m_authenticated) return;

    QString toNick = ProtocolHelper::getString(message, Protocol::TO_NICK);
    QString text = ProtocolHelper::getString(message, Protocol::TEXT);

    if (toNick.isEmpty() || text.isEmpty() || toNick.size() > Protocol::MaxNickLength) return;

    if (text.size() > Protocol::MaxMessageLength) {
        sendMessage(ProtocolHelper::createErrorMessage(
            QString("Сообщение слишком длинное (максимум %1 символов)").arg(Protocol::MaxMessageLength)));
        return;
    }

    int toUserId = m_database->getUserIdByNick(toNick);
    if (toUserId <= 0 || toUserId == m_userId) return;

    QString clientMsgId = ProtocolHelper::getString(message, Protocol::CLIENT_MSG_ID);
    if (clientMsgId.size() > Limits::MaxClientMsgIdLength) return;

    bool duplicate = false;
    int messageId = m_database->saveMessage(m_userId, toUserId, text, clientMsgId, &duplicate);
    if (messageId <= 0) {
        // Без подтверждения клиент оставит сообщение в очереди и повторит отправку
        return;
    }

    if (!clientMsgId.isEmpty()) {
        sendMessage(ProtocolHelper::createMessageAck(clientMsgId, messageId));
    }

    if (duplicate) {
        qDebug() << "Повторная отправка сообщения" << clientMsgId << "- уже сохранено как" << messageId;
        return;
    }
    forwardMessage(m_userId, toUserId, messageId, text);
}

void ClientHandler::handleDeliveryAck(const QJsonObject &message) {
    QList<int> ids;
    for (const QJsonValue &val : message[Protocol::IDS].toArray()) {
        int id = val.toInt();
        if (id > 0) {
            ids.append(id);
        }
        if (ids.size() >= Limits::MaxAckIds) {
            break;
        }
    }
    if (ids.isEmpty()) return;

    qDebug() << "Подтверждение доставки от" << m_nick << ":" << ids;
    m_database->markMessagesAsDelivered(m_userId, ids);
}

bool ClientHandler::checkProtocolVersion(const QJsonObject &message) {
    const int clientVersion = message[Protocol::PROTOCOL].toInt(0);
    if (clientVersion == Protocol::Version) {
        return true;
    }

    qDebug() << "Несовместимая версия протокола у клиента:" << clientVersion
             << "(сервер:" << Protocol::Version << ")";
    if (clientVersion > Protocol::Version) {
        sendAuthResponse(false, QString("Сервер устарел: приложение использует протокол версии %1, "
                                        "а сервер - %2. Обновите сервер.")
                                    .arg(clientVersion).arg(Protocol::Version));
    } else {
        sendAuthResponse(false, QString("Приложение устарело: сервер использует протокол версии %1, "
                                        "а у вас %2. Обновите приложение.")
                                    .arg(Protocol::Version)
                                    .arg(clientVersion > 0 ? QString::number(clientVersion) : QString("старая")));
    }
    return false;
}

bool ClientHandler::authBlocked() {
    const qint64 retryMs = m_authLimiter->retryAfterMs(m_ipKey);
    if (retryMs <= 0) {
        return false;
    }

    // Проверка до вычисления хеша пароля: заблокированный адрес не нагружает сервер
    const int minutes = static_cast<int>((retryMs + 59999) / 60000);
    qWarning() << "Слишком много неудачных попыток входа с" << m_ipKey << "- блокировка ещё на" << minutes << "мин";
    sendAuthResponse(false, QString("Слишком много неудачных попыток. Повторите через %1 мин.").arg(minutes));
    return true;
}

void ClientHandler::recordAuthFailure() {
    m_authLimiter->recordFailure(m_ipKey);
}

void ClientHandler::onAuthenticated() {
    m_authenticated = true;
    m_authTimer->stop();
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

void ClientHandler::handleGetOfflineMessages() {
    QList<QJsonObject> rawMessages = m_database->getUndeliveredMessages(m_userId);
    qDebug() << "Отправка offline-сообщений для" << m_nick << ", количество:" << rawMessages.size();

    // Доставленными сообщения считаются только после delivery_ack от клиента
    QList<QJsonObject> messages;
    for (const auto &msg : rawMessages) {
        int fromUserId = msg["from_user_id"].toInt();
        QString fromNick = m_database->getNickById(fromUserId);
        if (fromNick.isEmpty()) continue;

        QJsonObject resolved;
        resolved["id"] = msg["id"];
        resolved["from_id"] = fromUserId;
        resolved["from_nick"] = fromNick;
        resolved["message"] = msg["message"];
        resolved["timestamp"] = msg["timestamp"];
        messages.append(resolved);
    }

    QJsonObject response = ProtocolHelper::createMessageHistory(messages);
    sendMessage(response);
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

    if (user["id"].toInt() == m_userId) {
        QJsonObject response = ProtocolHelper::createSearchUserResponse(false, QJsonObject(), "Нельзя написать самому себе");
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
        m_nick, text, QDateTime::currentDateTime().toString(Qt::ISODate), fromUserId, messageId
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
