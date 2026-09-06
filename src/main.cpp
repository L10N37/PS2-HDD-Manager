#include "ui/MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName("PS2 HDD Manager");
    QCoreApplication::setApplicationVersion(PS2_HDD_APP_VERSION);
    QCoreApplication::setOrganizationName("L10N37");
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    MainWindow window;
    window.showMaximized();
    return application.exec();
}
