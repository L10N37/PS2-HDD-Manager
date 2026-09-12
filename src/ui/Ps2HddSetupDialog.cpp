#include "Ps2HddSetupDialog.h"
#include "PrivilegedSession.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <utility>

#ifdef __linux__
#include <unistd.h>
#endif

Ps2HddSetupDialog::Ps2HddSetupDialog(PrivilegedSession *session, QWidget *parent) : QDialog(parent), privilegedSession(session)
{
    setWindowTitle("PS2 HDD Setup - Fast APA + OPL / FHDB Provisioning");
    resize(1160, 820);
    setMinimumSize(940, 650);

    auto *layout = new QVBoxLayout(this);
    auto *notice = new QLabel(
            "v0.2.0: Extended APA Banks creates a conventional bootable Bank 0 "
            "plus games-only Bank 1+. OPL/PFS/FHDB stay in Bank 0 and the GUI can target either bank. Backends "
            "and downloads are persistently cached. FHDB still requires the console EEPROM HDD-boot "
            "setting to be enabled once per console.",
            this);
    notice->setWordWrap(true);
    notice->setStyleSheet("font-weight: 600; color: #7a4b00;");
    layout->addWidget(notice);

    auto *options = new QGroupBox("Target layout", this);
    auto *optionLayout = new QFormLayout(options);
    layoutMode = new QComboBox(options);
    layoutMode->addItem("Standard APA / PFS (current OPL, up to 2 TiB)",
            static_cast<int>(Ps2::HddLayoutMode::StandardApa));
    layoutMode->addItem("Extended APA banks (>2 TiB)",
            static_cast<int>(Ps2::HddLayoutMode::ExtendedApaBanks));
    optionLayout->addRow("Disk layout:", layoutMode);
    auto *stageInfo = new QLabel(
            "Fast format skips PS2SDK's historical whole-media APA-header scrub, then verifies the "
            "new linked APA chain and mounts every standard PFS system partition.", options);
    stageInfo->setWordWrap(true);
    optionLayout->addRow(QString(), stageInfo);
    layout->addWidget(options);

    auto *provision = new QGroupBox("OPL + optional applications", this);
    auto *provisionLayout = new QFormLayout(provision);

    installOplCheck = new QCheckBox("Install latest official OPL Beta (recommended)", provision);
    installOplCheck->setChecked(true);
    configureOplCheck = new QCheckBox(
            "Preconfigure OPL for Internal HDD: auto-start HDD, start on HDD games, cover art, write ops, cache and autosort/autorefresh", provision);
    configureOplCheck->setChecked(true);
    configureOplCheck->setToolTip(
            "Writes conf_opl.cfg directly so the first OPL boot is already configured for an internal-HDD build.");

    // PS2_HDD_FORMAT_IGR_DEFAULT_V1
    installHddIgrCheck = new QCheckBox(
            "Install HDD IGR Return (recommended)", provision);
    installHddIgrCheck->setChecked(true);
    installHddIgrCheck->setToolTip(
            "Copies the same matching OPL build to /OPL/IGR.ELF and preconfigures "
            "exit_path=hdd0:PP.FHDB.APPS:pfs:/OPL/IGR.ELF. "
            "This direct HDD return path was hardware-validated before release.");

    appsStorageSize = new QComboBox(provision);
    appsStorageSize->addItem("512 MiB", 512);
    appsStorageSize->addItem("1 GiB", 1024);
    appsStorageSize->addItem("2 GiB", 2048);
    appsStorageSize->addItem("4 GiB (recommended)", 4096);
    appsStorageSize->addItem("8 GiB", 8192);
    appsStorageSize->addItem("16 GiB", 16384);
    appsStorageSize->addItem("32 GiB", 32768);
    appsStorageSize->setCurrentIndex(
            appsStorageSize->findData(4096));
    appsStorageSize->setToolTip(
            "Bank-0 PFS space shared by OPL, Apps, ART, CFG, ROMS and emulator saves.");

    auto *appsLabel =
            new QLabel(
                "Additional preconfigured OPL Apps",
                provision);
    appsLabel->setStyleSheet("font-weight: 600;");
    installWleCheck = new QCheckBox("wLaunchELF_ISR - latest normal BOOT.ELF", provision);
    installWleCheck->setChecked(true);
    installMcaCheck =
            new QCheckBox(
                "Memory Card Annihilator - latest automated build",
                provision);
    installMcaCheck->setChecked(true);

    installFceummCheck =
            new QCheckBox(
                "FCEUmm-PS2 SMB - NES emulator + HDD/SMB ROM browser",
                provision);
    installFceummCheck->setChecked(true);
    installFceummCheck->setToolTip(
            "Installs the pinned tested FCEUmm-PS2 SMB release, creates /OPL/ROMS/NES and /OPL/SAVES/FCEUMM, and preconfigures HDD paths.");
    installEnablerCheck = new QCheckBox("FHDB HDD Boot Configuration - Status / Enable / Disable / Verify", provision);
    installEnablerCheck->setChecked(true);

    installFhdbCheck = new QCheckBox("Install FreeHDBoot 1.966 and configure OPL autoboot", provision);
    installFhdbCheck->setChecked(true);
    createFreeDvdBootCheck = new QCheckBox("Also create a FreeDVDBoot ISO containing the HDD-boot configuration utility", provision);
    createFreeDvdBootCheck->setChecked(false);
    freeDvdBootProfile = new QComboBox(provision);
    freeDvdBootProfile->addItem("All PS2 slims - DVD Player 3.10 / 3.11", "slim");
    freeDvdBootProfile->addItem("DVD Player 2.10 - 2.13 (phat, experimental custom disc)", "2.10-2.13");
    freeDvdBootProfile->addItem("DVD Player 3.04M+ English (phat, experimental custom disc)", "3.04M");
    freeDvdBootProfile->setEnabled(false);

    provisionLayout->addRow(QString(), installOplCheck);
    provisionLayout->addRow(QString(), configureOplCheck);
    provisionLayout->addRow(QString(), installHddIgrCheck);
    provisionLayout->addRow(
            "Apps / ART / ROMS reserve:",
            appsStorageSize);
    provisionLayout->addRow(QString(), appsLabel);
    provisionLayout->addRow(QString(), installWleCheck);
    provisionLayout->addRow(QString(), installMcaCheck);
    provisionLayout->addRow(QString(), installFceummCheck);
    provisionLayout->addRow(QString(), installEnablerCheck);
    provisionLayout->addRow(QString(), installFhdbCheck);
    provisionLayout->addRow(QString(), createFreeDvdBootCheck);
    provisionLayout->addRow("FreeDVDBoot profile:", freeDvdBootProfile);
    auto *fhdbWarning = new QLabel(
            "The dedicated FHDB HDD Boot Configuration app starts read-only, displays the current EEPROM state, "
            "and requires confirmation before Enable or Disable. Every write is verified by an immediate re-read.", provision);
    fhdbWarning->setWordWrap(true);
    provisionLayout->addRow(QString(), fhdbWarning);
    layout->addWidget(provision);

    const auto updateIgrSetupOption = [this]() {
        const bool available =
                installOplCheck->isChecked() &&
                configureOplCheck->isChecked();
        installHddIgrCheck->setEnabled(available);
        if (!available)
            installHddIgrCheck->setChecked(false);
    };

    connect(installOplCheck, &QCheckBox::toggled,
            this, [updateIgrSetupOption](bool) {
        updateIgrSetupOption();
    });
    connect(configureOplCheck, &QCheckBox::toggled,
            this, [updateIgrSetupOption](bool) {
        updateIgrSetupOption();
    });
    updateIgrSetupOption();

    connect(createFreeDvdBootCheck, &QCheckBox::toggled, this, [this](bool checked) {
        freeDvdBootProfile->setEnabled(checked);
        if (checked)
            installEnablerCheck->setChecked(true);
    });

    backendLabel = new QLabel(this);
    backendLabel->setWordWrap(true);
    backendLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(backendLabel);

    diskList = new QTreeWidget(this);
    diskList->setColumnCount(8);
    diskList->setHeaderLabels({ "Disk", "Model", "Serial", "Capacity", "Bus",
            "Logical / physical", "APA banks", "Safety result" });
    diskList->setSelectionMode(QAbstractItemView::SingleSelection);
    diskList->setSelectionBehavior(QAbstractItemView::SelectRows);
    diskList->setSortingEnabled(true);
    diskList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    diskList->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    diskList->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    diskList->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    diskList->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    diskList->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    diskList->header()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    diskList->header()->setSectionResizeMode(7, QHeaderView::Stretch);
    layout->addWidget(diskList, 1);

    detailsLabel = new QLabel("Select a disk to inspect its proposed PS2 layout.", this);
    detailsLabel->setWordWrap(true);
    detailsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailsLabel->setMinimumHeight(130);
    layout->addWidget(detailsLabel);

    statusLabel = new QLabel("No disk has been modified.", this);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(statusLabel);

    auto *buttons = new QDialogButtonBox(this);
    refreshButton = buttons->addButton("Refresh Disks", QDialogButtonBox::ActionRole);
    writeButton = buttons->addButton("Fast Format + Selected Setup", QDialogButtonBox::DestructiveRole);
    writeButton->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(refreshButton, &QPushButton::clicked, this, &Ps2HddSetupDialog::scanDisks);
    connect(writeButton, &QPushButton::clicked, this, &Ps2HddSetupDialog::formatSelectedDisk);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(diskList, &QTreeWidget::itemSelectionChanged, this, &Ps2HddSetupDialog::updateSelection);
    connect(layoutMode, &QComboBox::currentIndexChanged, this, [this]() {
        const bool extended = selectedMode() == Ps2::HddLayoutMode::ExtendedApaBanks;
        installOplCheck->setText(extended
                ? "Install Open PS2 Loader Extended APA (required for Bank 1+)"
                : "Install latest official OPL Beta (recommended)");
        if (extended)
            installOplCheck->setChecked(true);
        installOplCheck->setEnabled(!extended);
        refreshRows();
        updateSelection();
        updateWriteButton();
    });

    refreshBackendState();
#if defined(_WIN32) || defined(__linux__)
    scanDisks();
#else
    refreshButton->setEnabled(false);
    detailsLabel->setText("Physical-disk inspection is currently available on Windows and Linux.");
#endif
}

QString Ps2HddSetupDialog::formatBytes(std::uint64_t bytes)
{
    static const char *suffixes[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double value = static_cast<double>(bytes);
    int suffix = 0;
    while (value >= 1024.0 && suffix < 4) { value /= 1024.0; suffix++; }
    return suffix == 0 ? QString::number(bytes) + " B" :
            QString::number(value, 'f', 2) + " " + suffixes[suffix];
}

Ps2::HddLayoutMode Ps2HddSetupDialog::selectedMode() const
{
    return static_cast<Ps2::HddLayoutMode>(layoutMode->currentData().toInt());
}

Ps2::ProvisioningSelection Ps2HddSetupDialog::provisioningSelection() const
{
    Ps2::ProvisioningSelection selection;
    selection.installOpl = installOplCheck->isChecked();
    selection.configureOplPlugAndPlay = configureOplCheck->isChecked();
    selection.installHddIgrReturn = installHddIgrCheck->isChecked();
    selection.installWlaunchElf = installWleCheck->isChecked();
    selection.installMemoryCardAnnihilator =
            installMcaCheck->isChecked();
    selection.installFceumm =
            installFceummCheck->isChecked();
    selection.installFhdb = installFhdbCheck->isChecked();
    selection.installHddBootEnabler = installEnablerCheck->isChecked();
    return selection;
}

QString Ps2HddSetupDialog::provisioningSummary() const
{
    QStringList selected;
    if (installOplCheck->isChecked()) selected << "latest OPL Beta";
    if (configureOplCheck->isChecked()) selected << "plug-and-play OPL configuration";
    if (installHddIgrCheck->isChecked()) selected << "HDD IGR return to OPL";
    if (installWleCheck->isChecked()) selected << "wLaunchELF_ISR";
    if (installMcaCheck->isChecked())
        selected << "Memory Card Annihilator";
    if (installFceummCheck->isChecked())
        selected << "FCEUmm-PS2 SMB + ROMS/NES";
    if (installEnablerCheck->isChecked()) selected << "FHDB HDD Boot Configuration";
    if (installFhdbCheck->isChecked()) selected << "FreeHDBoot 1.966";
    if (createFreeDvdBootCheck->isChecked())
        selected << "FreeDVDBoot ISO (" + freeDvdBootProfile->currentData().toString() + ")";
    return selected.isEmpty() ? "none (format only)" : selected.join(", ");
}

void Ps2HddSetupDialog::evaluateSafety(Candidate &candidate)
{
    const Ps2::PhysicalDiskCandidate &disk = candidate.disk;
    if (!disk.inspectionError.empty()) candidate.safetyMessage = QString::fromStdString(disk.inspectionError);
    else if (disk.size < Ps2::HddLayoutPlanner::MinimumDiskSizeBytes) candidate.safetyMessage = "Disk is smaller than 8 GiB.";
    else if (disk.logicalSectorSize != Ps2::HddLayoutPlanner::SectorSize) candidate.safetyMessage = "PS2 APA requires 512-byte logical sectors.";
    else if ((disk.size % Ps2::HddLayoutPlanner::SectorSize) != 0) candidate.safetyMessage = "Capacity is not sector-aligned.";
    else if (disk.busType == "Virtual" || disk.busType == "File-backed virtual" || disk.busType == "Storage Spaces") candidate.safetyMessage = "Virtual and Storage Spaces disks are refused.";
    else if (disk.readOnly) candidate.safetyMessage = "Disk is write-protected.";
    else if (disk.containsSystemVolume) candidate.safetyMessage = "REFUSED: running operating-system disk.";
    else if (disk.containsBootPartition) candidate.safetyMessage = "REFUSED: active/EFI system partition.";
    else if (!disk.systemDiskCheckAvailable) candidate.safetyMessage = "REFUSED: system-disk check unavailable.";
    else {
        candidate.eligible = true;
        candidate.safetyMessage = disk.partitionCount == 0
                ? "Eligible for the guarded Fedora formatter."
                : QString("Eligible. %1 existing host partition(s) will be unmounted and erased automatically after the final ERASE confirmation.")
                        .arg(disk.partitionCount);
    }
}

QString Ps2HddSetupDialog::apaSummary(const Candidate &candidate)
{
    int valid = 0, damaged = 0;
    for (const Ps2::ApaBankProbe &probe : candidate.bankProbes) {
        if (probe.header.state == Ps2::ApaHeaderState::Valid) valid++;
        else if (probe.header.state == Ps2::ApaHeaderState::BadChecksum ||
                 probe.header.state == Ps2::ApaHeaderState::InvalidMbr) damaged++;
    }
    if (!candidate.probeError.isEmpty()) return "Probe failed";
    if (damaged != 0) return QString("%1 valid, %2 suspect").arg(valid).arg(damaged);
    return valid == 0 ? "None" : QString::number(valid) + " valid";
}

void Ps2HddSetupDialog::scanDisks()
{
    setCursor(Qt::WaitCursor);
    diskList->clear(); candidates.clear();
    detailsLabel->setText("Scanning physical disks and checking APA bank boundaries...");
    statusLabel->setText("Read-only scan in progress...");
    try {
        const std::vector<Ps2::PhysicalDiskCandidate> disks = Ps2::PhysicalDiskScanner::Scan();
        candidates.reserve(disks.size());
        for (const Ps2::PhysicalDiskCandidate &disk : disks) {
            Candidate candidate; candidate.disk = disk; evaluateSafety(candidate);
            try {
                candidate.bankProbes = Ps2::Apa::ProbePhysicalDrive(disk.devicePath, disk.size,
                        Ps2::HddLayoutPlanner::MaximumBankCount);
            } catch (const std::exception &error) { candidate.probeError = QString::fromLocal8Bit(error.what()); }
            candidates.push_back(std::move(candidate));
        }
        refreshRows();
        detailsLabel->setText(candidates.empty() ?
                "No supported physical disks were found. On Linux, raw APA inspection also requires root or suitable udev permissions." :
                "Select a disk to inspect its proposed PS2 layout.");
        statusLabel->setText("Read-only scan complete. No disk was modified.");
    } catch (const std::string &error) {
        detailsLabel->setText(QString::fromStdString(error));
        statusLabel->setText("Disk scan failed. No disk was modified.");
        QMessageBox::critical(this, "PS2 disk scan failed", QString::fromStdString(error));
    } catch (const std::exception &error) {
        detailsLabel->setText(QString::fromLocal8Bit(error.what()));
        statusLabel->setText("Disk scan failed. No disk was modified.");
        QMessageBox::critical(this, "PS2 disk scan failed", QString::fromLocal8Bit(error.what()));
    }
    unsetCursor(); updateWriteButton();
}

void Ps2HddSetupDialog::refreshRows()
{
    diskList->clear();
    for (std::size_t index = 0; index < candidates.size(); index++) {
        const Candidate &candidate = candidates[index];
        const Ps2::HddLayout plan = Ps2::HddLayoutPlanner::Plan(candidate.disk.size, selectedMode());
        auto *item = new QTreeWidgetItem(diskList);
        item->setData(0, Qt::UserRole, static_cast<qulonglong>(index));
        item->setText(0, QString::fromStdString(candidate.disk.devicePath));
        item->setText(1, QString::fromStdString(candidate.disk.model));
        item->setText(2, candidate.disk.serialNumber.empty() ? "--" : QString::fromStdString(candidate.disk.serialNumber));
        item->setText(3, formatBytes(candidate.disk.size));
        item->setText(4, QString::fromStdString(candidate.disk.busType));
        item->setText(5, QString("%1 / %2").arg(candidate.disk.logicalSectorSize).arg(candidate.disk.physicalSectorSize));
        item->setText(6, apaSummary(candidate));
        item->setText(7, plan.valid ? candidate.safetyMessage : QString::fromStdString(plan.message));
        const QColor color = candidate.eligible && plan.valid ? QColor(0, 100, 0) : QColor(150, 0, 0);
        for (int column = 0; column < diskList->columnCount(); column++) item->setForeground(column, color);
    }
}

void Ps2HddSetupDialog::refreshBackendState()
{
#ifdef __linux__
    const QString root = QString::fromLocal8Bit(PS2_HDD_PROJECT_ROOT);
    pfsshellPath = qEnvironmentVariable("PS2_HDD_PFSSHELL");
    if (pfsshellPath.isEmpty()) pfsshellPath = root + "/tools/pfsshell/bin/pfsshell";
    payloadPath = root + "/payload/runtime";
    fetchPayloadsPath = root + "/fetch_payloads.sh";
    freeDvdBootScriptPath = root + "/create_freedvdboot_iso.sh";
    writerPath = QCoreApplication::applicationDirPath() + "/PS2-HDD-Writer";
    const bool pfsshellReady = QFileInfo(pfsshellPath).isExecutable();
    const bool writerReady = QFileInfo(writerPath).isExecutable();
    const bool fetchReady = QFileInfo(fetchPayloadsPath).isExecutable();
    const bool isoReady = QFileInfo(freeDvdBootScriptPath).isExecutable();
    backendReady = pfsshellReady && writerReady && fetchReady && isoReady;
    if (backendReady) {
        backendLabel->setText("Fast formatter backend READY — current optional payloads will be fetched on demand.");
        backendLabel->setStyleSheet("font-weight: 600; color: #006400;");
    } else {
        QStringList missing;
        if (!pfsshellReady) missing << "pfsshell";
        if (!writerReady) missing << "privileged writer";
        if (!fetchReady) missing << "payload fetcher";
        if (!isoReady) missing << "FreeDVDBoot ISO creator";
        backendLabel->setText("Formatter backend NOT READY — missing: " + missing.join(", ") +
                ". Run ./prepare_fedora_test.sh, then rebuild/refresh.");
        backendLabel->setStyleSheet("font-weight: 600; color: #8a4b00;");
    }
#else
    backendReady = false;
    backendLabel->setText("Physical formatting is currently enabled only on Fedora/Linux.");
#endif
    updateWriteButton();
}

void Ps2HddSetupDialog::updateWriteButton()
{
    bool enabled = false;
#ifdef __linux__
    const QList<QTreeWidgetItem*> selected = diskList->selectedItems();
    if (backendReady && selected.size() == 1) {
        const std::size_t index = static_cast<std::size_t>(selected.first()->data(0, Qt::UserRole).toULongLong());
        if (index < candidates.size()) {
            const Candidate &candidate = candidates[index];
            const Ps2::HddLayout plan = Ps2::HddLayoutPlanner::Plan(candidate.disk.size, selectedMode());
            const bool standardFits = selectedMode() != Ps2::HddLayoutMode::StandardApa ||
                    candidate.disk.size / Ps2::HddLayoutPlanner::SectorSize <= Ps2::HddLayoutPlanner::MaximumApaSectorCount;
            enabled = candidate.eligible && plan.valid && standardFits;
        }
    }
#endif
    writeButton->setEnabled(enabled);
}

bool Ps2HddSetupDialog::runProcessWithStatus(const QString &program, const QStringList &arguments,
        const QString &initialStatus, QString *combinedOutput)
{
    statusLabel->setText(initialStatus);
    QApplication::processEvents();
    QProcess process(this);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(10000)) {
        if (combinedOutput) *combinedOutput = process.errorString();
        return false;
    }
    QString output;
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(100);
        const QString chunk = QString::fromLocal8Bit(process.readAll());
        output += chunk;
        const QStringList lines = chunk.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines)
            if (line.startsWith("STAGE:") || line.startsWith("==>")) statusLabel->setText(line);
        QApplication::processEvents();
        process.waitForFinished(25);
    }
    output += QString::fromLocal8Bit(process.readAll());
    if (combinedOutput) *combinedOutput = output;
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool Ps2HddSetupDialog::runWriterWithStatus(const QStringList &arguments,
        const QString &initialStatus, QString *combinedOutput)
{
#ifdef __linux__
    if (!privilegedSession || !privilegedSession->isUnlocked()) {
        if (combinedOutput) *combinedOutput = "PS2 HDD access is locked. Close this dialog and use Unlock once.";
        return false;
    }
    statusLabel->setText(initialStatus);
    QApplication::processEvents();
    QString lineWindow;
    return privilegedSession->run(arguments, combinedOutput, [this, &lineWindow](const QString &chunk) {
        lineWindow += chunk;
        if (lineWindow.size() > 4096) lineWindow = lineWindow.right(4096);
        const QStringList lines = chunk.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            if (line.startsWith("STAGE:")) statusLabel->setText(line.mid(6).trimmed());
            else if (!line.trimmed().isEmpty()) statusLabel->setText(line.trimmed());
        }
        QApplication::processEvents();
    });
#else
    Q_UNUSED(arguments); Q_UNUSED(initialStatus); Q_UNUSED(combinedOutput);
    return false;
#endif
}

void Ps2HddSetupDialog::formatSelectedDisk()
{
#ifndef __linux__
    QMessageBox::information(this, "Formatter unavailable", "Physical formatting is currently enabled only on Fedora/Linux.");
    return;
#else
    const QList<QTreeWidgetItem*> selected = diskList->selectedItems();
    if (selected.size() != 1 || !backendReady) return;
    const std::size_t index = static_cast<std::size_t>(selected.first()->data(0, Qt::UserRole).toULongLong());
    if (index >= candidates.size()) return;
    const Candidate &candidate = candidates[index];
    const Ps2::HddLayout plan = Ps2::HddLayoutPlanner::Plan(candidate.disk.size, selectedMode());
    if (!candidate.eligible || !plan.valid) return;
    const bool extended = selectedMode() == Ps2::HddLayoutMode::ExtendedApaBanks;
    if (!extended && candidate.disk.size / Ps2::HddLayoutPlanner::SectorSize > Ps2::HddLayoutPlanner::MaximumApaSectorCount) return;

    const Ps2::ProvisioningSelection provision = provisioningSelection();
    const QString device = QString::fromStdString(candidate.disk.devicePath);
    const QString layoutDescription = extended
            ? QString("Extended APA Banks: Bank 0 is the conventional boot/system/PFS/FHDB bank; "
                      "Bank 1+ are independent games-only APA banks. Every bank is verified after creation.")
            : QString("Standard Sony/PS2 APA/PFS: one conventional Bank 0 with the four system PFS partitions.");
    QString warning = QString(
            "THIS ERASES THE ENTIRE DISK.\n\nTarget: %1\nModel: %2\nCapacity: %3 (%4 bytes)\n\n"
            "%5\n\nSelected optional setup: %6")
            .arg(device).arg(QString::fromStdString(candidate.disk.model))
            .arg(formatBytes(candidate.disk.size)).arg(QString::number(candidate.disk.size))
            .arg(layoutDescription).arg(provisioningSummary());

    warning +=
            QString(
                "\n\nOPL / Apps / ART / ROMS PFS reserve: %1 MiB (%2 GiB).")
                .arg(appsStorageSize->currentData().toUInt())
                .arg(
                    appsStorageSize->currentData().toDouble() /
                            1024.0,
                    0,
                    'f',
                    1);
    if (provision.installFhdb)
        warning += "\n\nFHDB WARNING: the HDD files/bootstrap will be installed, but the console EEPROM HDD-boot setting must still be enabled once. Use the included Status / Enable / Disable / Verify utility from OPL/FMCB.";
    if (createFreeDvdBootCheck->isChecked())
        warning += "\n\nA FreeDVDBoot ISO will also be created in ~/Downloads after the HDD verifies. The Slim profile boots the enabler directly; phat custom-disc profiles are experimental/manual-launch. Compatibility depends on the PS2 DVD Player version.";

    if (QMessageBox::warning(this, "ERASE disk and create PS2 HDD?", warning,
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;

    bool accepted = false;
    const QString phrase = "ERASE " + device;
    const QString typed = QInputDialog::getText(this, "Final destructive confirmation",
            "Type exactly:\n" + phrase, QLineEdit::Normal, QString(), &accepted);
    if (!accepted || typed != phrase) {
        if (accepted) QMessageBox::warning(this, "Confirmation did not match", "Disk was not modified.");
        return;
    }

    refreshButton->setEnabled(false); writeButton->setEnabled(false);

    QStringList fetchArguments;
    if (provision.installOpl && !extended) fetchArguments << "--opl";
    if (provision.installWlaunchElf) fetchArguments << "--wle";
    if (provision.installMemoryCardAnnihilator)
        fetchArguments << "--mca";
    if (provision.installFceumm)
        fetchArguments << "--fceumm";
    if (provision.installFhdb) fetchArguments << "--fhdb";
    if (provision.installHddBootEnabler) fetchArguments << "--hdd-enabler";
    if (createFreeDvdBootCheck->isChecked()) fetchArguments << "--freedvdboot";

    QString output;
    if (!fetchArguments.isEmpty()) {
        if (!runProcessWithStatus(fetchPayloadsPath, fetchArguments,
                "Downloading/verifying selected current PS2 payloads (disk is not being written yet)...", &output)) {
            refreshButton->setEnabled(true); refreshBackendState();
            statusLabel->setText("Payload download failed. The physical writer was not started.");
            QMessageBox::critical(this, "Optional payload preparation failed",
                    "The disk was not touched.\n\n" + output.right(8000));
            return;
        }
    }

    if (extended && provision.installOpl) {
        const QString root = QString::fromLocal8Bit(PS2_HDD_PROJECT_ROOT);
        const QString source = root + "/payload/banked-opl/OPNPS2LD.ELF";
        const QString destination = payloadPath + "/opl/OPNPS2LD.ELF";
        if (!QFileInfo(source).isFile()) {
            refreshButton->setEnabled(true); refreshBackendState();
            QMessageBox::critical(this, "Bank-aware OPL missing", "Bundled Extended APA OPL ELF is missing. Disk was not modified.");
            return;
        }
        QDir().mkpath(QFileInfo(destination).absolutePath());
        QFile::remove(destination);
        if (!QFile::copy(source, destination)) {
            refreshButton->setEnabled(true); refreshBackendState();
            QMessageBox::critical(this, "Bank-aware OPL staging failed", "Could not stage the bundled Extended APA OPL ELF. Disk was not modified.");
            return;
        }
    }

    QStringList arguments;
    arguments << "--device" << device
              << "--expected-size" << QString::number(candidate.disk.size)
              << "--pfsshell" << pfsshellPath
              << "--apps-size-mib"
              << QString::number(
                    appsStorageSize->currentData().toUInt());
    if (extended) arguments << "--extended-banks";
    if (provision.any()) {
        arguments << "--payload-dir" << payloadPath;
        if (provision.installOpl) arguments << "--install-opl";
        if (provision.configureOplPlugAndPlay) arguments << "--configure-opl";
        if (provision.installHddIgrReturn) arguments << "--install-hdd-igr";
        if (provision.installWlaunchElf) arguments << "--install-wle";
        if (provision.installMemoryCardAnnihilator)
            arguments << "--install-mca";
        if (provision.installFceumm)
            arguments << "--install-fceumm";
        if (provision.installFhdb) arguments << "--install-fhdb";
        if (provision.installHddBootEnabler) arguments << "--install-hdd-enabler";
    }

    for (;;) {
        output.clear();
        const bool writerOk = runWriterWithStatus(arguments,
                extended ? "Formatting Bank 0 + games-only upper APA banks. Do not disconnect the disk..." :
                           "Fast-formatting standard APA/PFS. Do not disconnect the disk...", &output);
        if (writerOk) break;

        if (output.contains("TARGET_BUSY:")) {
            const QString busy = output.mid(output.lastIndexOf("TARGET_BUSY:")).right(5000);
            const auto choice = QMessageBox::warning(this, "Target drive is still in use",
                    busy + "\n\nClose the listed application/window, then click Retry. "
                           "No lazy/forced unmount is used.",
                    QMessageBox::Retry | QMessageBox::Cancel,
                    QMessageBox::Retry);
            if (choice == QMessageBox::Retry)
                continue;

            refreshButton->setEnabled(true);
            refreshBackendState();
            statusLabel->setText("Formatting cancelled while waiting for the target drive to become idle.");
            scanDisks();
            return;
        }

        refreshButton->setEnabled(true);
        refreshBackendState();
        statusLabel->setText("Formatter failed. Review the writer output before doing anything else.");
        QMessageBox::critical(this, "PS2 HDD formatting failed",
                "The writer stopped and reported an error:\n\n" + output.right(9000));
        scanDisks();
        return;
    }

    QString isoPath;
    if (createFreeDvdBootCheck->isChecked()) {
        const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        isoPath = QDir::homePath() + "/Downloads/PS2-FHDB-Boot-Enabler-FreeDVDBoot-" +
                freeDvdBootProfile->currentData().toString() + "-" + stamp + ".iso";
        QString isoOutput;
        const QStringList isoArgs = { "--profile", freeDvdBootProfile->currentData().toString(),
                "--output", isoPath, "--payload-dir", payloadPath };
        if (!runProcessWithStatus(freeDvdBootScriptPath, isoArgs,
                "HDD verified. Creating optional FreeDVDBoot ISO...", &isoOutput)) {
            refreshButton->setEnabled(true); refreshBackendState();
            statusLabel->setText("HDD succeeded; optional FreeDVDBoot ISO creation failed.");
            QMessageBox::warning(this, "HDD ready; ISO creation failed",
                    "The HDD format/provisioning completed successfully. Only the optional ISO failed:\n\n" +
                    isoOutput.right(8000));
            scanDisks();
            return;
        }
    }

    refreshButton->setEnabled(true); refreshBackendState();
    statusLabel->setText(extended ? "Extended APA banks and selected setup completed and verified."
                                  : "Standard APA/PFS format and selected setup completed and verified.");
    QString done = extended
            ? "Bank 0 and every games-only upper APA bank verified successfully. Bank 0 contains the normal PS2 system/PFS/FHDB layout."
            : "The standard APA chain and all four system PFS partitions verified successfully.";
    if (provision.installOpl)
        done += extended
                ? "\n\nThe Extended APA-aware OPL build was installed and PP.FHDB.APPS was configured as OPL's Bank-0 data/app partition."
                : "\n\nLatest OPL Beta was installed and PP.FHDB.APPS was configured as OPL's HDD data/app partition.";
    if (provision.configureOplPlugAndPlay)
        done += "\nOPL was preconfigured for automatic Internal HDD startup, HDD games as the default device, artwork, write operations, caching and auto-refresh/sort.";
    if (provision.installHddIgrReturn)
        done += "\nHDD IGR return was preinstalled as /OPL/IGR.ELF and OPL exit_path was pointed to the managed HDD copy.";
    if (provision.installWlaunchElf) done += "\nLatest normal wLaunchELF_ISR was installed.";
    if (provision.installMemoryCardAnnihilator) done += "\nMemory Card Annihilator was installed in OPL Apps.";
    if (provision.installHddBootEnabler) done += "\nThe dedicated FHDB HDD Boot Configuration utility was installed in OPL Apps.";
    if (provision.installFhdb) done += "\nFreeHDBoot 1.966 and its MBR bootstrap were installed. The PS2 EEPROM HDD-boot setting still needs to be enabled once per console.";
    if (!isoPath.isEmpty()) done += "\n\nFreeDVDBoot ISO created:\n" + isoPath;
    QMessageBox::information(this, "PS2 HDD setup verified", done);
    scanDisks();
#endif
}

QString Ps2HddSetupDialog::candidateDetails(const Candidate &candidate) const
{
    const Ps2::HddLayout plan = Ps2::HddLayoutPlanner::Plan(candidate.disk.size, selectedMode());
    QString details = QString(
            "%1 | %2 | Serial: %3\nCapacity: %4 (%5 bytes) | Bus: %6 | Logical/physical sector: %7/%8 bytes\n"
            "Host layout: %9, %10 partition(s) | Existing APA: %11\nSafety: %12")
            .arg(QString::fromStdString(candidate.disk.devicePath))
            .arg(QString::fromStdString(candidate.disk.model))
            .arg(candidate.disk.serialNumber.empty() ? "not reported" : QString::fromStdString(candidate.disk.serialNumber))
            .arg(formatBytes(candidate.disk.size)).arg(QString::number(candidate.disk.size))
            .arg(QString::fromStdString(candidate.disk.busType))
            .arg(candidate.disk.logicalSectorSize).arg(candidate.disk.physicalSectorSize)
            .arg(candidate.disk.rawPartitionStyle ? "RAW/console layout" : "MBR/GPT/other")
            .arg(candidate.disk.partitionCount).arg(apaSummary(candidate)).arg(candidate.safetyMessage);
    if (!candidate.probeError.isEmpty()) details += "\nAPA probe error: " + candidate.probeError;
    if (plan.valid) {
        details += "\n\nPlanned layout:";
        for (const Ps2::ApaBankLayout &bank : plan.banks) {
            const QString role = bank.role == Ps2::ApaBankRole::BootSystemAndGames ?
                    "standard APA system + games" : "games-only APA bank";
            details += QString("\n  Bank %1: physical LBA 0x%2, %3 addressable — %4")
                    .arg(bank.index).arg(QString::number(bank.baseSector, 16).toUpper())
                    .arg(formatBytes(bank.addressableSectorCount * Ps2::HddLayoutPlanner::SectorSize)).arg(role);
        }
        if (selectedMode() == Ps2::HddLayoutMode::StandardApa) {
            details += "\n  Fast write: canonical APA + verified PFS system partitions.";
            details += "\n  Optional setup: " + provisioningSummary() + ".";
        } else details += "\n  Extended mode creates these banks; OPL/PFS/FHDB remain in Bank 0.";
        if (plan.unaddressedSectorCount != 0)
            details += "\n  Outside this layout: " + formatBytes(plan.unaddressedSectorCount * Ps2::HddLayoutPlanner::SectorSize) + ".";
        details += "\n  " + QString::fromStdString(plan.message);
    } else details += "\n\nLayout cannot be created: " + QString::fromStdString(plan.message);

    for (const Ps2::ApaBankProbe &probe : candidate.bankProbes) {
        if (probe.header.state == Ps2::ApaHeaderState::NotPresent) continue;
        details += QString("\nExisting Bank %1 @ 0x%2: %3").arg(probe.index)
                .arg(QString::number(probe.baseSector, 16).toUpper())
                .arg(QString::fromStdString(probe.header.message));
        if (probe.header.state == Ps2::ApaHeaderState::Valid)
            details += QString(" MBR v%1, OSD payload %2 sector(s) @ 0x%3.")
                    .arg(probe.header.mbrVersion).arg(probe.header.osdSize)
                    .arg(QString::number(probe.header.osdStart, 16).toUpper());
    }
    return details;
}

void Ps2HddSetupDialog::updateSelection()
{
    const QList<QTreeWidgetItem*> selected = diskList->selectedItems();
    if (selected.isEmpty()) { detailsLabel->setText("Select a disk to inspect its proposed PS2 layout."); updateWriteButton(); return; }
    const std::size_t index = static_cast<std::size_t>(selected.first()->data(0, Qt::UserRole).toULongLong());
    if (index < candidates.size()) detailsLabel->setText(candidateDetails(candidates[index]));
    updateWriteButton();
}
