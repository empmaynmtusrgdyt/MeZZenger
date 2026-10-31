#include "ClientHandler.h"
#include "DatabaseServer.h"
#include "Protocol.h"
#include "ServerLimits.h"
#include "FileStore.h"
#include "FileSession.h"
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
                             FileStore *files, QObject *parent)
    : QObject(parent)
    , m_socket(socket)
    , m_database(db)
    , m_authLimiter(authLimiter)
    , m_files(files)
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
    QJsonObject loggable = message;
    if (loggable.contains(Protocol::TOKEN)) {
        loggable[Protocol::TOKEN] = "***";
    }
    qDebug() << "Отправлено сообщение клиенту" << m_nick << ":" << loggable;
}

void ClientHandler::readMessages() {
    m_buffer += m_socket->readAll();

    if (m_fileSession) {
        m_fileSession->feed(m_buffer);
        m_buffer.clear();
        return;
    }

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
        for (const QString &secret : {Protocol::PASSWORD, Protocol::INVITE, Protocol::OLD_PASSWORD, Protocol::NEW_PASSWORD, Protocol::TOKEN}) {
            if (loggable.contains(secret)) {
                loggable[secret] = "***";
            }
        }
        qDebug() << "Получено сообщение от клиента:" << loggable;
        processMessage(message);

        // После заголовка передачи файла идут сырые байты, а не кадры
        if (m_fileSession) {
            m_fileSession->feed(m_buffer);
            m_buffer.clear();
            return;
        }
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
        case Protocol::MessageType::FilePut:
            startFileSession(message, true);
            break;
        case Protocol::MessageType::FileGet:
            startFileSession(message, false);
            break;
        case Protocol::MessageType::FileUploadRequest:
            if (m_authenticated) {
                handleFileUploadRequest(message);
            }
            break;
        case Protocol::MessageType::FileDownloadRequest:
            if (m_authenticated) {
                handleFileDownloadRequest(message);
            }
            break;
        case Protocol::MessageType::GroupCreate:
            if (m_authenticated) {
                handleGroupCreate(message);
            }
            break;
        case Protocol::MessageType::GroupAdd:
            if (m_authenticated) {
                handleGroupAdd(message);
            }
            break;
        case Protocol::MessageType::GroupRemove:
            if (m_authenticated) {
                handleGroupRemove(message);
            }
            break;
        case Protocol::MessageType::GroupRename:
            if (m_authenticated) {
                handleGroupRename(message);
            }
            break;
        case Protocol::MessageType::ChangePassword:
            if (m_authenticated) {
                handleChangePassword(message);
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
    sendGroupList();

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
    sendGroupList();

    emit authenticated(m_userId, this);
}

void ClientHandler::handleMessage(const QJsonObject &message) {
    if (!m_authenticated) return;

    QString toNick = ProtocolHelper::getString(message, Protocol::TO_NICK);
    QString text = ProtocolHelper::getString(message, Protocol::TEXT);

    const QString fileId = ProtocolHelper::getString(message, Protocol::FILE_ID);

    const int groupId = message[Protocol::GROUP_ID].toInt(0);

    // Подпись к файлу может быть пустой
    if ((groupId <= 0 && toNick.isEmpty()) || (text.isEmpty() && fileId.isEmpty()) || toNick.size() > Protocol::MaxNickLength
        || fileId.size() > 64) return;

    if (text.size() > Protocol::MaxMessageLength) {
        sendMessage(ProtocolHelper::createErrorMessage(
            QString("Сообщение слишком длинное (максимум %1 символов)").arg(Protocol::MaxMessageLength)));
        return;
    }

    if (groupId > 0) {
        handleGroupMessage(groupId, text, fileId, message);
        return;
    }

    int toUserId = m_database->getUserIdByNick(toNick);
    if (toUserId <= 0 || toUserId == m_userId) return;

    QString clientMsgId = ProtocolHelper::getString(message, Protocol::CLIENT_MSG_ID);
    if (clientMsgId.size() > Limits::MaxClientMsgIdLength) return;

    bool duplicate = false;
    int messageId = m_database->saveMessage(m_userId, toUserId, text, clientMsgId, &duplicate, fileId);
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
    forwardMessage(m_userId, toUserId, messageId, text, fileId);
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
    // Друзей немного, поэтому отдаём всех: клиент показывает их списком, а не ищет по точному нику
    QList<QJsonObject> users = m_database->getAllUsers(m_userId);
    for (auto &user : users) {
        user[Protocol::ONLINE] = m_isOnline && m_isOnline(user["id"].toInt());
    }
    qDebug() << "Sending user list to" << m_nick << ", count:" << users.size();
    QJsonObject response = ProtocolHelper::createUserList(users);
    sendMessage(response);
}

void ClientHandler::handleChangePassword(const QJsonObject &message) {
    const QString oldPassword = ProtocolHelper::getString(message, Protocol::OLD_PASSWORD);
    const QString newPassword = ProtocolHelper::getString(message, Protocol::NEW_PASSWORD);

    // Старый пароль можно перебирать так же, как при входе, поэтому считаем попытки по IP
    const qint64 retryMs = m_authLimiter->retryAfterMs(m_ipKey);
    if (retryMs > 0) {
        sendMessage(ProtocolHelper::createChangePasswordResponse(false,
            QString("Слишком много неудачных попыток. Повторите через %1 мин.").arg((retryMs + 59999) / 60000)));
        return;
    }

    if (newPassword.size() < Protocol::MinPasswordLength || newPassword.size() > Protocol::MaxPasswordLength
        || oldPassword.size() > Protocol::MaxPasswordLength) {
        sendMessage(ProtocolHelper::createChangePasswordResponse(false,
            QString("Новый пароль должен быть от %1 до %2 символов").arg(Protocol::MinPasswordLength).arg(Protocol::MaxPasswordLength)));
        return;
    }
    if (!m_database->verifyPassword(m_userId, oldPassword)) {
        recordAuthFailure();
        sendMessage(ProtocolHelper::createChangePasswordResponse(false, "Неверный текущий пароль"));
        return;
    }
    if (!m_database->setPassword(m_userId, newPassword)) {
        sendMessage(ProtocolHelper::createChangePasswordResponse(false, "Не удалось сменить пароль"));
        return;
    }
    qDebug() << "Пароль изменён:" << m_nick;
    sendMessage(ProtocolHelper::createChangePasswordResponse(true, "Пароль изменён"));
}

void ClientHandler::handleFileUploadRequest(const QJsonObject &message) {
    const QString ref = ProtocolHelper::getString(message, Protocol::REF);
    const QString name = ProtocolHelper::getString(message, Protocol::FILE_NAME);
    const qint64 size = static_cast<qint64>(message[Protocol::FILE_SIZE].toDouble());

    QString error, fileId;
    const QString token = m_files->issueUpload(m_userId, name, size, &fileId, &error);
    if (token.isEmpty()) {
        sendMessage(ProtocolHelper::createFileTicket(false, error, ref));
        return;
    }
    sendMessage(ProtocolHelper::createFileTicket(true, QString(), ref, fileId, token, FileStore::sanitizeName(name), size));
}

void ClientHandler::handleFileDownloadRequest(const QJsonObject &message) {
    const QString fileId = ProtocolHelper::getString(message, Protocol::FILE_ID);

    QString error;
    const QString token = m_files->issueDownload(m_userId, fileId, &error);
    if (token.isEmpty()) {
        sendMessage(ProtocolHelper::createFileTicket(false, error, QString(), fileId));
        return;
    }
    const QJsonObject info = m_database->fileInfo(fileId);
    sendMessage(ProtocolHelper::createFileTicket(true, QString(), QString(), fileId, token,
                                                 info["name"].toString(), static_cast<qint64>(info["size"].toDouble())));
}

// Отдельное соединение передачи файла: личность подтверждает одноразовый токен, входить не нужно
void ClientHandler::startFileSession(const QJsonObject &message, bool upload) {
    if (m_authenticated || m_fileSession) return;

    FileStore::Ticket ticket;
    const QString token = ProtocolHelper::getString(message, Protocol::TOKEN);
    if (m_authLimiter->retryAfterMs(m_ipKey) > 0 || !m_files->take(token, &ticket) || ticket.upload != upload) {
        recordAuthFailure();
        sendMessage(ProtocolHelper::createFileResult(false, "Недействительное разрешение на передачу файла"));
        m_socket->disconnectFromHost();
        return;
    }

    m_authTimer->stop();
    m_fileSession = new FileSession(m_socket, m_files, m_database, ticket, this);
    m_fileSession->start();
}

void ClientHandler::handleGetOfflineMessages() {
    QList<QJsonObject> rawMessages = m_database->getUndeliveredMessages(m_userId);
    qDebug() << "Отправка offline-сообщений для" << m_nick << ", количество:" << rawMessages.size();

    // Доставленными сообщения считаются только после delivery_ack от клиента
    QList<QJsonObject> messages;
    for (const auto &msg : rawMessages) {
        int fromUserId = msg["from_user_id"].toInt();
        // from_user_id = 0 - служебное сообщение группы, автора у него нет
        QString fromNick = fromUserId == 0 ? QString() : m_database->getNickById(fromUserId);
        if (fromUserId != 0 && fromNick.isEmpty()) continue;

        QJsonObject resolved;
        resolved["id"] = msg["id"];
        resolved["from_id"] = fromUserId;
        resolved["from_nick"] = fromNick;
        resolved["message"] = msg["message"];
        resolved["timestamp"] = msg["timestamp"];
        if (msg.contains(Protocol::GROUP_ID)) {
            resolved[Protocol::GROUP_ID] = msg[Protocol::GROUP_ID];
        }
        if (fromUserId == 0) {
            resolved[Protocol::SYSTEM] = true;
        }
        if (msg.contains(Protocol::FILE_ID)) {
            resolved[Protocol::FILE_ID] = msg[Protocol::FILE_ID];
            resolved[Protocol::FILE_NAME] = msg[Protocol::FILE_NAME];
            resolved[Protocol::FILE_SIZE] = msg[Protocol::FILE_SIZE];
        }
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

void ClientHandler::forwardMessage(int fromUserId, int toUserId, int messageId, const QString &text, const QString &fileId,
                                   int groupId) {
    QString fileName;
    qint64 fileSize = 0;
    if (!fileId.isEmpty()) {
        const QJsonObject info = m_database->fileInfo(fileId);
        fileName = info["name"].toString();
        fileSize = static_cast<qint64>(info["size"].toDouble());
    }
    QJsonObject message = ProtocolHelper::createMessageWithTimestamp(
        m_nick, text, QDateTime::currentDateTime().toString(Qt::ISODate), fromUserId, messageId,
        fileId, fileName, fileSize, groupId
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

namespace {

// Положительные неповторяющиеся id из JSON-массива, не больше limit штук
QList<int> parseIds(const QJsonValue &value, int limit) {
    QList<int> ids;
    for (const QJsonValue &item : value.toArray()) {
        const int id = item.toInt();
        if (id > 0 && !ids.contains(id)) {
            ids.append(id);
        }
        if (ids.size() >= limit) {
            break;
        }
    }
    return ids;
}

// Описание проблемы с названием группы или пустая строка
QString groupNameProblem(const QString &name) {
    if (name.isEmpty() || name.size() > Protocol::MaxGroupNameLength) {
        return QString("Название группы должно быть от 1 до %1 символов").arg(Protocol::MaxGroupNameLength);
    }
    for (uint codePoint : name.toUcs4()) {
        if (!QChar::isPrint(codePoint)) {
            return "Название группы содержит недопустимые символы";
        }
    }
    return QString();
}

}

void ClientHandler::groupResult(bool success, const QString &message, int groupId) {
    sendMessage(ProtocolHelper::createGroupResult(success, message, groupId));
}

void ClientHandler::sendGroupList() {
    sendMessage(ProtocolHelper::createGroupList(m_database->groupsOf(m_userId)));
}

void ClientHandler::pushGroupInfo(int groupId) {
    const QJsonObject group = m_database->groupInfo(groupId);
    if (group.isEmpty()) {
        return;
    }
    for (int memberId : m_database->groupMemberIds(groupId)) {
        emit pushToUser(memberId, ProtocolHelper::createGroupInfo(group));
    }
}

void ClientHandler::handleGroupCreate(const QJsonObject &message) {
    const QString name = ProtocolHelper::getString(message, Protocol::NAME).trimmed();
    if (const QString problem = groupNameProblem(name); !problem.isEmpty()) {
        groupResult(false, problem);
        return;
    }

    QList<int> members = parseIds(message[Protocol::MEMBERS], Protocol::MaxGroupMembers + 1);
    members.removeAll(m_userId);
    if (members.isEmpty() || members.size() + 1 > Protocol::MaxGroupMembers) {
        groupResult(false, QString("В группе должно быть от 2 до %1 участников").arg(Protocol::MaxGroupMembers));
        return;
    }
    for (int id : members) {
        if (m_database->getNickById(id).isEmpty()) {
            groupResult(false, "Один из участников не найден");
            return;
        }
        if (m_database->groupCountOf(id) >= Protocol::MaxGroupsPerUser) {
            groupResult(false, "Один из участников состоит в слишком большом числе групп");
            return;
        }
    }
    if (m_database->groupCountOf(m_userId) >= Protocol::MaxGroupsPerUser) {
        groupResult(false, "Вы состоите в слишком большом числе групп");
        return;
    }

    const int groupId = m_database->createGroup(name, m_userId, members);
    if (groupId <= 0) {
        groupResult(false, "Не удалось создать группу");
        return;
    }
    qDebug() << "Группа создана:" << groupId << name << "владелец" << m_nick;
    groupResult(true, QString(), groupId);
    pushGroupInfo(groupId);
}

void ClientHandler::handleGroupAdd(const QJsonObject &message) {
    const int groupId = message[Protocol::GROUP_ID].toInt(0);
    const QJsonObject group = m_database->groupInfo(groupId);
    if (group.isEmpty() || group[Protocol::OWNER_ID].toInt() != m_userId) {
        groupResult(false, "Добавлять участников может только владелец группы", groupId);
        return;
    }

    const QList<int> current = m_database->groupMemberIds(groupId);
    QList<int> added;
    for (int id : parseIds(message[Protocol::MEMBERS], Protocol::MaxGroupMembers)) {
        if (current.contains(id)) {
            continue;
        }
        if (m_database->getNickById(id).isEmpty()) {
            groupResult(false, "Один из участников не найден", groupId);
            return;
        }
        if (m_database->groupCountOf(id) >= Protocol::MaxGroupsPerUser) {
            groupResult(false, "Один из участников состоит в слишком большом числе групп", groupId);
            return;
        }
        added.append(id);
    }
    if (added.isEmpty()) {
        groupResult(false, "Некого добавлять", groupId);
        return;
    }
    if (current.size() + added.size() > Protocol::MaxGroupMembers) {
        groupResult(false, QString("В группе не может быть больше %1 участников").arg(Protocol::MaxGroupMembers), groupId);
        return;
    }
    if (!m_database->addGroupMembers(groupId, added)) {
        groupResult(false, "Не удалось добавить участников", groupId);
        return;
    }
    groupResult(true, QString(), groupId);
    pushGroupInfo(groupId);

    // Всем, включая новых: новичок видит, кто его добавил и кто ещё добавлен
    QStringList addedNicks;
    for (int id : added) {
        addedNicks << m_database->getNickById(id);
    }
    announceToGroup(groupId, m_database->groupMemberIds(groupId), m_nick + " добавил(а) " + addedNicks.join(", "));
}

void ClientHandler::handleGroupRemove(const QJsonObject &message) {
    const int groupId = message[Protocol::GROUP_ID].toInt(0);
    const int userId = message[Protocol::ID].toInt(0);
    const QJsonObject group = m_database->groupInfo(groupId);
    const QList<int> members = m_database->groupMemberIds(groupId);
    if (group.isEmpty() || !members.contains(m_userId) || !members.contains(userId)) {
        groupResult(false, "Участник или группа не найдены", groupId);
        return;
    }
    const int ownerId = group[Protocol::OWNER_ID].toInt();
    if (userId != m_userId && ownerId != m_userId) {
        groupResult(false, "Удалять участников может только владелец группы", groupId);
        return;
    }
    if (!m_database->removeGroupMember(groupId, userId)) {
        groupResult(false, "Не удалось удалить участника", groupId);
        return;
    }

    QList<int> remaining = members;
    remaining.removeAll(userId);
    const QString removedNick = m_database->getNickById(userId);
    emit pushToUser(userId, ProtocolHelper::createGroupRemoved(groupId));

    if (remaining.size() < 2) {
        // Группа из одного человека не нужна
        for (int id : remaining) {
            emit pushToUser(id, ProtocolHelper::createGroupRemoved(groupId));
        }
        m_database->deleteGroup(groupId);
    } else {
        if (userId == ownerId) {
            m_database->setGroupOwner(groupId, remaining.first());
        }
        pushGroupInfo(groupId);
        announceToGroup(groupId, remaining, userId == m_userId
            ? removedNick + " покинул(а) группу"
            : removedNick + " удалён(а) из группы");
    }
    groupResult(true, QString(), groupId);
}

// Служебное сообщение остальным участникам: хранится как обычное, поэтому дойдёт и тем, кто сейчас не в сети
void ClientHandler::announceToGroup(int groupId, const QList<int> &recipients, const QString &text) {
    for (const auto &row : m_database->saveGroupNotice(groupId, text, recipients)) {
        QJsonObject notice = ProtocolHelper::createMessageWithTimestamp(
            QString(), text, QDateTime::currentDateTime().toString(Qt::ISODate), 0, row.first,
            QString(), QString(), 0, groupId, true);
        emit messageReady(row.second, row.first, notice);
    }
}

void ClientHandler::handleGroupRename(const QJsonObject &message) {
    const int groupId = message[Protocol::GROUP_ID].toInt(0);
    const QString name = ProtocolHelper::getString(message, Protocol::NAME).trimmed();
    const QJsonObject group = m_database->groupInfo(groupId);
    if (group.isEmpty() || group[Protocol::OWNER_ID].toInt() != m_userId) {
        groupResult(false, "Переименовать группу может только владелец", groupId);
        return;
    }
    if (const QString problem = groupNameProblem(name); !problem.isEmpty()) {
        groupResult(false, problem, groupId);
        return;
    }
    const QString oldName = group[Protocol::NAME].toString();
    if (!m_database->renameGroup(groupId, name)) {
        groupResult(false, "Не удалось переименовать группу", groupId);
        return;
    }
    groupResult(true, QString(), groupId);
    pushGroupInfo(groupId);
    if (name != oldName) {
        announceToGroup(groupId, m_database->groupMemberIds(groupId),
                        m_nick + " переименовал(а) группу: «" + oldName + "» → «" + name + "»");
    }
}

void ClientHandler::handleGroupMessage(int groupId, const QString &text, const QString &fileId, const QJsonObject &message) {
    if (!m_database->groupMemberIds(groupId).contains(m_userId)) {
        // Клиент мог не узнать, что его убрали из группы: пусть уберёт её и свои неотправленные сообщения
        sendMessage(ProtocolHelper::createGroupRemoved(groupId));
        return;
    }

    const QString clientMsgId = ProtocolHelper::getString(message, Protocol::CLIENT_MSG_ID);
    if (clientMsgId.size() > Limits::MaxClientMsgIdLength) return;

    bool duplicate = false;
    QList<QPair<int, int>> created;
    const int ackId = m_database->saveGroupMessage(m_userId, groupId, text, clientMsgId, &duplicate, fileId, &created);
    if (ackId <= 0) {
        return;
    }
    if (!clientMsgId.isEmpty()) {
        sendMessage(ProtocolHelper::createMessageAck(clientMsgId, ackId));
    }
    if (duplicate) {
        return;
    }
    for (const auto &row : created) {
        forwardMessage(m_userId, row.second, row.first, text, fileId, groupId);
    }
}
