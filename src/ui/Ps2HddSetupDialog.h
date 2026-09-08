#ifndef PS2HDDSETUPDIALOG_H
#define PS2HDDSETUPDIALOG_H

#include <QDialog>
#include <QString>

#include "core/PhysicalDisk.h"
#include "core/Ps2Apa.h"
#include "core/Ps2HddFormat.h"
#include "core/Ps2HddLayout.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class PrivilegedSession;

class Ps2HddSetupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit Ps2HddSetupDialog(PrivilegedSession *session, QWidget *parent = nullptr);

private:
    struct Candidate
    {
        Ps2::PhysicalDiskCandidate disk;
        std::vector<Ps2::ApaBankProbe> bankProbes;
        bool eligible = false;
        QString safetyMessage;
        QString probeError;
    };

    void scanDisks();
    void updateSelection();
    void refreshRows();
    void refreshBackendState();
    void updateWriteButton();
    void formatSelectedDisk();
    Ps2::HddLayoutMode selectedMode() const;
    Ps2::ProvisioningSelection provisioningSelection() const;
    QString provisioningSummary() const;
    QString candidateDetails(const Candidate &candidate) const;
    bool runProcessWithStatus(const QString &program, const QStringList &arguments,
            const QString &initialStatus, QString *combinedOutput);
    bool runWriterWithStatus(const QStringList &arguments,
            const QString &initialStatus, QString *combinedOutput);
    static void evaluateSafety(Candidate &candidate);
    static QString formatBytes(std::uint64_t bytes);
    static QString apaSummary(const Candidate &candidate);

    QComboBox *layoutMode = nullptr;
    QCheckBox *installOplCheck = nullptr;
    QCheckBox *configureOplCheck = nullptr;
    QCheckBox *installWleCheck = nullptr;
    QCheckBox *installMcaCheck = nullptr;
    QCheckBox *installFceummCheck = nullptr;
    QCheckBox *installFhdbCheck = nullptr;
    QCheckBox *installEnablerCheck = nullptr;
    QCheckBox *createFreeDvdBootCheck = nullptr;
    QComboBox *freeDvdBootProfile = nullptr;
    QComboBox *appsStorageSize = nullptr;
    QTreeWidget *diskList = nullptr;
    QLabel *detailsLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QLabel *backendLabel = nullptr;
    QPushButton *refreshButton = nullptr;
    QPushButton *writeButton = nullptr;
    std::vector<Candidate> candidates;
    QString pfsshellPath;
    QString writerPath;
    QString fetchPayloadsPath;
    QString freeDvdBootScriptPath;
    QString payloadPath;
    bool backendReady = false;
    PrivilegedSession *privilegedSession = nullptr;
};

#endif // PS2HDDSETUPDIALOG_H
