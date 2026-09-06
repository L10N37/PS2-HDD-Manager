#ifndef PRIVILEGEDSESSION_H
#define PRIVILEGEDSESSION_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;

class PrivilegedSession : public QObject
{
    Q_OBJECT

public:
    explicit PrivilegedSession(const QString &writerPath, QObject *parent = nullptr);
    ~PrivilegedSession() override;

    bool unlock(QWidget *dialogParent = nullptr);
    void lock();
    bool isUnlocked() const;
    bool isBusy() const { return busy; }
    QString lastError() const { return errorText; }

    // Runs one existing PS2-HDD-Writer CLI operation through the already
    // authenticated root helper. modeArguments must NOT include the writer
    // executable itself. Output is the child's merged stdout/stderr.
    bool run(const QStringList &modeArguments, QString *output = nullptr,
            const std::function<void(const QString &)> &outputCallback = {});

signals:
    void unlockedChanged(bool unlocked);

private:
    static void appendU32(QByteArray &buffer, quint32 value);
    static quint32 readU32(const char *data);
    bool waitForHandshake(QWidget *dialogParent);
    bool parseResponse(QByteArray &buffer, QString *output,
            const std::function<void(const QString &)> &outputCallback,
            bool *done, int *exitCode);

    QString writerPath;
    QProcess *process = nullptr;
    bool busy = false;
    QString errorText;
    QByteArray responseBuffer;
};

#endif // PRIVILEGEDSESSION_H
