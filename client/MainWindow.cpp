#include "MainWindow.h"
#include "Protocol.h"
#include "ServerConnection.h"
#include <QMessageBox>
#include <QTimer>
#include <QUuid>
#include <QJsonDocument>
#include <QDateTime>
#include <QScrollBar>
#include <QFont>
#include <QDebug>

MainWindow::MainWindow(ServerConnection *connection, const QString &nick, QWidget *parent)
    : QMainWindow(parent)
    , m_nick(nick)
    , m_connection(connection)
    , m_database(new DatabaseClient(this))
{
    qDebug() << "MainWindow constructor called";
    setWindowTitle("MeZZenger - " + nick);
    resize(900, 600);
    
    m_database->initialize("mezzenger_client.db");
    
    setupUI();
    applyStyles();
    
    m_connection->setParent(this);
    m_connection->setDialogParent(this);
    connect(m_connection, &ServerConnection::messageReceived, this, &MainWindow::processServerMessage);
    connect(m_connection, &ServerConnection::authRejected, this, [this](const QString &message) {
        QMessageBox::warning(this, "Ошибка", message);
        close();
    });
    connect(m_connection, &ServerConnection::tlsRejected, this, [this]() {
        QTimer::singleShot(0, this, &QWidget::close);
    });
    m_connection->setAutoReconnect(true);
    m_connection->startDispatch();
}

MainWindow::~MainWindow() {
    m_connection->close();
}

void MainWindow::setupUI() {
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    
    QHBoxLayout *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    
    QWidget *leftPanel = new QWidget();
    leftPanel->setFixedWidth(280);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setSpacing(10);
    leftLayout->setContentsMargins(15, 15, 15, 15);
    
    QLabel *titleLabel = new QLabel("Чаты");
    titleLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: #4caf50;");
    leftLayout->addWidget(titleLabel);
    
    m_searchEdit = new QLineEdit();
    m_searchEdit->setPlaceholderText("Найти пользователя...");
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MainWindow::onSearchRequested);
    leftLayout->addWidget(m_searchEdit);
    
    m_contactsList = new QListWidget();
    m_contactsList->setSpacing(5);
    m_contactsList->setCursor(Qt::PointingHandCursor);
    connect(m_contactsList, &QListWidget::itemClicked, this, &MainWindow::onContactClicked);
    leftLayout->addWidget(m_contactsList);
    
    mainLayout->addWidget(leftPanel);
    
    QWidget *rightPanel = new QWidget();
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setSpacing(10);
    rightLayout->setContentsMargins(15, 15, 15, 15);
    
    QLabel *chatTitleLabel = new QLabel("Выберите контакт для начала переписки");
    chatTitleLabel->setStyleSheet("font-size: 16px; font-weight: bold;");
    chatTitleLabel->setAlignment(Qt::AlignCenter);
    rightLayout->addWidget(chatTitleLabel);
    
    m_messagesArea = new QTextEdit();
    m_messagesArea->setReadOnly(true);
    m_messagesArea->setStyleSheet("background-color: #1e1e1e;");
    rightLayout->addWidget(m_messagesArea, 1);
    
    QHBoxLayout *inputLayout = new QHBoxLayout();
    inputLayout->setSpacing(10);
    
    m_messageEdit = new QLineEdit();
    m_messageEdit->setPlaceholderText("Введите сообщение...");
    m_messageEdit->setMaxLength(Protocol::MaxMessageLength);
    m_messageEdit->setDisabled(true);
    connect(m_messageEdit, &QLineEdit::returnPressed, this, &MainWindow::onSendMessagePressed);
    inputLayout->addWidget(m_messageEdit);
    
    m_sendButton = new QPushButton("Отправить");
    m_sendButton->setDisabled(true);
    m_sendButton->setCursor(Qt::PointingHandCursor);
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::onSendMessageClicked);
    inputLayout->addWidget(m_sendButton);
    
    rightLayout->addLayout(inputLayout);
    mainLayout->addWidget(rightPanel, 1);
}

void MainWindow::applyStyles() {
    setStyleSheet(
        "QWidget {"
        "    background-color: #2b2b2b;"
        "    color: #ffffff;"
        "    font-family: 'Segoe UI', Arial, sans-serif;"
        "    font-size: 14px;"
        "}"
        "QListWidget {"
        "    background-color: #3c3c3c;"
        "    border: none;"
        "    border-radius: 6px;"
        "    padding: 5px;"
        "}"
        "QListWidget::item {"
        "    padding: 10px;"
        "    border-radius: 4px;"
        "    margin: 2px;"
        "}"
        "QListWidget::item:selected {"
        "    background-color: #4caf50;"
        "}"
        "QListWidget::item:hover:!selected {"
        "    background-color: #444444;"
        "}"
        "QLineEdit {"
        "    background-color: #3c3c3c;"
        "    border: 1px solid #555555;"
        "    border-radius: 6px;"
        "    padding: 8px;"
        "    color: #ffffff;"
        "}"
        "QLineEdit:focus {"
        "    border: 1px solid #4caf50;"
        "}"
        "QTextEdit {"
        "    background-color: #1e1e1e;"
        "    border: 1px solid #555555;"
        "    border-radius: 6px;"
        "    padding: 10px;"
        "    color: #ffffff;"
        "}"
        "QPushButton {"
        "    background-color: #4caf50;"
        "    border: none;"
        "    border-radius: 6px;"
        "    padding: 10px 20px;"
        "    color: #ffffff;"
        "    font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "    background-color: #45a049;"
        "}"
        "QPushButton:pressed {"
        "    background-color: #3d8b40;"
        "}"
        "QPushButton:disabled {"
        "    background-color: #555555;"
        "    color: #888888;"
        "}"
    );
}

void MainWindow::requestOfflineMessages() {
    qDebug() << "Requesting offline messages";
    m_connection->send(ProtocolHelper::createGetOfflineMessages());
}

void MainWindow::processServerMessage(const QJsonObject &message) {
    Protocol::MessageType type = ProtocolHelper::parseMessageType(message);
    qDebug() << "Server message type:" << static_cast<int>(type);
    
    switch (type) {
        case Protocol::MessageType::UserList: {
            qDebug() << "Received user list";
            QJsonArray usersArray = message[Protocol::USERS].toArray();
            QList<QJsonObject> users;
            for (const QJsonValue &val : usersArray) {
                users.append(val.toObject());
            }
            updateUserList(users);
            break;
        }
        
        case Protocol::MessageType::Message: {
            QString fromNick = ProtocolHelper::getString(message, Protocol::FROM_NICK);
            QString text = ProtocolHelper::getString(message, Protocol::TEXT);
            QString timestamp = ProtocolHelper::getString(message, Protocol::TIMESTAMP);
            qDebug() << "Message from" << fromNick << ":" << text;
            
            int fromUserId = message[Protocol::FROM_ID].toInt(-1);
            if (fromUserId <= 0) {
                for (const auto &user : m_allUsers) {
                    if (user.nick == fromNick) {
                        fromUserId = user.id;
                        break;
                    }
                }
            }
            
            if (fromUserId > 0) {
                if (!m_users.contains(fromUserId)) {
                    addUserToSidebar(fromUserId, fromNick);
                }
                
                int serverId = message[Protocol::ID].toInt(0);
                auto saved = m_database->saveIncoming(fromUserId, m_nick, fromNick, text, serverId);
                if (saved == DatabaseClient::SaveResult::Error) {
                    // Без подтверждения сервер доставит сообщение ещё раз
                    break;
                }
                if (serverId > 0) {
                    sendDeliveryAck({serverId});
                }
                
                if (saved == DatabaseClient::SaveResult::Saved) {
                    if (fromUserId == m_selectedUserId) {
                        appendMessage(fromNick, text, timestamp, false, false);
                        clearContactUnread(fromUserId);
                    } else {
                        markContactAsUnread(fromUserId);
                        qDebug() << "Непрочитанное сообщение от" << fromNick;
                    }
                }
            } else {
                qDebug() << "Не найден ID для ника:" << fromNick;
            }
            break;
        }
        
        case Protocol::MessageType::MessageHistory: {
            QJsonArray messagesArray = message["messages"].toArray();
            qDebug() << "Received offline messages, count:" << messagesArray.size();
            bool affectsSelectedContact = false;
            QList<int> ackIds;
            for (const QJsonValue &val : messagesArray) {
                QJsonObject msg = val.toObject();
                QString fromNick = msg["from_nick"].toString();
                QString text = msg["message"].toString();
                QString timestamp = msg["timestamp"].toString();
                
                int fromUserId = msg["from_id"].toInt(-1);
                if (fromUserId <= 0) {
                    for (const auto &user : m_allUsers) {
                        if (user.nick == fromNick) {
                            fromUserId = user.id;
                            break;
                        }
                    }
                }
                
                if (fromUserId > 0) {
                    if (!m_users.contains(fromUserId)) {
                        addUserToSidebar(fromUserId, fromNick);
                    }
                    
                    int serverId = msg["id"].toInt(0);
                    auto saved = m_database->saveIncoming(fromUserId, m_nick, fromNick, text, serverId);
                    if (saved == DatabaseClient::SaveResult::Error) {
                        continue;
                    }
                    if (serverId > 0) {
                        ackIds.append(serverId);
                    }
                    if (saved == DatabaseClient::SaveResult::Duplicate) {
                        continue;
                    }
                    
                    if (fromUserId == m_selectedUserId) {
                        affectsSelectedContact = true;
                    } else {
                        markContactAsUnread(fromUserId);
                    }
                }
            }
            if (affectsSelectedContact) {
                loadMessageHistory(m_selectedUserId);
            }
            if (!ackIds.isEmpty()) {
                sendDeliveryAck(ackIds);
                // Сервер отдаёт офлайн-сообщения порциями, запрашиваем следующую
                if (messagesArray.size() >= 100) {
                    requestOfflineMessages();
                }
            }
            break;
        }

        case Protocol::MessageType::MessageAck: {
            QString clientMsgId = ProtocolHelper::getString(message, Protocol::CLIENT_MSG_ID);
            int serverId = message[Protocol::ID].toInt(0);
            if (!clientMsgId.isEmpty() && serverId > 0
                && m_database->markOutgoingSent(m_nick, clientMsgId, serverId)
                && m_selectedUserId > 0) {
                loadMessageHistory(m_selectedUserId);
            }
            break;
        }
        
        case Protocol::MessageType::SearchUserResponse: {
            bool success = ProtocolHelper::getBool(message, Protocol::SUCCESS);
            QString msg = ProtocolHelper::getString(message, Protocol::MESSAGE);
            qDebug() << "Search user response:" << success << msg;
            
            if (success) {
                QJsonObject userObj = message["user"].toObject();
                int userId = userObj["id"].toInt();
                QString nick = userObj["nick"].toString();
                
                if (userId > 0 && !nick.isEmpty()) {
                    if (!m_users.contains(userId)) {
                        addUserToSidebar(userId, nick);
                    }
                    
                    m_selectedUserId = userId;
                    m_selectedUserNick = nick;
                    
                    m_messageEdit->setDisabled(false);
                    m_sendButton->setDisabled(false);
                    m_messageEdit->setFocus();
                    
                    loadMessageHistory(userId);
                    
                    for (int i = 0; i < m_contactsList->count(); ++i) {
                        QListWidgetItem *item = m_contactsList->item(i);
                        if (item->data(Qt::UserRole).toInt() == userId) {
                            m_contactsList->setCurrentItem(item);
                            break;
                        }
                    }
                }
            } else {
                QMessageBox::information(this, "Поиск", msg);
            }
            m_searchEdit->clear();
            break;
        }
        
        default:
            break;
    }
}

void MainWindow::updateUserList(const QList<QJsonObject> &users) {
    qDebug() << "Updating user list, count:" << users.size();
    m_contactsList->clear();
    m_users.clear();
    m_allUsers.clear();
    
    for (const auto &user : users) {
        int id = user["id"].toInt();
        QString nick = user["nick"].toString();
        qDebug() << "User:" << id << nick;
        
        UserInfo info;
        info.id = id;
        info.nick = nick;
        info.unread = false;
        
        m_allUsers[id] = info;
    }
    
    // Получатель неподтверждённого сообщения может быть неизвестен серверу (переписки ещё нет)
    for (const auto &pending : m_database->getPendingOutgoing(m_nick)) {
        if (!m_allUsers.contains(pending.userId) && !pending.peerNick.isEmpty()) {
            UserInfo info;
            info.id = pending.userId;
            info.nick = pending.peerNick;
            m_allUsers[pending.userId] = info;
        }
    }
    
    refreshSidebar();
    resendPendingMessages();
    requestOfflineMessages();
}

void MainWindow::sendPendingMessage(const PendingMessage &pending) {
    if (!m_connection->isAuthenticated() || pending.peerNick.isEmpty()) {
        return;
    }
    QJsonObject message = ProtocolHelper::createOutgoingMessage(pending.peerNick, pending.message, pending.clientMsgId);
    m_connection->send(message);
}

void MainWindow::resendPendingMessages() {
    const QList<PendingMessage> pending = m_database->getPendingOutgoing(m_nick);
    if (!pending.isEmpty()) {
        qDebug() << "Повторная отправка неподтверждённых сообщений:" << pending.size();
    }
    for (const auto &message : pending) {
        sendPendingMessage(message);
    }
}

void MainWindow::sendDeliveryAck(const QList<int> &ids) {
    if (!m_connection->isAuthenticated() || ids.isEmpty()) {
        return;
    }
    m_connection->send(ProtocolHelper::createDeliveryAck(ids));
}

void MainWindow::refreshSidebar() {
    QList<int> partnerIds = m_database->getConversationPartners(m_nick);
    qDebug() << "Conversation partners count:" << partnerIds.size();
    
    m_contactsList->clear();
    m_users.clear();
    
    for (int id : partnerIds) {
        if (m_allUsers.contains(id)) {
            UserInfo info = m_allUsers[id];
            m_users[id] = info;
            
            QListWidgetItem *item = new QListWidgetItem(info.nick);
            item->setData(Qt::UserRole, id);
            m_contactsList->addItem(item);
        }
    }
}

void MainWindow::addUserToSidebar(int userId, const QString &nick) {
    if (m_users.contains(userId)) {
        return;
    }
    
    UserInfo info;
    info.id = userId;
    info.nick = nick;
    info.unread = false;
    
    m_users[userId] = info;
    m_allUsers[userId] = info;
    
    QListWidgetItem *item = new QListWidgetItem(nick);
    item->setData(Qt::UserRole, userId);
    m_contactsList->addItem(item);
}

void MainWindow::onContactClicked(QListWidgetItem *item) {
    int userId = item->data(Qt::UserRole).toInt();
    
    clearContactUnread(userId);
    
    m_selectedUserId = userId;
    m_selectedUserNick = m_users[userId].nick;
    
    m_messageEdit->setDisabled(false);
    m_sendButton->setDisabled(false);
    m_messageEdit->setFocus();
    
    loadMessageHistory(userId);
    markMessagesReadOnServer(userId);
    m_database->markMessagesAsRead(userId, m_nick);
}

void MainWindow::loadMessageHistory(int userId) {
    m_messagesArea->clear();
    
    QList<MessageEntry> history = m_database->getMessageHistory(userId, m_nick, 200);
    
    qDebug() << "Загрузка истории для userId" << userId << ", найдено записей:" << history.size();
    
    for (const auto &entry : history) {
        appendMessage(entry.fromNick, entry.message, entry.timestamp, entry.isOutgoing, entry.isRead, entry.pending);
    }
    
    m_messagesArea->verticalScrollBar()->setValue(m_messagesArea->verticalScrollBar()->maximum());
}

void MainWindow::appendMessage(const QString &fromNick, const QString &text, 
                                const QString &timestamp, bool isOutgoing, bool isRead, bool pending) {
    QString prefix = isOutgoing ? "Вы" : fromNick;
    if (pending) {
        prefix += " ⏳";
    }
    
    QString html = m_messagesArea->toHtml();
    
    QString messageHtml = QString(
        "<div style='margin: 5px 0;'>"
        "<span style='color: #4caf50; font-weight: bold;'>%1:</span> "
        "<span>%2</span>"
        "</div>"
    ).arg(prefix, text.toHtmlEscaped());
    
    html += messageHtml;
    m_messagesArea->setHtml(html);
}

void MainWindow::markContactAsUnread(int userId) {
    if (!m_users.contains(userId)) {
        return;
    }
    
    m_users[userId].unread = true;
    
    for (int i = 0; i < m_contactsList->count(); ++i) {
        QListWidgetItem *item = m_contactsList->item(i);
        if (item->data(Qt::UserRole).toInt() == userId) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
            break;
        }
    }
}

void MainWindow::clearContactUnread(int userId) {
    if (!m_users.contains(userId)) {
        return;
    }
    
    m_users[userId].unread = false;
    
    for (int i = 0; i < m_contactsList->count(); ++i) {
        QListWidgetItem *item = m_contactsList->item(i);
        if (item->data(Qt::UserRole).toInt() == userId) {
            QFont font = item->font();
            font.setBold(false);
            item->setFont(font);
            break;
        }
    }
}

void MainWindow::onSearchRequested() {
    QString nick = m_searchEdit->text().trimmed();
    if (nick.isEmpty()) {
        return;
    }
    
    qDebug() << "Searching for user:" << nick;
    
    m_connection->send(ProtocolHelper::createSearchUserMessage(nick));
}

void MainWindow::onSendMessagePressed() {
    onSendMessageClicked();
}

void MainWindow::onSendMessageClicked() {
    QString text = m_messageEdit->text().trimmed();
    if (text.isEmpty() || m_selectedUserId <= 0) {
        return;
    }
    
    // Сообщение сохраняется как неподтверждённое и уходит серверу сразу, если есть связь,
    // иначе будет отправлено после переподключения
    QString clientMsgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!m_database->saveOutgoing(m_selectedUserId, m_nick, m_selectedUserNick, text, clientMsgId)) {
        QMessageBox::warning(this, "Ошибка", "Не удалось сохранить сообщение в локальную базу");
        return;
    }
    sendPendingMessage({m_selectedUserId, m_selectedUserNick, text, clientMsgId});
    
    appendMessage(m_nick, text, QDateTime::currentDateTime().toString(Qt::ISODate), true, false, true);
    
    m_messageEdit->clear();
}

void MainWindow::markMessagesReadOnServer(int userId) {
    m_connection->send(ProtocolHelper::createMarkMessagesRead(userId));
}
