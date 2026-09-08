#include "ui/MainWindow.h"
#include "DebugTrace.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMessageLogContext>
#include <QStyleFactory>


namespace
{
const char *qtTypeName(QtMsgType type)
{
    switch (type)
    {
        case QtDebugMsg:    return "QT_DEBUG";
        case QtInfoMsg:     return "QT_INFO";
        case QtWarningMsg:  return "QT_WARNING";
        case QtCriticalMsg: return "QT_CRITICAL";
        case QtFatalMsg:    return "QT_FATAL";
    }
    return "QT_UNKNOWN";
}

void ps2QtMessageHandler(
        QtMsgType type,
        const QMessageLogContext &context,
        const QString &message)
{
    const QByteArray bytes =
            message.toUtf8();

    std::string line =
            std::string(qtTypeName(type)) +
            " | " +
            bytes.constData();

    if (context.file)
    {
        line += " | ";
        line += context.file;
        line += ':';
        line += std::to_string(context.line);
    }

    DebugTrace::write(line);

    if (type == QtFatalMsg)
        std::abort();
}
}

int main(int argc, char *argv[])
{
    DebugTrace::installCrashHandlers();
    qInstallMessageHandler(ps2QtMessageHandler);
    DebugTrace::write("main: constructing QApplication");

    QApplication application(argc, argv);
    DebugTrace::write("main: QApplication constructed");
    QCoreApplication::setApplicationName("PS2 HDD Manager");
    QCoreApplication::setApplicationVersion(PS2_HDD_APP_VERSION);
    QCoreApplication::setOrganizationName("L10N37");
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    DebugTrace::write("main: constructing MainWindow");
    MainWindow window;
    DebugTrace::write("main: MainWindow constructed");
    window.showMaximized();
    DebugTrace::write("main: entering Qt event loop");
    const int result = application.exec();
    DebugTrace::write(
            "main: Qt event loop exited rc=" +
            std::to_string(result));
    return result;
}
