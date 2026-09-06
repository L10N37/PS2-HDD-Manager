#include "PrivilegedSession.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QThread>
#include <QWidget>

#ifdef __linux__
#include <unistd.h>
#endif

namespace
{
constexpr char Handshake[] = "PS2HDD-SESSION-1\n";
constexpr char RequestMagic[] = "P2RQ";
constexpr quint8 OutputFrame = 1;
constexpr quint8 DoneFrame = 2;
constexpr quint32 MaxArgumentBytes = 16U * 1024U * 1024U;
constexpr quint32 MaxOutputFrameBytes = 16U * 1024U * 1024U;
}

PrivilegedSession::PrivilegedSession(const QString &path, QObject *parent)
    : QObject(parent), writerPath(path)
{
}

PrivilegedSession::~PrivilegedSession()
{
    lock();
}

void PrivilegedSession::appendU32(QByteArray &buffer, quint32 value)
{
    buffer.append(static_cast<char>(value & 0xff));
    buffer.append(static_cast<char>((value >> 8) & 0xff));
    buffer.append(static_cast<char>((value >> 16) & 0xff));
    buffer.append(static_cast<char>((value >> 24) & 0xff));
}

quint32 PrivilegedSession::readU32(const char *data)
{
    return static_cast<quint32>(static_cast<unsigned char>(data[0])) |
            (static_cast<quint32>(static_cast<unsigned char>(data[1])) << 8) |
            (static_cast<quint32>(static_cast<unsigned char>(data[2])) << 16) |
            (static_cast<quint32>(static_cast<unsigned char>(data[3])) << 24);
}

bool PrivilegedSession::isUnlocked() const
{
    return process && process->state() == QProcess::Running;
}

bool PrivilegedSession::waitForHandshake(QWidget *dialogParent)
{
    QElapsedTimer timer;
    timer.start();
    QByteArray handshake;
    // Polkit can wait while the user enters a password, so allow two minutes.
    while (timer.elapsed() < 120000) {
        if (!process || process->state() == QProcess::NotRunning) {
            errorText = process ? QString::fromLocal8Bit(process->readAllStandardError()).trimmed()
                                : QStringLiteral("Privileged helper did not start.");
            if (errorText.isEmpty()) errorText = "Authentication was cancelled or the privileged helper exited.";
            return false;
        }
        if (process->waitForReadyRead(80)) {
            handshake += process->readAllStandardOutput();
            const int newline = handshake.indexOf('\n');
            if (newline >= 0) {
                const QByteArray line = handshake.left(newline + 1);
                responseBuffer = handshake.mid(newline + 1);
                if (line == Handshake)
                    return true;
                errorText = "Unexpected privileged-session handshake: " + QString::fromLocal8Bit(line).trimmed();
                return false;
            }
        }
        QApplication::processEvents();
        QThread::msleep(10);
    }
    Q_UNUSED(dialogParent);
    errorText = "Timed out waiting for the one-time privileged helper to unlock.";
    return false;
}

bool PrivilegedSession::unlock(QWidget *dialogParent)
{
#ifndef __linux__
    Q_UNUSED(dialogParent);
    errorText = "Persistent raw-HDD privilege sessions are currently Fedora/Linux only.";
    return false;
#else
    if (isUnlocked()) return true;
    lock();
    errorText.clear();
    if (!QFileInfo(writerPath).isExecutable()) {
        errorText = "PS2-HDD-Writer is not executable. Run prepare_fedora_test.sh.";
        return false;
    }

    process = new QProcess(this);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    QString program;
    QStringList arguments;
    if (geteuid() == 0) {
        program = writerPath;
        arguments << "--session-stdio" << "--client-uid" << QString::number(getuid());
    } else {
        program = "pkexec";
        arguments << writerPath << "--session-stdio" << "--client-uid" << QString::number(getuid());
    }
    process->start(program, arguments, QIODevice::ReadWrite);
    if (!process->waitForStarted(10000)) {
        errorText = process->errorString();
        lock();
        return false;
    }
    if (!waitForHandshake(dialogParent)) {
        lock();
        return false;
    }
    emit unlockedChanged(true);
    return true;
#endif
}

void PrivilegedSession::lock()
{
    if (!process) return;
    const bool wasUnlocked = process->state() != QProcess::NotRunning;
    if (process->state() == QProcess::Running) {
        process->closeWriteChannel(); // EOF tells the root helper to exit.
        if (!process->waitForFinished(1000)) {
            process->terminate();
            if (!process->waitForFinished(500)) process->kill();
        }
    }
    process->deleteLater();
    process = nullptr;
    responseBuffer.clear();
    busy = false;
    if (wasUnlocked) emit unlockedChanged(false);
}

bool PrivilegedSession::parseResponse(QByteArray &buffer, QString *output,
        const std::function<void(const QString &)> &outputCallback,
        bool *done, int *exitCode)
{
    while (buffer.size() >= 5) {
        const quint8 type = static_cast<quint8>(buffer.at(0));
        const quint32 length = readU32(buffer.constData() + 1);
        if (length > MaxOutputFrameBytes) {
            errorText = "Privileged helper sent an invalid oversized response frame.";
            return false;
        }
        if (buffer.size() < static_cast<int>(5 + length)) return true;
        const QByteArray payload = buffer.mid(5, static_cast<int>(length));
        buffer.remove(0, static_cast<int>(5 + length));

        if (type == OutputFrame) {
            const QString text = QString::fromLocal8Bit(payload);
            if (output) *output += text;
            if (outputCallback) outputCallback(text);
        } else if (type == DoneFrame) {
            if (length != 4) {
                errorText = "Privileged helper sent an invalid completion frame.";
                return false;
            }
            if (exitCode) *exitCode = static_cast<int>(readU32(payload.constData()));
            if (done) *done = true;
            return true;
        } else {
            errorText = "Privileged helper sent an unknown response frame.";
            return false;
        }
    }
    return true;
}

bool PrivilegedSession::run(const QStringList &modeArguments, QString *output,
        const std::function<void(const QString &)> &outputCallback)
{
#ifndef __linux__
    Q_UNUSED(modeArguments); Q_UNUSED(output); Q_UNUSED(outputCallback);
    errorText = "Persistent raw-HDD privilege sessions are currently Fedora/Linux only.";
    return false;
#else
    if (!isUnlocked()) {
        errorText = "PS2 HDD access is locked. Use Unlock once before accessing the physical HDD.";
        if (output) *output = errorText;
        return false;
    }
    if (busy) {
        errorText = "The privileged PS2 HDD session is already busy.";
        if (output) *output = errorText;
        return false;
    }
    if (modeArguments.contains("--session-stdio")) {
        errorText = "Nested privileged sessions are not permitted.";
        if (output) *output = errorText;
        return false;
    }

    busy = true;
    errorText.clear();
    if (output) output->clear();

    QByteArray request(RequestMagic, 4);
    appendU32(request, static_cast<quint32>(modeArguments.size()));
    for (const QString &argument : modeArguments) {
        const QByteArray bytes = argument.toUtf8();
        if (bytes.size() < 0 || static_cast<quint64>(bytes.size()) > MaxArgumentBytes) {
            busy = false;
            errorText = "A privileged helper argument is too large.";
            if (output) *output = errorText;
            return false;
        }
        appendU32(request, static_cast<quint32>(bytes.size()));
        request += bytes;
    }

    if (process->write(request) != request.size() || !process->waitForBytesWritten(10000)) {
        busy = false;
        errorText = "Could not send the operation to the unlocked PS2 HDD helper: " + process->errorString();
        if (output) *output = errorText;
        lock();
        return false;
    }

    bool done = false;
    int code = -1;
    while (!done) {
        if (!process || process->state() == QProcess::NotRunning) {
            errorText = "The privileged PS2 HDD helper exited unexpectedly.";
            if (process) {
                const QString stderrText = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
                if (!stderrText.isEmpty()) errorText += "\n" + stderrText;
            }
            if (output) *output += errorText;
            busy = false;
            lock();
            return false;
        }
        process->waitForReadyRead(80);
        responseBuffer += process->readAllStandardOutput();
        if (!parseResponse(responseBuffer, output, outputCallback, &done, &code)) {
            if (output) *output += "\n" + errorText;
            busy = false;
            lock();
            return false;
        }
        QApplication::processEvents();
        process->waitForFinished(10);
    }
    busy = false;
    return code == 0;
#endif
}
