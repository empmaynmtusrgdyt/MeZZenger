#include "LoginWindow.h"
#include "Protocol.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include "ServerConnection.h"
#include <QDebug>

LoginWindow::LoginWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("MeZZenger - Вход");
    resize(400, 360);
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
    m_nickEdit->setMaxLength(Protocol::MaxNickLength);
    formLayout->addRow("Ник:", m_nickEdit);

    m_passwordEdit = new QLineEdit();
    m_passwordEdit->setPlaceholderText("Пароль");
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setMaxLength(Protocol::MaxPasswordLength);
    formLayout->addRow("Пароль:", m_passwordEdit);

    m_inviteEdit = new QLineEdit();
    m_inviteEdit->setPlaceholderText("только для регистрации");
    formLayout->addRow("Код приглашения:", m_inviteEdit);

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
    startAuth("login");
}

void LoginWindow::onRegisterClicked() {
    QString nick = getNick();
    QString password = getPassword();
    if (nick.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Введите ник и пароль");
        return;
    }
    if (nick.length() < Protocol::MinNickLength) {
        QMessageBox::warning(this, "Ошибка", QString("Ник должен быть не короче %1 символов").arg(Protocol::MinNickLength));
        return;
    }
    if (password.length() < Protocol::MinPasswordLength) {
        QMessageBox::warning(this, "Ошибка", QString("Пароль должен быть не менее %1 символов").arg(Protocol::MinPasswordLength));
        return;
    }
    if (m_inviteEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Для регистрации нужен код приглашения. Его выдаёт администратор сервера.");
        return;
    }
    startAuth("register");
}

void LoginWindow::setBusy(bool busy) {
    m_statusLabel->setText(busy ? "Подключение..." : QString());
    m_loginButton->setEnabled(!busy);
    m_registerButton->setEnabled(!busy);
}

void LoginWindow::startAuth(const QString &type) {
    qDebug() << "Auth request to" << getHost() << getPort() << type << getNick();
    setBusy(true);

    if (m_connection) {
        m_connection->deleteLater();
    }
    auto *connection = new ServerConnection(this);
    m_connection = connection;
    connection->setDialogParent(this);

    connect(connection, &ServerConnection::authenticated, this, [this, connection]() {
        emit loginSuccessful(connection);
    });
    connect(connection, &ServerConnection::authRejected, this, [this](const QString &message) {
        setBusy(false);
        QMessageBox::warning(this, "Ошибка", message);
    });
    connect(connection, &ServerConnection::failed, this, [this](const QString &error) {
        setBusy(false);
        QMessageBox::warning(this, "Ошибка", error);
    });

    connection->open(getHost(), static_cast<quint16>(getPort()), getNick(), getPassword(),
                     type == "register" ? m_inviteEdit->text().trimmed() : QString());
}
