#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTcpSocket>
#include <QTimer>
#include <QMap>
#include <QJsonObject>
#include <QAbstractSocket>
#include "DatabaseClient.h"
#include "Protocol.h"

struct UserInfo {
    int id;
    QString nick;
    bool unread = false;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QString &host, int port, const QString &nick, 
                        const QString &password, QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onContactClicked(QListWidgetItem *item);
    void onSearchRequested();
    void onSendMessageClicked();
    void onSendMessagePressed();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void onSocketError();
    void reconnectTimerTimeout();
    void onSocketConnected();

private:
    QString m_host;
    int m_port;
    QString m_nick;
    QString m_password;
    
    QTcpSocket *m_socket;
    QTimer *m_reconnectTimer;
    QByteArray m_buffer;
    
    DatabaseClient *m_database;
    
    QListWidget *m_contactsList;
    QLineEdit *m_searchEdit;
    QTextEdit *m_messagesArea;
    QLineEdit *m_messageEdit;
    QPushButton *m_sendButton;
    
    int m_selectedUserId = -1;
    QString m_selectedUserNick;
    
    QMap<int, UserInfo> m_users;
    QMap<int, UserInfo> m_allUsers;
    
    void setupUI();
    void applyStyles();
    void connectToServer();
    void sendAuthRequest(const QString &type);
    void requestOfflineMessages();
    void processServerMessage(const QJsonObject &message);
    void updateUserList(const QList<QJsonObject> &users);
    void refreshSidebar();
    void addUserToSidebar(int userId, const QString &nick);
    void loadMessageHistory(int userId);
    void appendMessage(const QString &fromNick, const QString &text, const QString &timestamp, bool isOutgoing, bool isRead);
    void markContactAsUnread(int userId);
    void clearContactUnread(int userId);
    void markMessagesReadOnServer(int userId);
};

#endif
