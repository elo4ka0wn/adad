#include "mainwindow.h"

#include <QApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication application(argc, argv);

    QTranslator translator;
    const QString locale = QLocale::system().name();
    if (translator.load(QStringLiteral("qtbase_" + locale), QLibraryInfo::location(QLibraryInfo::TranslationsPath))) {
        application.installTranslator(&translator);
    }

    MainWindow window;
    window.show();

    return application.exec();
}
