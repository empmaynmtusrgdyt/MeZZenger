#ifndef FILETRANSFER_H
#define FILETRANSFER_H

#include <QObject>
#include <QSslSocket>
#include <QFile>
#include <QTimer>
#include <QWidget>
#include <functional>

// Передача одного файла отдельным TLS-соединением по одноразовому токену от сервера.
// Основной канал при этом не занят: сообщения идут как обычно.
class FileTransfer : public QObject {
    Q_OBJECT

public:
    enum class Mode { Upload, Download };

    FileTransfer(Mode mode, const QString &host, quint16 port, const QString &token,
                 const QString &localPath, qint64 expectedSize,
                 std::function<QWidget *()> dialogParent, QObject *parent = nullptr);

    void start();
    void cancel();

    Mode mode() const { return m_mode; }
    QString localPath() const { return m_localPath; }

signals:
    void progress(qint64 done, qint64 total);
    void finished(bool ok, const QString &error);

private:
    Mode m_mode;
    QString m_host;
    quint16 m_port;
    QString m_token;
    QString m_localPath;
    qint64 m_expectedSize;
    std::function<QWidget *()> m_dialogParent;

    QSslSocket *m_socket;
    QTimer *m_connectTimer;
    QFile m_file;
    QByteArray m_buffer;
    qint64 m_total = 0;
    qint64 m_done = 0;
    bool m_gotInfo = false;
    bool m_allSent = false;
    bool m_finished = false;

    void onEncrypted();
    void onReadyRead();
    void pumpUpload();
    void handleDownloadData();
    void handleUploadReply();
    void finish(bool ok, const QString &error = QString());
};

#endif
