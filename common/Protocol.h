#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <cstdint>
#include <cstring>

namespace Protocol {
    enum class MessageType {
        Login,
        Register,
        AuthResponse,
        UserList,
        Message,
        MessageHistory,
        GetOfflineMessages,
        Error,
        SearchUser,
        SearchUserResponse,
        MarkMessagesRead,
        MessageAck,
        DeliveryAck,
        Presence,
        ChangePassword,
        ChangePasswordResponse,
        FileUploadRequest,
        FileDownloadRequest,
        FileTicket,
        FilePut,
        FileGet,
        FileInfo,
        FileResult,
        GroupCreate,
        GroupAdd,
        GroupRemove,
        GroupRename,
        GroupResult,
        GroupInfo,
        GroupList,
        GroupRemoved
    };

    inline const QString TYPE = "type";
    inline const QString NICK = "nick";
    inline const QString PASSWORD = "password";
    inline const QString SUCCESS = "success";
    inline const QString MESSAGE = "message";
    inline const QString USERS = "users";
    inline const QString FROM_ID = "from_id";
    inline const QString FROM_NICK = "from_nick";
    inline const QString TO_NICK = "to_nick";
    inline const QString TEXT = "text";
    inline const QString TIMESTAMP = "timestamp";
    inline const QString ID = "id";
    inline const QString IDS = "ids";
    inline const QString CLIENT_MSG_ID = "client_msg_id";
    inline const QString INVITE = "invite";
    inline const QString PROTOCOL = "protocol";
    inline const QString ONLINE = "online";
    inline const QString OLD_PASSWORD = "old_password";
    inline const QString NEW_PASSWORD = "new_password";
    inline const QString FILE_ID = "file_id";
    inline const QString FILE_NAME = "file_name";
    inline const QString FILE_SIZE = "file_size";
    inline const QString TOKEN = "token";
    inline const QString REF = "ref";
    inline const QString SIZE = "size";
    inline const QString GROUP_ID = "group_id";
    inline const QString GROUP = "group";
    inline const QString GROUPS = "groups";
    inline const QString MEMBERS = "members";
    inline const QString OWNER_ID = "owner_id";
    inline const QString NAME = "name";
    inline const QString SYSTEM = "system";

    // Версия протокола. Увеличивайте при любом несовместимом изменении (смена смысла полей,
    // новые обязательные поля или сообщения). Добавление необязательных полей версию не меняет.
    // Сервер принимает только клиентов с точно такой же версией.
    inline constexpr int Version = 1;

    // Ограничения на данные от пользователя (клиент ограничивает ввод, сервер проверяет сам)
    inline constexpr int MinNickLength = 2;
    inline constexpr int MaxNickLength = 32;
    inline constexpr int MinPasswordLength = 4;
    inline constexpr int MaxPasswordLength = 128;
    inline constexpr int MaxMessageLength = 4000;
    inline constexpr qint64 MaxFileSize = 50LL * 1024 * 1024;
    inline constexpr int MaxFileNameLength = 200;
    inline constexpr int MaxGroupNameLength = 40;
    inline constexpr int MaxGroupMembers = 50;
    inline constexpr int MaxGroupsPerUser = 50;
}

class ProtocolHelper {
public:
    static QJsonObject createLoginMessage(const QString &nick, const QString &password) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "login";
        obj[Protocol::PROTOCOL] = Protocol::Version;
        obj[Protocol::NICK] = nick;
        obj[Protocol::PASSWORD] = password;
        return obj;
    }

    static QJsonObject createRegisterMessage(const QString &nick, const QString &password, const QString &invite = QString()) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "register";
        obj[Protocol::PROTOCOL] = Protocol::Version;
        obj[Protocol::NICK] = nick;
        obj[Protocol::PASSWORD] = password;
        obj[Protocol::INVITE] = invite;
        return obj;
    }

    static QJsonObject createAuthResponse(bool success, const QString &message) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "auth_response";
        obj[Protocol::PROTOCOL] = Protocol::Version;
        obj[Protocol::SUCCESS] = success;
        obj[Protocol::MESSAGE] = message;
        return obj;
    }

    static QJsonObject createUserList(const QList<QJsonObject> &users) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "user_list";
        QJsonArray arr;
        for (const auto &user : users) {
            arr.append(user);
        }
        obj[Protocol::USERS] = arr;
        return obj;
    }

    static QJsonObject createMessage(const QString &fromNick, const QString &toNick, const QString &text) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "message";
        obj[Protocol::FROM_NICK] = fromNick;
        obj[Protocol::TO_NICK] = toNick;
        obj[Protocol::TEXT] = text;
        return obj;
    }

    // Сообщение от клиента серверу. client_msg_id нужен серверу, чтобы отбросить повторную отправку.
    // fileId (если не пуст) ссылается на файл, заранее загруженный отдельным соединением.
    // Имя и размер сервер берёт из своей записи о файле, присланным значениям не верит.
    // groupId > 0: сообщение в группу (to_nick не нужен), сервер разошлёт его участникам.
    static QJsonObject createOutgoingMessage(const QString &toNick, const QString &text, const QString &clientMsgId,
                                             const QString &fileId = QString(), int groupId = 0) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "message";
        if (groupId > 0) {
            obj[Protocol::GROUP_ID] = groupId;
        } else {
            obj[Protocol::TO_NICK] = toNick;
        }
        obj[Protocol::TEXT] = text;
        obj[Protocol::CLIENT_MSG_ID] = clientMsgId;
        if (!fileId.isEmpty()) {
            obj[Protocol::FILE_ID] = fileId;
        }
        return obj;
    }

    // Загрузка: клиент просит разрешение (ref возвращается в file_ticket); скачивание: по file_id.
    static QJsonObject createFileUploadRequest(const QString &name, qint64 size, const QString &ref) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "file_upload_request";
        obj[Protocol::FILE_NAME] = name;
        obj[Protocol::FILE_SIZE] = size;
        obj[Protocol::REF] = ref;
        return obj;
    }

    static QJsonObject createFileDownloadRequest(const QString &fileId) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "file_download_request";
        obj[Protocol::FILE_ID] = fileId;
        return obj;
    }

    // Сервер -> клиенту: одноразовый токен для отдельного соединения передачи файла.
    static QJsonObject createFileTicket(bool success, const QString &message, const QString &ref = QString(),
                                        const QString &fileId = QString(), const QString &token = QString(),
                                        const QString &name = QString(), qint64 size = 0) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "file_ticket";
        obj[Protocol::SUCCESS] = success;
        obj[Protocol::MESSAGE] = message;
        obj[Protocol::REF] = ref;
        obj[Protocol::FILE_ID] = fileId;
        obj[Protocol::TOKEN] = token;
        obj[Protocol::FILE_NAME] = name;
        obj[Protocol::FILE_SIZE] = size;
        return obj;
    }

    // Первый кадр отдельного соединения: после него идут size сырых байт (put) или сервер присылает file_info и байты (get).
    static QJsonObject createFilePut(const QString &token) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "file_put";
        obj[Protocol::TOKEN] = token;
        return obj;
    }

    static QJsonObject createFileGet(const QString &token) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "file_get";
        obj[Protocol::TOKEN] = token;
        return obj;
    }

    static QJsonObject createFileInfo(qint64 size) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "file_info";
        obj[Protocol::SIZE] = size;
        return obj;
    }

    static QJsonObject createFileResult(bool success, const QString &message = QString()) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "file_result";
        obj[Protocol::SUCCESS] = success;
        obj[Protocol::MESSAGE] = message;
        return obj;
    }

    // Сервер -> отправителю: сообщение принято и сохранено под серверным id.
    static QJsonObject createMessageAck(const QString &clientMsgId, int id) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "message_ack";
        obj[Protocol::CLIENT_MSG_ID] = clientMsgId;
        obj[Protocol::ID] = id;
        return obj;
    }

    // Получатель -> серверу: сообщения с этими id получены и сохранены.
    static QJsonObject createDeliveryAck(const QList<int> &ids) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "delivery_ack";
        QJsonArray arr;
        for (int id : ids) {
            arr.append(id);
        }
        obj[Protocol::IDS] = arr;
        return obj;
    }

    static QJsonObject createMessageWithTimestamp(const QString &fromNick, const QString &text, const QString &timestamp, int fromId = 0, int id = 0,
                                                  const QString &fileId = QString(), const QString &fileName = QString(), qint64 fileSize = 0,
                                                  int groupId = 0, bool system = false) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "message";
        // Служебное сообщение группы (например, о выходе участника): без автора, показывается по центру
        if (system) {
            obj[Protocol::SYSTEM] = true;
        }
        if (groupId > 0) {
            obj[Protocol::GROUP_ID] = groupId;
        }
        if (id > 0) {
            obj[Protocol::ID] = id;
        }
        obj[Protocol::FROM_ID] = fromId;
        obj[Protocol::FROM_NICK] = fromNick;
        obj[Protocol::TEXT] = text;
        obj[Protocol::TIMESTAMP] = timestamp;
        if (!fileId.isEmpty()) {
            obj[Protocol::FILE_ID] = fileId;
            obj[Protocol::FILE_NAME] = fileName;
            obj[Protocol::FILE_SIZE] = fileSize;
        }
        return obj;
    }

    // Сервер -> всем вошедшим: пользователь появился в сети, вышел или только что зарегистрировался.
    static QJsonObject createPresence(int userId, const QString &nick, bool online) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "presence";
        obj[Protocol::ID] = userId;
        obj[Protocol::NICK] = nick;
        obj[Protocol::ONLINE] = online;
        return obj;
    }

    static QJsonObject createChangePassword(const QString &oldPassword, const QString &newPassword) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "change_password";
        obj[Protocol::OLD_PASSWORD] = oldPassword;
        obj[Protocol::NEW_PASSWORD] = newPassword;
        return obj;
    }

    static QJsonObject createChangePasswordResponse(bool success, const QString &message) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "change_password_response";
        obj[Protocol::SUCCESS] = success;
        obj[Protocol::MESSAGE] = message;
        return obj;
    }

    // Группы. Команды клиента получают group_result; состояние (group_info, group_list, group_removed)
    // сервер присылает всем затронутым участникам сам. group: {id, name, owner_id, members: [{id, nick}]}.
    static QJsonObject createGroupCreate(const QString &name, const QList<int> &memberIds) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "group_create";
        obj[Protocol::NAME] = name;
        QJsonArray arr;
        for (int id : memberIds) {
            arr.append(id);
        }
        obj[Protocol::MEMBERS] = arr;
        return obj;
    }

    static QJsonObject createGroupAdd(int groupId, const QList<int> &memberIds) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "group_add";
        obj[Protocol::GROUP_ID] = groupId;
        QJsonArray arr;
        for (int id : memberIds) {
            arr.append(id);
        }
        obj[Protocol::MEMBERS] = arr;
        return obj;
    }

    // userId == свой id - выход из группы; чужой - удаление участника (только владелец)
    static QJsonObject createGroupRemove(int groupId, int userId) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "group_remove";
        obj[Protocol::GROUP_ID] = groupId;
        obj[Protocol::ID] = userId;
        return obj;
    }

    static QJsonObject createGroupRename(int groupId, const QString &name) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "group_rename";
        obj[Protocol::GROUP_ID] = groupId;
        obj[Protocol::NAME] = name;
        return obj;
    }

    static QJsonObject createGroupResult(bool success, const QString &message, int groupId = 0) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "group_result";
        obj[Protocol::SUCCESS] = success;
        obj[Protocol::MESSAGE] = message;
        obj[Protocol::GROUP_ID] = groupId;
        return obj;
    }

    static QJsonObject createGroupInfo(const QJsonObject &group) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "group_info";
        obj[Protocol::GROUP] = group;
        return obj;
    }

    static QJsonObject createGroupList(const QList<QJsonObject> &groups) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "group_list";
        QJsonArray arr;
        for (const auto &group : groups) {
            arr.append(group);
        }
        obj[Protocol::GROUPS] = arr;
        return obj;
    }

    // Пользователь больше не участник группы (или группа удалена)
    static QJsonObject createGroupRemoved(int groupId) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "group_removed";
        obj[Protocol::GROUP_ID] = groupId;
        return obj;
    }

    static QJsonObject createGetOfflineMessages() {
        QJsonObject obj;
        obj[Protocol::TYPE] = "get_offline_messages";
        return obj;
    }

    static QJsonObject createMessageHistory(const QList<QJsonObject> &messages) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "message_history";
        QJsonArray arr;
        for (const auto &msg : messages) {
            arr.append(msg);
        }
        obj["messages"] = arr;
        return obj;
    }

    static QJsonObject createErrorMessage(const QString &message) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "error";
        obj[Protocol::MESSAGE] = message;
        return obj;
    }

    static QJsonObject createSearchUserMessage(const QString &nick) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "search_user";
        obj[Protocol::NICK] = nick;
        return obj;
    }

    static QJsonObject createSearchUserResponse(bool success, const QJsonObject &user = QJsonObject(), const QString &message = QString()) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "search_user_response";
        obj[Protocol::SUCCESS] = success;
        if (!user.isEmpty()) {
            obj["user"] = user;
        }
        if (!message.isEmpty()) {
            obj[Protocol::MESSAGE] = message;
        }
        return obj;
    }

    static QJsonObject createMarkMessagesRead(int userId) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "mark_messages_read";
        obj[Protocol::ID] = userId;
        return obj;
    }

    static Protocol::MessageType parseMessageType(const QJsonObject &obj) {
        QString type = obj[Protocol::TYPE].toString();
        if (type == "login") return Protocol::MessageType::Login;
        if (type == "register") return Protocol::MessageType::Register;
        if (type == "auth_response") return Protocol::MessageType::AuthResponse;
        if (type == "user_list") return Protocol::MessageType::UserList;
        if (type == "message") return Protocol::MessageType::Message;
        if (type == "message_history") return Protocol::MessageType::MessageHistory;
        if (type == "get_offline_messages") return Protocol::MessageType::GetOfflineMessages;
        if (type == "error") return Protocol::MessageType::Error;
        if (type == "search_user") return Protocol::MessageType::SearchUser;
        if (type == "search_user_response") return Protocol::MessageType::SearchUserResponse;
        if (type == "mark_messages_read") return Protocol::MessageType::MarkMessagesRead;
        if (type == "message_ack") return Protocol::MessageType::MessageAck;
        if (type == "delivery_ack") return Protocol::MessageType::DeliveryAck;
        if (type == "presence") return Protocol::MessageType::Presence;
        if (type == "change_password") return Protocol::MessageType::ChangePassword;
        if (type == "change_password_response") return Protocol::MessageType::ChangePasswordResponse;
        if (type == "file_upload_request") return Protocol::MessageType::FileUploadRequest;
        if (type == "file_download_request") return Protocol::MessageType::FileDownloadRequest;
        if (type == "file_ticket") return Protocol::MessageType::FileTicket;
        if (type == "file_put") return Protocol::MessageType::FilePut;
        if (type == "file_get") return Protocol::MessageType::FileGet;
        if (type == "file_info") return Protocol::MessageType::FileInfo;
        if (type == "file_result") return Protocol::MessageType::FileResult;
        if (type == "group_create") return Protocol::MessageType::GroupCreate;
        if (type == "group_add") return Protocol::MessageType::GroupAdd;
        if (type == "group_remove") return Protocol::MessageType::GroupRemove;
        if (type == "group_rename") return Protocol::MessageType::GroupRename;
        if (type == "group_result") return Protocol::MessageType::GroupResult;
        if (type == "group_info") return Protocol::MessageType::GroupInfo;
        if (type == "group_list") return Protocol::MessageType::GroupList;
        if (type == "group_removed") return Protocol::MessageType::GroupRemoved;
        return Protocol::MessageType::Error;
    }

    static QString getString(const QJsonObject &obj, const QString &key, const QString &defaultVal = QString()) {
        QJsonValue val = obj[key];
        return val.isNull() ? defaultVal : val.toString();
    }

    static bool getBool(const QJsonObject &obj, const QString &key, bool defaultVal = false) {
        QJsonValue val = obj[key];
        return val.isNull() ? defaultVal : val.toBool();
    }
};

constexpr uint32_t HeaderSize = sizeof(uint32_t);
constexpr uint32_t MaxMessageSize = 1024 * 1024;

inline QByteArray serializeMessage(const QJsonObject& json) {
    QJsonDocument doc(json);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);
    
    QByteArray buffer;
    buffer.resize(HeaderSize + jsonData.size());
    
    uint32_t length = static_cast<uint32_t>(jsonData.size());
    buffer[0] = (length >> 24) & 0xFF;
    buffer[1] = (length >> 16) & 0xFF;
    buffer[2] = (length >> 8) & 0xFF;
    buffer[3] = length & 0xFF;
    
    memcpy(buffer.data() + HeaderSize, jsonData.constData(), jsonData.size());
    
    return buffer;
}

inline QJsonObject deserializeMessage(const QByteArray& data) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        return QJsonObject();
    }
    return doc.object();
}

inline uint32_t readHeader(const QByteArray& header) {
    if (header.size() < static_cast<int>(HeaderSize)) {
        return 0;
    }
    return (static_cast<uint8_t>(header[0]) << 24) |
           (static_cast<uint8_t>(header[1]) << 16) |
           (static_cast<uint8_t>(header[2]) << 8) |
           static_cast<uint8_t>(header[3]);
}

#endif
