#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTextBrowser>
#include <QSet>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMap>
#include <QJsonObject>
#include "DatabaseClient.h"
#include "Protocol.h"

class ServerConnection;
class FileTransfer;

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

    // Отправка файла выбранному собеседнику и скачивание (без targetPath спрашивает, куда сохранить);
    // вынесены из обработчиков кнопок, чтобы их можно было вызвать без файловых диалогов
    void attachFile(const QString &path);
    void downloadFile(const QString &fileId, const QString &targetPath = QString());

private slots:
    void onContactClicked(QListWidgetItem *item);
    void onFilterChanged(const QString &text);
    void onFilterEntered();
    void onChangePassword();
    void onAttachClicked();
    void onAnchorClicked(const QUrl &url);
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
    QTextBrowser *m_messagesArea;
    QLineEdit *m_messageEdit;
    QPushButton *m_sendButton;
    QPushButton *m_attachButton;
    
    int m_selectedUserId = -1;
    QString m_selectedUserNick;
    
    QMap<int, UserInfo> m_users;

    // Файл, выбранный для отправки, до получения разрешения сервера (ключ - ref запроса)
    struct UploadJob {
        QString path;
        QString name;
        int userId;
        QString peerNick;
        QString caption;
    };
    QMap<QString, UploadJob> m_uploads;
    QMap<QString, QString> m_downloadRequests;  // file_id -> куда сохранить (пусто - спросить)
    int m_activeTransfers = 0;
    
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
    void handleFileTicket(const QJsonObject &message);
    void startUpload(const UploadJob &job, const QString &fileId, const QString &token, qint64 size);
    void startDownload(const QString &token, const QString &name, qint64 size, const QString &targetPath);
    void attachTransfer(FileTransfer *transfer, const QString &label);
    FileAttachment fileFromMessage(const QJsonObject &message) const;
    static QString humanSize(qint64 bytes);
    int findUserIdByNick(const QString &nick) const;
    void loadMessageHistory(int userId);
    void appendMessage(const QString &fromNick, const QString &text, const QString &timestamp, bool isOutgoing, bool isRead, bool pending = false,
                       const FileAttachment &file = FileAttachment());
    void markContactAsUnread(int userId);
    void clearContactUnread(int userId);
    void markMessagesReadOnServer(int userId);
};

#endif
