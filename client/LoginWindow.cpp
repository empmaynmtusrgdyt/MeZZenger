#include "LoginWindow.h"
#include "Protocol.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QTcpSocket>
#include <QTimer>
#include <QDebug>

LoginWindow::LoginWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("MeZZenger - Вход");
    resize(400, 300);
    setupUI();
    applyStyles();
}

LoginWindow::~LoginWindow() {}

void LoginWindow::setupUI() {
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setSpacing(20);
    mainLayout->setContentsMargins(30, 30, 30, 30);

    QLabel *titleLabel = new QLabel("MeZZenger");
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("font-size: 24px; font-weight: bold; color: #4caf50;");
    mainLayout->addWidget(titleLabel);
    mainLayout->addSpacing(20);

    QFormLayout *formLayout = new QFormLayout();
    formLayout->setSpacing(15);

    m_hostEdit = new QLineEdit();
    m_hostEdit->setPlaceholderText("localhost");
    m_hostEdit->setText("localhost");
    formLayout->addRow("Хост:", m_hostEdit);

    m_portEdit = new QLineEdit();
    m_portEdit->setPlaceholderText("5555");
    m_portEdit->setText("5555");
    m_portEdit->setMaxLength(5);
    formLayout->addRow("Порт:", m_portEdit);

    m_nickEdit = new QLineEdit();
    m_nickEdit->setPlaceholderText("Ваш ник");
    formLayout->addRow("Ник:", m_nickEdit);

    m_passwordEdit = new QLineEdit();
    m_passwordEdit->setPlaceholderText("Пароль");
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    formLayout->addRow("Пароль:", m_passwordEdit);

    mainLayout->addLayout(formLayout);
    mainLayout->addSpacing(20);

    m_statusLabel = new QLabel();
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("color: #888888;");
    mainLayout->addWidget(m_statusLabel);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(15);

    m_loginButton = new QPushButton("Вход");
    m_loginButton->setMinimumHeight(45);
    m_loginButton->setCursor(Qt::PointingHandCursor);
    connect(m_loginButton, &QPushButton::clicked, this, &LoginWindow::onLoginClicked);

    m_registerButton = new QPushButton("Регистрация");
    m_registerButton->setMinimumHeight(45);
    m_registerButton->setCursor(Qt::PointingHandCursor);
    connect(m_registerButton, &QPushButton::clicked, this, &LoginWindow::onRegisterClicked);

    buttonLayout->addWidget(m_loginButton);
    buttonLayout->addWidget(m_registerButton);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addStretch();
}

void LoginWindow::applyStyles() {
    setStyleSheet(
        "LoginWindow { background-color: #2b2b2b; }"
        "QWidget { color: #ffffff; font-family: 'Segoe UI', Arial, sans-serif; font-size: 14px; }"
        "QLineEdit { background-color: #3c3c3c; border: 1px solid #555555; border-radius: 6px; padding: 10px; color: #ffffff; }"
        "QLineEdit:focus { border: 1px solid #4caf50; }"
        "QPushButton { background-color: #4caf50; border: none; border-radius: 6px; padding: 10px 20px; color: #ffffff; font-weight: bold; }"
        "QPushButton:hover { background-color: #45a049; }"
        "QPushButton:pressed { background-color: #3d8b40; }"
    );
}

QString LoginWindow::getHost() const { return m_hostEdit->text().trimmed(); }
int LoginWindow::getPort() const { return m_portEdit->text().toInt(); }
QString LoginWindow::getNick() const { return m_nickEdit->text().trimmed(); }
QString LoginWindow::getPassword() const { return m_passwordEdit->text(); }

void LoginWindow::onLoginClicked() {
    QString nick = getNick();
    QString password = getPassword();
    if (nick.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Введите ник и пароль");
        return;
    }
    m_currentHost = getHost();
    m_currentPort = static_cast<quint16>(getPort());
    sendAuthRequest("login");
}

void LoginWindow::onRegisterClicked() {
    QString nick = getNick();
    QString password = getPassword();
    if (nick.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Введите ник и пароль");
        return;
    }
    if (password.length() < 4) {
        QMessageBox::warning(this, "Ошибка", "Пароль должен быть не менее 4 символов");
        return;
    }
    m_currentHost = getHost();
    m_currentPort = static_cast<quint16>(getPort());
    sendAuthRequest("register");
}

void LoginWindow::sendAuthRequest(const QString &type) {
    qDebug() << "Auth request to" << m_currentHost << m_currentPort << type << getNick();

    m_statusLabel->setText("Подключение...");
    m_loginButton->setEnabled(false);
    m_registerButton->setEnabled(false);

    QTcpSocket *socket = new QTcpSocket(this);
    QByteArray buffer;
    QTimer *timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);

    QJsonObject msg;
    msg["type"] = type;
    msg["nick"] = getNick();
    msg["password"] = getPassword();
    QByteArray data = serializeMessage(msg);

    connect(socket, &QTcpSocket::connected, [=]() {
        timeoutTimer->stop();
        socket->write(data);
        qDebug() << "Auth data sent";
    });

    connect(socket, &QTcpSocket::readyRead, [=]() mutable {
        buffer.append(socket->readAll());

        while (buffer.size() >= (int)HeaderSize) {
            uint32_t len = readHeader(buffer.left(HeaderSize));
            if (len == 0 || len > MaxMessageSize) {
                qDebug() << "Invalid message length";
                socket->close();
                return;
            }
            if (buffer.size() < (int)(HeaderSize + len))
                break;

            QByteArray jsonData = buffer.mid(HeaderSize, len);
            buffer = buffer.mid(HeaderSize + len);

            QJsonObject resp = deserializeMessage(jsonData);
            if (resp.isEmpty()) {
                qDebug() << "Failed to parse JSON";
                continue;
            }

            Protocol::MessageType msgType = ProtocolHelper::parseMessageType(resp);
            qDebug() << "Received message type:" << (int)msgType;

            if (msgType == Protocol::MessageType::AuthResponse) {
                bool ok = ProtocolHelper::getBool(resp, Protocol::SUCCESS);
                QString errMsg = ProtocolHelper::getString(resp, Protocol::MESSAGE);
                qDebug() << "Auth response" << ok << errMsg;

                if (ok) {
                    emit loginSuccessful();
                } else {
                    m_statusLabel->clear();
                    m_loginButton->setEnabled(true);
                    m_registerButton->setEnabled(true);
                    QMessageBox::warning(this, "Ошибка", errMsg);
                    socket->close();
                }
                return;
            }
        }
    });

    connect(socket, &QTcpSocket::errorOccurred, [=](QAbstractSocket::SocketError) {
        qDebug() << "Socket error:" << socket->errorString();
        timeoutTimer->stop();
        m_statusLabel->clear();
        m_loginButton->setEnabled(true);
        m_registerButton->setEnabled(true);
        QMessageBox::warning(this, "Ошибка", "Ошибка соединения: " + socket->errorString());
        socket->close();
    });

    connect(timeoutTimer, &QTimer::timeout, [=]() {
        qDebug() << "Connection timeout";
        socket->abort();
        m_statusLabel->clear();
        m_loginButton->setEnabled(true);
        m_registerButton->setEnabled(true);
        QMessageBox::warning(this, "Ошибка", "Превышено время подключения");
        socket->close();
    });

    timeoutTimer->start(5000);
    socket->connectToHost(m_currentHost, m_currentPort);
}
