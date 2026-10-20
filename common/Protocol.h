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
        MarkMessagesRead
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
}

class ProtocolHelper {
public:
    static QJsonObject createLoginMessage(const QString &nick, const QString &password) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "login";
        obj[Protocol::NICK] = nick;
        obj[Protocol::PASSWORD] = password;
        return obj;
    }

    static QJsonObject createRegisterMessage(const QString &nick, const QString &password) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "register";
        obj[Protocol::NICK] = nick;
        obj[Protocol::PASSWORD] = password;
        return obj;
    }

    static QJsonObject createAuthResponse(bool success, const QString &message) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "auth_response";
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

    static QJsonObject createMessageWithTimestamp(const QString &fromNick, const QString &text, const QString &timestamp, int fromId = 0) {
        QJsonObject obj;
        obj[Protocol::TYPE] = "message";
        obj[Protocol::FROM_ID] = fromId;
        obj[Protocol::FROM_NICK] = fromNick;
        obj[Protocol::TEXT] = text;
        obj[Protocol::TIMESTAMP] = timestamp;
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
