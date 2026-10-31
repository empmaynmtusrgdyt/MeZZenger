#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMap>
#include <QJsonObject>
#include "DatabaseClient.h"
#include "Protocol.h"

class ServerConnection;

struct UserInfo {
    int id;
    QString nick;
    bool unread = false;
    bool online = false;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // Забирает соединение, в котором уже прошёл вход
    explicit MainWindow(ServerConnection *connection, const QString &nick, QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onContactClicked(QListWidgetItem *item);
    void onFilterChanged(const QString &text);
    void onFilterEntered();
    void onChangePassword();
    void onSendMessageClicked();
    void onSendMessagePressed();

private:
    QString m_nick;
    ServerConnection *m_connection;
    
    DatabaseClient *m_database;
    
    QListWidget *m_contactsList;
    QLineEdit *m_searchEdit;
    QLabel *m_chatTitle;
    QSystemTrayIcon *m_tray = nullptr;
    QString m_pendingNewPassword;
    QTextEdit *m_messagesArea;
    QLineEdit *m_messageEdit;
    QPushButton *m_sendButton;
    
    int m_selectedUserId = -1;
    QString m_selectedUserNick;
    
    QMap<int, UserInfo> m_users;
    
    void setupUI();
    void applyStyles();
    void requestOfflineMessages();
    void sendPendingMessage(const PendingMessage &pending);
    void resendPendingMessages();
    void sendDeliveryAck(const QList<int> &ids);
    void processServerMessage(const QJsonObject &message);
    void updateUserList(const QList<QJsonObject> &users);
    void setupMenu();
    void upsertUser(int userId, const QString &nick, bool online);
    void rebuildSidebar();
    void applyFilter();
    void selectUser(int userId);
    void updateChatTitle();
    void updateWindowTitle();
    void notifyIncoming(const QString &nick, const QString &text);
    int findUserIdByNick(const QString &nick) const;
    void loadMessageHistory(int userId);
    void appendMessage(const QString &fromNick, const QString &text, const QString &timestamp, bool isOutgoing, bool isRead, bool pending = false);
    void markContactAsUnread(int userId);
    void clearContactUnread(int userId);
    void markMessagesReadOnServer(int userId);
};

#endif
