#include "MainWindow.h"
#include "Protocol.h"
#include <QMessageBox>
#include <QJsonDocument>
#include <QDateTime>
#include <QScrollBar>
#include <QFont>
#include <QDebug>

MainWindow::MainWindow(const QString &host, int port, const QString &nick,
                       const QString &password, QWidget *parent)
    : QMainWindow(parent)
    , m_host(host)
    , m_port(port)
    , m_nick(nick)
    , m_password(password)
    , m_socket(new QTcpSocket(this))
    , m_reconnectTimer(new QTimer(this))
    , m_database(new DatabaseClient(this))
{
    qDebug() << "MainWindow constructor called";
    setWindowTitle("MeZZenger - " + nick);
    resize(900, 600);
    
    m_database->initialize("mezzenger_client.db");
    
    setupUI();
    applyStyles();
    
    connect(m_socket, &QTcpSocket::readyRead, this, &MainWindow::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &MainWindow::onSocketDisconnected);
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::errorOccurred),
            this, &MainWindow::onSocketError);
    connect(m_socket, &QTcpSocket::connected, this, &MainWindow::onSocketConnected);
    
    connect(m_reconnectTimer, &QTimer::timeout, this, &MainWindow::reconnectTimerTimeout);
    m_reconnectTimer->setSingleShot(true);
    
    qDebug() << "Connecting to server" << host << port;
    connectToServer();
}

MainWindow::~MainWindow() {
    m_socket->disconnectFromHost();
    m_socket->close();
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

void MainWindow::connectToServer() {
    m_socket->connectToHost(m_host, m_port);
}

void MainWindow::sendAuthRequest(const QString &type) {
    qDebug() << "Sending auth request:" << type << "nick:" << m_nick;
    QJsonObject message;
    message["type"] = type;
    message["nick"] = m_nick;
    message["password"] = m_password;
    
    QByteArray data = serializeMessage(message);
    m_socket->write(data);
}

void MainWindow::requestOfflineMessages() {
    qDebug() << "Requesting offline messages";
    QJsonObject request;
    request["type"] = "get_offline_messages";
    QByteArray data = serializeMessage(request);
    m_socket->write(data);
}

void MainWindow::onSocketReadyRead() {
    qDebug() << "Data received from server";
    m_buffer += m_socket->readAll();
    
    while (m_buffer.size() >= static_cast<int>(HeaderSize)) {
        uint32_t messageLength = readHeader(m_buffer.left(HeaderSize));
        
        if (messageLength == 0 || messageLength > MaxMessageSize) {
            m_buffer.clear();
            return;
        }
        
        if (m_buffer.size() < static_cast<int>(HeaderSize + messageLength)) {
            break;
        }
        
        QByteArray messageData = m_buffer.mid(HeaderSize, messageLength);
        m_buffer = m_buffer.mid(HeaderSize + messageLength);
        
        QJsonObject message = deserializeMessage(messageData);
        if (message.isEmpty()) {
            continue;
        }
        
        processServerMessage(message);
    }
}

void MainWindow::processServerMessage(const QJsonObject &message) {
    Protocol::MessageType type = ProtocolHelper::parseMessageType(message);
    qDebug() << "Server message type:" << static_cast<int>(type);
    
    switch (type) {
        case Protocol::MessageType::AuthResponse: {
            bool success = ProtocolHelper::getBool(message, Protocol::SUCCESS);
            QString msg = ProtocolHelper::getString(message, Protocol::MESSAGE);
            qDebug() << "Auth response success:" << success << "msg:" << msg;
            
            if (success) {
            } else {
                QMessageBox::warning(this, "Ошибка", msg);
                close();
            }
            break;
        }
        
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
                
                m_database->saveMessage(fromUserId, m_nick, text, false, fromNick);
                
                if (fromUserId == m_selectedUserId) {
                    appendMessage(fromNick, text, timestamp, false, false);
                    clearContactUnread(fromUserId);
                } else {
                    markContactAsUnread(fromUserId);
                    qDebug() << "Непрочитанное сообщение от" << fromNick;
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
                    
                    m_database->saveMessage(fromUserId, m_nick, text, false, fromNick);
                    
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
    
    refreshSidebar();
    requestOfflineMessages();
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
        appendMessage(entry.fromNick, entry.message, entry.timestamp, entry.isOutgoing, entry.isRead);
    }
    
    m_messagesArea->verticalScrollBar()->setValue(m_messagesArea->verticalScrollBar()->maximum());
}

void MainWindow::appendMessage(const QString &fromNick, const QString &text, 
                                const QString &timestamp, bool isOutgoing, bool isRead) {
    QString prefix = isOutgoing ? "Вы" : fromNick;
    
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
    
    QJsonObject searchRequest = ProtocolHelper::createSearchUserMessage(nick);
    QByteArray data = serializeMessage(searchRequest);
    m_socket->write(data);
}

void MainWindow::onSendMessagePressed() {
    onSendMessageClicked();
}

void MainWindow::onSendMessageClicked() {
    QString text = m_messageEdit->text().trimmed();
    if (text.isEmpty() || m_selectedUserId <= 0) {
        return;
    }
    
    QJsonObject message;
    message["type"] = "message";
    message["to_nick"] = m_selectedUserNick;
    message["text"] = text;
    
    QByteArray data = serializeMessage(message);
    m_socket->write(data);
    
    m_database->saveMessage(m_selectedUserId, m_nick, text, true, m_nick);
    
    appendMessage(m_nick, text, QDateTime::currentDateTime().toString(Qt::ISODate), true, false);
    
    m_messageEdit->clear();
}

void MainWindow::markMessagesReadOnServer(int userId) {
    QJsonObject request = ProtocolHelper::createMarkMessagesRead(userId);
    QByteArray data = serializeMessage(request);
    m_socket->write(data);
}

void MainWindow::onSocketDisconnected() {
    qDebug() << "Disconnected from server";
    m_reconnectTimer->start(3000);
}

void MainWindow::onSocketError() {
    qDebug() << "Socket error:" << m_socket->errorString();
}

void MainWindow::reconnectTimerTimeout() {
    qDebug() << "Reconnect timeout, attempting to reconnect";
    if (!m_socket->isOpen()) {
        connectToServer();
    }
}

void MainWindow::onSocketConnected() {
    qDebug() << "Socket connected, sending login request for" << m_nick;
    sendAuthRequest("login");
}
