// Настоящие окна клиента (LoginWindow -> MainWindow), управляемые скриптом, для интеграционных тестов.
// Печатает строки "HARNESS <событие>: <данные>" в stdout, по ним тесты проверяют поведение.
// Поля ввода ищутся по порядку (у окон нет имён для тестов), поэтому порядок проверяется при запуске.
#include <QApplication>
#include <QCommandLineParser>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStyleFactory>
#include <QTextEdit>
#include <QTimer>
#include <cstdio>
#include "LoginWindow.h"
#include "MainWindow.h"

static void report(const QString &kind, const QString &data = QString()) {
    QString line = data;
    line.replace('\n', ' ');
    fprintf(stdout, "HARNESS %s: %s\n", qPrintable(kind), qPrintable(line));
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setOrganizationName("MeZZengerTest");
    app.setApplicationName("client_harness");

    QCommandLineParser parser;
    parser.addOption({"nick", "ник", "nick"});
    parser.addOption({"mode", "reg или login", "mode", "login"});
    parser.addOption({"host", "хост", "host", "127.0.0.1"});
    parser.addOption({"port", "порт", "port"});
    parser.addOption({"password", "пароль", "password", "pass1234"});
    parser.addOption({"invite", "код приглашения", "invite"});
    parser.addOption({"pin", "закреплённый отпечаток сертификата (без параметра - первый вход)", "fingerprint"});
    parser.addOption({"answer", "ответ на вопросы Да/Нет: yes или no", "answer", "yes"});
    parser.addOption({"script", "шаги: мс:действие:аргумент,... (search, send, dump, setpin)", "script"});
    parser.addOption({"duration", "сколько работать, мс", "ms", "6000"});
    parser.addOption({"settings-dir", "каталог настроек (отпечатки серверов)", "dir"});
    parser.process(app);

    QSettings::setDefaultFormat(QSettings::IniFormat);
    if (parser.isSet("settings-dir")) {
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, parser.value("settings-dir"));
    }
    const QString pinKey = QString("trusted/%1:%2").arg(parser.value("host"), parser.value("port"));
    {
        QSettings settings;
        settings.clear();
        if (parser.isSet("pin")) {
            settings.setValue(pinKey, parser.value("pin"));
        }
    }

    // Отвечает на все диалоги: вопросы о сертификате по --answer, остальные закрывает кнопкой OK
    QTimer clicker;
    clicker.setInterval(100);
    QObject::connect(&clicker, &QTimer::timeout, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box) {
            return;
        }
        report("dialog", box->text());
        QAbstractButton *button = parser.value("answer") == "yes" ? box->button(QMessageBox::Yes) : box->button(QMessageBox::No);
        if (!button) {
            button = box->button(QMessageBox::Ok);
        }
        if (button) {
            button->click();
        } else {
            box->close();
        }
    });
    clicker.start();

    LoginWindow login;
    login.show();
    MainWindow *mainWindow = nullptr;

    QObject::connect(&login, &LoginWindow::loginSuccessful, [&](ServerConnection *connection) {
        report("login-successful");
        mainWindow = new MainWindow(connection, login.getNick());
        mainWindow->show();
        login.hide();

        const auto edits = mainWindow->findChildren<QLineEdit *>();
        if (edits.size() != 2) {
            report("error", "ожидалось 2 поля ввода в главном окне, найдено " + QString::number(edits.size()));
            app.exit(2);
            return;
        }

        for (const QString &step : parser.value("script").split(',', Qt::SkipEmptyParts)) {
            const QStringList parts = step.split(':');
            const int at = parts[0].toInt();
            const QString action = parts.value(1);
            const QString arg = parts.mid(2).join(':');
            QTimer::singleShot(at, [=]() {
                if (action == "search") {
                    edits[0]->setText(arg);
                    emit edits[0]->returnPressed();
                } else if (action == "send") {
                    edits[1]->setText(arg);
                    emit edits[1]->returnPressed();
                } else if (action == "dump") {
                    report("view", mainWindow->findChild<QTextEdit *>()->toPlainText());
                } else if (action == "attach") {
                    mainWindow->attachFile(arg);
                } else if (action == "download") {
                    // Скачивает все вложения открытого чата в каталог arg под именем file_id
                    const QString html = mainWindow->findChild<QTextEdit *>()->toHtml();
                    QRegularExpression link("href=\"file:([0-9a-f]+)\"");
                    for (auto it = link.globalMatch(html); it.hasNext();) {
                        const QString id = it.next().captured(1);
                        mainWindow->downloadFile(id, arg + "/" + id);
                    }
                } else if (action == "contacts") {
                    QStringList items;
                    auto *list = mainWindow->findChild<QListWidget *>();
                    for (int i = 0; i < list->count(); ++i) {
                        if (!list->item(i)->isHidden()) {
                            items << list->item(i)->text();
                        }
                    }
                    report("contacts", items.join('|'));
                } else if (action == "wintitle") {
                    report("wintitle", mainWindow->windowTitle());
                } else if (action == "setpin") {
                    QSettings().setValue(pinKey, arg);
                }
                report("step", action + " " + arg);
            });
        }
    });

    const auto edits = login.findChildren<QLineEdit *>();
    if (edits.size() != 5) {
        report("error", "ожидалось 5 полей ввода в окне входа, найдено " + QString::number(edits.size()));
        return 2;
    }
    edits[0]->setText(parser.value("host"));
    edits[1]->setText(parser.value("port"));
    edits[2]->setText(parser.value("nick"));
    edits[3]->setText(parser.value("password"));
    edits[4]->setText(parser.value("invite"));

    const QString buttonText = parser.value("mode") == "reg" ? "Регистрация" : "Вход";
    for (auto *button : login.findChildren<QPushButton *>()) {
        if (button->text() == buttonText) {
            QTimer::singleShot(300, [button]() { button->click(); });
            break;
        }
    }

    QTimer::singleShot(parser.value("duration").toInt(), [&]() {
        const auto labels = login.findChildren<QLabel *>();
        report("end", QString("login_visible=%1 main_visible=%2 buttons_enabled=%3 status=%4 pin=%5")
                          .arg(login.isVisible()).arg(mainWindow && mainWindow->isVisible())
                          .arg(login.findChildren<QPushButton *>().first()->isEnabled())
                          .arg(labels.isEmpty() ? QString() : labels.last()->text())
                          .arg(QSettings().value(pinKey).toString()));
        app.quit();
    });

    return app.exec();
}
