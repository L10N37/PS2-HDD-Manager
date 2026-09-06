#include "MainWindow.h"

#include "FhdbConfigDialog.h"
#include "Ps2HddSetupDialog.h"
#include "PrivilegedSession.h"
#include "ArtworkProvider.h"
#include "core/Ps2HddLayout.h"
#include "core/OplConfig.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QCheckBox>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPainter>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QSet>
#include <QSplitter>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QTemporaryFile>
#include <QTimer>
#include <QTextStream>
#include <QTreeView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>

#ifdef __linux__
#include <unistd.h>
#endif

namespace
{
constexpr int KindRole = Qt::UserRole;
constexpr int PathRole = Qt::UserRole + 1;
constexpr int LoadedRole = Qt::UserRole + 2;
constexpr int PartitionRole = Qt::UserRole + 3;

QStringList localUrls(const QMimeData *mime)
{
    QStringList result;
    if (!mime || !mime->hasUrls())
        return result;
    for (const QUrl &url : mime->urls())
        if (url.isLocalFile())
            result << url.toLocalFile();
    return result;
}

bool isDiscImagePath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "iso" || suffix == "bin" || suffix == "gi" || suffix == "iml" ||
            suffix == "nrg" || suffix == "zso";
}

QString cleanMachineField(QString value)
{
    value.replace('\t', ' ');
    value.replace('\r', ' ');
    value.replace('\n', ' ');
    return value.trimmed();
}

QString normalizedLocalPath(const QString &path)
{
    QFileInfo info(path);
    QString key = info.canonicalFilePath();
    if (key.isEmpty()) key = info.absoluteFilePath();
    return QDir::cleanPath(key);
}

class MarkedFileDelegate final : public QStyledItemDelegate
{
public:
    MarkedFileDelegate(QFileSystemModel *model, const QSet<QString> *marked, QObject *parent)
        : QStyledItemDelegate(parent), model(model), marked(marked) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem adjusted(option);
        if (model && marked) {
            const QModelIndex rowIndex = index.sibling(index.row(), 0);
            const QString path = normalizedLocalPath(model->filePath(rowIndex));
            if (marked->contains(path)) {
                adjusted.backgroundBrush = QColor(150, 20, 20, 175);
                adjusted.palette.setColor(QPalette::Text, Qt::white);
                adjusted.palette.setColor(QPalette::HighlightedText, Qt::white);
            }
        }
        QStyledItemDelegate::paint(painter, adjusted, index);
    }

private:
    QFileSystemModel *model;
    const QSet<QString> *marked;
};
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
#ifdef __linux__
    const QString root = QString::fromLocal8Bit(PS2_HDD_PROJECT_ROOT);
    writerPath = QCoreApplication::applicationDirPath() + "/PS2-HDD-Writer";
    hdlDumpPath = root + "/tools/hdl-dump/bin/hdl_dump";
    fetchPayloadsPath = root + "/fetch_payloads.sh";
    runtimePayloadPath = root + "/payload/runtime";
    pfsshellPath = qEnvironmentVariable("PS2_HDD_PFSSHELL");
    if (pfsshellPath.isEmpty())
        pfsshellPath = root + "/tools/pfsshell/bin/pfsshell";
#endif
    privilegedSession = new PrivilegedSession(writerPath, this);
    network = new QNetworkAccessManager(this);
    buildUi();
    populatePcDrives();

    QSettings settings("VajskiDs", "PS2-HDD-Manager");
    const QString lastPath = settings.value("pc/lastPath", QDir::homePath()).toString();
    navigatePc(QFileInfo(lastPath).isDir() ? lastPath : QDir::homePath());
    refreshDisks();

    // Ask once after the main window is visible. KDE/Polkit then owns one
    // authentication dialog for the whole application session rather than
    // spawning a prompt for each game/PFS operation.
    QTimer::singleShot(0, this, [this]() {
        if (unlockHddSession(false))
            refreshCurrentPs2Tree();
        else
            statusBar()->showMessage("PS2 HDD access remains locked. Use Unlock once when you are ready.");
    });
}

QString MainWindow::formatBytes(std::uint64_t bytes)
{
    static const char *suffixes[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double value = static_cast<double>(bytes);
    int suffix = 0;
    while (value >= 1024.0 && suffix < 4) { value /= 1024.0; suffix++; }
    return suffix == 0 ? QString::number(bytes) + " B" :
            QString::number(value, 'f', 2) + " " + suffixes[suffix];
}

QString MainWindow::joinPfsPath(const QString &base, const QString &name)
{
    QString result = base;
    if (result.isEmpty()) result = "/";
    if (!result.endsWith('/')) result += '/';
    result += name;
    result.replace(QRegularExpression("/{2,}"), "/");
    return result;
}

QString MainWindow::pfsParentPath(const QString &path)
{
    if (path.isEmpty() || path == "/") return "/";
    const int slash = path.lastIndexOf('/');
    return slash <= 0 ? "/" : path.left(slash);
}

bool MainWindow::unlockHddSession(bool showFailure)
{
#ifdef __linux__
    if (!privilegedSession) return false;
    if (privilegedSession->isUnlocked()) return true;
    statusBar()->showMessage("Waiting for one-time Linux authentication...");
    const bool ok = privilegedSession->unlock(this);
    updateUnlockUi();
    if (!ok) {
        const QString message = privilegedSession->lastError();
        statusBar()->showMessage("PS2 HDD access is locked.");
        if (showFailure && !message.isEmpty())
            QMessageBox::warning(this, "PS2 HDD access remains locked", message +
                    "\n\nNo disk operation was started. Click Unlock once to try again.");
        return false;
    }
    statusBar()->showMessage("PS2 HDD access unlocked for this application session.");
    return true;
#else
    Q_UNUSED(showFailure);
    return false;
#endif
}

void MainWindow::updateUnlockUi()
{
    const bool unlocked = privilegedSession && privilegedSession->isUnlocked();
    if (unlockLabel) {
        unlockLabel->setText(unlocked ? "🔓 HDD access unlocked for session" : "🔒 HDD access locked");
        unlockLabel->setStyleSheet(unlocked ? "font-weight: 600; color: #2e7d32;" : "font-weight: 600;");
    }
    if (unlockButton) {
        unlockButton->setText(unlocked ? "Unlocked" : "Unlock once");
        unlockButton->setEnabled(!unlocked);
    }
}

void MainWindow::buildUi()
{
    setWindowTitle(QString("PS2 HDD Manager %1").arg(PS2_HDD_APP_VERSION));
    resize(1420, 820);
    setMinimumSize(1040, 650);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    auto *deviceBar = new QHBoxLayout();
    deviceBar->addWidget(new QLabel("PS2 physical disk:", central));
    diskCombo = new QComboBox(central);
    diskCombo->setMinimumWidth(420);
    deviceBar->addWidget(diskCombo, 1);
    unlockLabel = new QLabel("🔒 HDD access locked", central);
    unlockButton = new QPushButton("Unlock once", central);
    unlockButton->setToolTip("Authenticate once with Polkit. The restricted PS2-HDD-Writer helper remains available only until this application closes.");
    auto *refresh = new QPushButton("Reload Disks", central);
    auto *setup = new QPushButton("PS2 HDD Setup...", central);
    auto *menu = new QPushButton("Configure FHDB Menu...", central);
    deviceBar->addWidget(unlockLabel);
    deviceBar->addWidget(unlockButton);
    deviceBar->addWidget(refresh);
    deviceBar->addWidget(setup);
    deviceBar->addWidget(menu);
    layout->addLayout(deviceBar);

    auto *splitter = new QSplitter(Qt::Horizontal, central);
    splitter->addWidget(buildPcPane());
    splitter->addWidget(buildPs2Pane());
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);

    auto *commandBar = new QHBoxLayout();
    commandBar->addStretch();
    copyToPs2Button = new QPushButton("F5  Install selected game(s)  →  PS2 HDD", central);
    copyToPs2Button->setShortcut(QKeySequence(QStringLiteral("F5")));
    copyToPs2Button->setToolTip("Installs selected PS2 disc images as normal HDL APA game partitions.");
    commandBar->addWidget(copyToPs2Button);
    commandBar->addStretch();
    layout->addLayout(commandBar);

    transferProgress = new QProgressBar(central);
    transferProgress->setRange(0, 100);
    transferProgress->setValue(0);
    transferProgress->setVisible(false);
    progressDetail = new QLabel(central);
    progressDetail->setVisible(false);
    layout->addWidget(transferProgress);
    layout->addWidget(progressDetail);

    statusLabel = new QLabel(
            "The right pane shows the real installed HDL game table and OPL PFS storage. "
            "Drop games on HDL Games; drop ART/CFG/APPS folders onto OPL Storage.", central);
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);
    setCentralWidget(central);
    statusBar()->showMessage("No disk has been modified.");

    connect(unlockButton, &QPushButton::clicked, this, [this]() {
        if (unlockHddSession(true)) refreshCurrentPs2Tree();
    });
    connect(privilegedSession, &PrivilegedSession::unlockedChanged, this, [this](bool) { updateUnlockUi(); });
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshDisks);
    connect(setup, &QPushButton::clicked, this, [this]() {
        if (!unlockHddSession(true)) return;
        Ps2HddSetupDialog dialog(privilegedSession, this);
        dialog.exec();
        refreshDisks();
    });
    connect(menu, &QPushButton::clicked, this, [this]() {
        FhdbConfigDialog dialog(this);
        dialog.exec();
    });
    connect(diskCombo, &QComboBox::currentIndexChanged, this, &MainWindow::selectDisk);
    connect(copyToPs2Button, &QPushButton::clicked, this, &MainWindow::installSelectedPcGames);
    updateUnlockUi();
}

QWidget *MainWindow::buildPcPane()
{
    auto *group = new QGroupBox("PC", this);
    auto *layout = new QVBoxLayout(group);
    auto *toolbar = new QHBoxLayout();
    pcDriveCombo = new QComboBox(group);
    pcDriveCombo->setMinimumWidth(300);
    auto *up = new QPushButton("Up", group);
    auto *refresh = new QPushButton("Refresh", group);
    toolbar->addWidget(pcDriveCombo, 1);
    toolbar->addWidget(up);
    toolbar->addWidget(refresh);
    layout->addLayout(toolbar);

    pcPath = new QLineEdit(group);
    layout->addWidget(pcPath);
    pcModel = new QFileSystemModel(group);
    pcModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);
    pcModel->setRootPath(QString());
    pcView = new QTreeView(group);
    pcView->setModel(pcModel);
    pcView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    pcView->viewport()->installEventFilter(this);
    pcView->setItemDelegate(new MarkedFileDelegate(pcModel, &markedPcPaths, pcView));
    pcView->setDragEnabled(true);
    pcView->setDragDropMode(QAbstractItemView::DragOnly);
    pcView->setSelectionBehavior(QAbstractItemView::SelectRows);
    pcView->setSortingEnabled(true);
    pcView->sortByColumn(0, Qt::AscendingOrder);
    pcView->setRootIsDecorated(false);
    pcView->setItemsExpandable(false);
    pcView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < 4; i++) pcView->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    layout->addWidget(pcView, 1);
    layout->addWidget(new QLabel(
            "Fedora mount points are listed above. Ctrl/Shift selection works normally; RIGHT-CLICK toggles persistent red transfer marks. "
            "F5 uses red-marked games first, otherwise the normal selection. Drag files/folders to the PS2 pane for PFS copy.", group));

    connect(pcDriveCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) navigatePc(pcDriveCombo->itemData(index).toString());
    });
    connect(pcPath, &QLineEdit::returnPressed, this, [this]() { navigatePc(pcPath->text()); });
    connect(pcView, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        const QString path = pcModel->filePath(index);
        if (QFileInfo(path).isDir()) navigatePc(path);
    });
    connect(up, &QPushButton::clicked, this, [this]() {
        QDir directory(pcPath->text());
        if (directory.cdUp()) navigatePc(directory.absolutePath());
    });
    connect(refresh, &QPushButton::clicked, this, [this]() {
        populatePcDrives(); navigatePc(pcPath->text());
    });
    return group;
}

QWidget *MainWindow::buildPs2Pane()
{
    auto *group = new QGroupBox("PlayStation 2 HDD - live APA / HDL / PFS view", this);
    auto *layout = new QVBoxLayout(group);
    auto *toolbar = new QHBoxLayout();
    auto *refresh = new QPushButton("Refresh HDD", group);
    addArtButton = new QPushButton("Add Art...", group);
    addArtButton->setToolTip("Scan installed HDL Game IDs, download matching OPL artwork into the persistent cache, then install it directly into OPL Storage/ART.");
    installAppsButton = new QPushButton("Install / Update OPL Apps...", group);
    installAppsButton->setToolTip(
            "Downloads only the selected current payloads, reusing the persistent cache, then installs them into the existing OPL PFS storage without formatting the HDD.");
    applyOplDefaultsButton = new QPushButton("Apply Recommended OPL Defaults", group);
    applyOplDefaultsButton->setToolTip(
            "Writes a plug-and-play conf_opl.cfg: Internal HDD auto-start/default, Apps auto, "
            "cover art, write operations, game-list cache, auto-refresh and auto-sort. "
            "IGR exit_path is deliberately left unchanged because direct HDD/PFS IGR return is not safe in stock OPL.");
    toolbar->addWidget(refresh);
    toolbar->addWidget(addArtButton);
    toolbar->addWidget(installAppsButton);
    toolbar->addWidget(applyOplDefaultsButton);
    toolbar->addStretch();
    layout->addLayout(toolbar);
    ps2View = new QTreeWidget(group);
    ps2View->setAcceptDrops(true);
    ps2View->viewport()->setAcceptDrops(true);
    ps2View->viewport()->installEventFilter(this);
    ps2View->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ps2View->setColumnCount(4);
    ps2View->setHeaderLabels({ "Name", "Game ID / Type", "Media", "Size" });
    ps2View->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ps2View->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ps2View->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ps2View->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(ps2View, 1);
    layout->addWidget(new QLabel(
            "HDL Games/CD/DVD are virtual categories; games remain normal APA HDL partitions. "
            "OPL Storage exposes the real PFS files. Drop an ART folder onto OPL Storage to merge it automatically.", group));

    connect(refresh, &QPushButton::clicked, this, [this]() {
        if (unlockHddSession(true)) refreshCurrentPs2Tree();
    });
    connect(addArtButton, &QPushButton::clicked, this, &MainWindow::addArtwork);
    connect(installAppsButton, &QPushButton::clicked, this, &MainWindow::installOrUpdateOplApps);
    connect(applyOplDefaultsButton, &QPushButton::clicked, this, &MainWindow::applyRecommendedOplDefaults);
    connect(ps2View, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (!item || item->data(0, KindRole).toInt() != NodePfsDirectory || item->data(0, LoadedRole).toBool())
            return;
        const int index = diskCombo ? diskCombo->currentIndex() : -1;
        if (index >= 0 && static_cast<std::size_t>(index) < disks.size())
            populatePfsDirectory(item, item->data(0, PathRole).toString(), disks[static_cast<std::size_t>(index)]);
    });
    return group;
}

void MainWindow::populatePcDrives()
{
    const QString current = pcPath ? QDir::fromNativeSeparators(pcPath->text()) : QString();
    pcDriveCombo->blockSignals(true);
    pcDriveCombo->clear();

    QSet<QString> roots;
    const auto volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &volume : volumes) {
        if (!volume.isValid() || !volume.isReady() || volume.rootPath().isEmpty()) continue;
        const QString root = QDir::cleanPath(volume.rootPath());
        if (roots.contains(root)) continue;
        roots.insert(root);
        QString name = volume.displayName().trimmed();
        if (name.isEmpty()) name = volume.name();
        if (name.isEmpty()) name = root;
        QString label = QString("%1 — %2 free / %3 — %4")
                .arg(name)
                .arg(formatBytes(static_cast<std::uint64_t>(volume.bytesAvailable())))
                .arg(formatBytes(static_cast<std::uint64_t>(volume.bytesTotal())))
                .arg(root);
        pcDriveCombo->addItem(label, root);
    }
    if (!roots.contains("/")) pcDriveCombo->addItem("System root — /", "/");

    int best = -1;
    int bestLength = -1;
    for (int i = 0; i < pcDriveCombo->count(); i++) {
        const QString root = pcDriveCombo->itemData(i).toString();
        if (!current.isEmpty() && (current == root || current.startsWith(root.endsWith('/') ? root : root + '/')) && root.size() > bestLength) {
            best = i; bestLength = root.size();
        }
    }
    if (best >= 0) pcDriveCombo->setCurrentIndex(best);
    pcDriveCombo->blockSignals(false);
}

void MainWindow::navigatePc(const QString &path)
{
    const QFileInfo info(QDir::fromNativeSeparators(path));
    if (!info.exists() || !info.isDir()) return;
    const QString absolute = QDir::cleanPath(info.absoluteFilePath());
    pcPath->setText(QDir::toNativeSeparators(absolute));
    pcView->setRootIndex(pcModel->index(absolute));
    QSettings("VajskiDs", "PS2-HDD-Manager").setValue("pc/lastPath", absolute);
}

void MainWindow::refreshDisks()
{
    QString previous;
    const int oldIndex = diskCombo ? diskCombo->currentIndex() : -1;
    if (oldIndex >= 0 && static_cast<std::size_t>(oldIndex) < disks.size())
        previous = QString::fromStdString(disks[static_cast<std::size_t>(oldIndex)].devicePath);

    diskCombo->blockSignals(true);
    diskCombo->clear();
    disks.clear();
    ps2View->clear();
    currentOplPartition.clear();
    currentOplBase.clear();
    statusBar()->showMessage("Scanning physical disks...");
    try {
        disks = Ps2::PhysicalDiskScanner::Scan();
        int restore = -1;
        for (std::size_t i = 0; i < disks.size(); i++) {
            const auto &disk = disks[i];
            diskCombo->addItem(QString("%1 — %2 — %3")
                    .arg(QString::fromStdString(disk.devicePath))
                    .arg(QString::fromStdString(disk.model))
                    .arg(formatBytes(disk.size)));
            if (QString::fromStdString(disk.devicePath) == previous) restore = static_cast<int>(i);
        }
        if (disks.empty()) statusLabel->setText("No supported physical disks were found.");
        statusBar()->showMessage("Disk scan complete. No disk was modified.");
        diskCombo->blockSignals(false);
        if (!disks.empty()) {
            diskCombo->setCurrentIndex(restore >= 0 ? restore : 0);
            selectDisk(diskCombo->currentIndex());
        }
    } catch (const std::exception &e) {
        diskCombo->blockSignals(false);
        statusLabel->setText(QString::fromLocal8Bit(e.what()));
        statusBar()->showMessage("Disk scan failed.");
    }
    if (copyToPs2Button) copyToPs2Button->setEnabled(selectedDiskCanInstallGames());
}

void MainWindow::selectDisk(int index)
{
    resetProgress();
    currentOplPartition.clear();
    currentOplBase.clear();
    if (index < 0 || static_cast<std::size_t>(index) >= disks.size()) {
        ps2View->clear(); return;
    }
    populatePs2Tree(disks[static_cast<std::size_t>(index)]);
    if (copyToPs2Button) copyToPs2Button->setEnabled(selectedDiskCanInstallGames());
}

void MainWindow::refreshCurrentPs2Tree()
{
    const int index = diskCombo ? diskCombo->currentIndex() : -1;
    if (index >= 0 && static_cast<std::size_t>(index) < disks.size())
        populatePs2Tree(disks[static_cast<std::size_t>(index)]);
}

void MainWindow::populatePs2Tree(const Ps2::PhysicalDiskCandidate &disk)
{
    ps2View->clear();
    currentOplPartition.clear();
    currentOplBase.clear();
    auto *root = new QTreeWidgetItem(ps2View);
    root->setText(0, "Internal HDD");
    root->setText(1, QString::fromStdString(disk.devicePath));
    root->setText(3, formatBytes(disk.size));
    root->setData(0, KindRole, NodeHddRoot);

    auto *games = new QTreeWidgetItem(root);
    games->setText(0, "HDL Games");
    games->setText(1, "Real installed game table");
    games->setData(0, KindRole, NodeGamesRoot);
    populateInstalledGames(games, disk);

    auto *opl = new QTreeWidgetItem(root);
    opl->setText(0, "OPL Storage");
    opl->setText(1, "PFS");
    opl->setData(0, KindRole, NodePfsDirectory);
    // @opl asks the privileged backend to map OPL's logical storage root:
    // custom partitions use /OPL, while the canonical +OPL partition uses /.
    opl->setData(0, PathRole, "@opl");
    populatePfsDirectory(opl, "@opl", disk, false);

    root->setExpanded(true);
    games->setExpanded(true);
    opl->setExpanded(true);
    statusLabel->setText("Live PS2 HDD view refreshed. Drag ISO(s) to HDL Games or ART/CFG/APPS files and folders to OPL Storage.");
}

bool MainWindow::runPrivilegedWriter(const Ps2::PhysicalDiskCandidate &disk,
        const QStringList &modeArguments, QString *output, bool parseProgress, const QString &progressPrefix)
{
#ifdef __linux__
    if (!privilegedSession || !privilegedSession->isUnlocked()) {
        if (output) *output = "PS2 HDD access is locked. Authenticate once with the Unlock button.";
        return false;
    }
    QStringList args;
    args << "--device" << QString::fromStdString(disk.devicePath)
         << "--expected-size" << QString::number(disk.size);
    args << modeArguments;

    QString progressWindow;
    return privilegedSession->run(args, output, [this, parseProgress, progressPrefix, &progressWindow](const QString &chunk) {
        if (parseProgress) {
            progressWindow += chunk;
            if (progressWindow.size() > 4096) progressWindow = progressWindow.right(4096);
            parseTransferProgress(progressWindow, progressPrefix);
        }
        const QStringList lines = chunk.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines)
            if (line.startsWith("STAGE:")) statusLabel->setText(line.mid(6).trimmed());
    });
#else
    Q_UNUSED(disk); Q_UNUSED(modeArguments); Q_UNUSED(output); Q_UNUSED(parseProgress); Q_UNUSED(progressPrefix);
    return false;
#endif
}

void MainWindow::populateInstalledGames(QTreeWidgetItem *gamesRoot, const Ps2::PhysicalDiskCandidate &disk)
{
    auto *dvd = new QTreeWidgetItem(gamesRoot);
    dvd->setText(0, "DVD"); dvd->setData(0, KindRole, NodeGamesMedia); dvd->setData(0, PathRole, "dvd");
    auto *cd = new QTreeWidgetItem(gamesRoot);
    cd->setText(0, "CD"); cd->setData(0, KindRole, NodeGamesMedia); cd->setData(0, PathRole, "cd");

    QString reason;
    if (!selectedDiskCanInstallGames(&reason)) {
        auto *note = new QTreeWidgetItem(gamesRoot);
        note->setText(0, "(game table unavailable)"); note->setText(1, reason);
        return;
    }

    QString output;
    if (!runPrivilegedWriter(disk, { "--hdl-dump", hdlDumpPath, "--list-games" }, &output)) {
        auto *note = new QTreeWidgetItem(gamesRoot);
        note->setText(0, "(could not read HDL game table)");
        note->setText(1, output.right(160));
        return;
    }

    int count = 0;
    for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
        if (!line.startsWith("GAME\t")) continue;
        const QStringList f = line.split('\t');
        if (f.size() < 5) continue;
        const QString media = cleanMachineField(f[1]);
        bool ok = false;
        const qulonglong kb = cleanMachineField(f[2]).toULongLong(&ok);
        const QString startup = cleanMachineField(f[3]);
        const QString name = cleanMachineField(f.mid(4).join(" "));
        QTreeWidgetItem *parent = media == "CD" ? cd : dvd;
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, name);
        item->setText(1, startup);
        item->setText(2, media);
        item->setText(3, ok ? formatBytes(static_cast<std::uint64_t>(kb) * 1024ULL) : QString());
        item->setData(0, KindRole, NodeGame);
        count++;
    }
    gamesRoot->setText(3, QString("%1 game(s)").arg(count));
    dvd->setExpanded(true); cd->setExpanded(true);
}

void MainWindow::populatePfsDirectory(QTreeWidgetItem *item, const QString &path,
        const Ps2::PhysicalDiskCandidate &disk, bool showErrors)
{
    if (!item) return;
    QString reason;
    if (!selectedDiskCanBrowsePfs(&reason)) {
        while (item->childCount() > 0) delete item->takeChild(0);
        auto *note = new QTreeWidgetItem(item); note->setText(0, "(PFS unavailable)"); note->setText(1, reason);
        item->setData(0, LoadedRole, true);
        return;
    }

    QString output;
    if (!runPrivilegedWriter(disk,
            { "--pfsshell", pfsshellPath, "--list-pfs", "--partition", "auto", "--pfs-path", path }, &output)) {
        while (item->childCount() > 0) delete item->takeChild(0);
        auto *note = new QTreeWidgetItem(item);
        note->setText(0, "(OPL PFS not available)");
        note->setText(1, output.right(180));
        item->setData(0, LoadedRole, true);
        if (showErrors) QMessageBox::warning(this, "Could not read OPL storage", output.right(5000));
        return;
    }

    while (item->childCount() > 0) delete item->takeChild(0);
    QString actualPath = path;
    for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
        if (line.startsWith("OPL_PARTITION\t")) {
            currentOplPartition = cleanMachineField(line.section('\t', 1));
            item->setText(1, currentOplPartition + ": PFS");
            item->setData(0, PartitionRole, currentOplPartition);
            continue;
        }
        if (line.startsWith("OPL_BASE\t")) {
            currentOplBase = cleanMachineField(line.section('\t', 1));
            actualPath = currentOplBase;
            item->setData(0, PathRole, actualPath);
            continue;
        }
        if (!line.startsWith("PFS\t")) continue;
        const QStringList f = line.split('\t');
        if (f.size() < 4) continue;
        const bool directory = f[1] == "D";
        const QString name = cleanMachineField(f.mid(3).join(" "));
        auto *child = new QTreeWidgetItem(item);
        child->setText(0, name);
        child->setText(1, directory ? "Folder" : "File");
        if (!directory) {
            bool ok = false; const qulonglong bytes = f[2].toULongLong(&ok);
            if (ok) child->setText(3, formatBytes(bytes));
        }
        child->setData(0, KindRole, directory ? NodePfsDirectory : NodePfsFile);
        child->setData(0, PathRole, joinPfsPath(actualPath, name));
        child->setData(0, PartitionRole, currentOplPartition);
        child->setData(0, LoadedRole, !directory);
        if (directory) {
            auto *placeholder = new QTreeWidgetItem(child);
            placeholder->setText(0, "...");
        }
    }
    item->setData(0, PathRole, actualPath);
    item->setData(0, LoadedRole, true);
}

bool MainWindow::selectedDiskCanInstallGames(QString *reason) const
{
#ifdef __linux__
    if (!QFileInfo(writerPath).isExecutable() || !QFileInfo(hdlDumpPath).isExecutable()) {
        if (reason) *reason = "HDL backend is not ready. Run prepare_fedora_test.sh."; return false;
    }
    const int index = diskCombo ? diskCombo->currentIndex() : -1;
    if (index < 0 || static_cast<std::size_t>(index) >= disks.size()) {
        if (reason) *reason = "No PS2 HDD is selected."; return false;
    }
    const auto &disk = disks[static_cast<std::size_t>(index)];
    if (!disk.inspectionError.empty() || disk.logicalSectorSize != 512 || disk.readOnly ||
            !disk.systemDiskCheckAvailable || disk.containsSystemVolume || disk.containsBootPartition || disk.partitionCount != 0) {
        if (reason) *reason = "Selected disk does not pass the guarded raw-disk safety checks."; return false;
    }
    if (disk.size / Ps2::HddLayoutPlanner::SectorSize > Ps2::HddLayoutPlanner::BankBoundarySectors) {
        if (reason) *reason = "Physical write access above the first 2 TiB APA bank is not enabled yet."; return false;
    }
    return true;
#else
    if (reason) *reason = "Physical PS2 HDD access is currently Fedora/Linux only.";
    return false;
#endif
}

bool MainWindow::selectedDiskCanBrowsePfs(QString *reason) const
{
    if (!selectedDiskCanInstallGames(reason)) return false;
    if (!QFileInfo(pfsshellPath).isExecutable()) {
        if (reason) *reason = "PFS backend is not ready. Run prepare_fedora_test.sh."; return false;
    }
    return true;
}

bool MainWindow::probeGameImage(const QString &path, QString *media, QString *details) const
{
    QProcess probe;
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(hdlDumpPath, { "cdvd_info2", path, "--csv" });
    if (!probe.waitForStarted(5000) || !probe.waitForFinished(30000)) {
        if (details) *details = "hdl_dump cdvd_info2 could not inspect " + path; return false;
    }
    const QString output = QString::fromLocal8Bit(probe.readAll()).trimmed();
    if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
        if (details) *details = output; return false;
    }
    QString detected;
    for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith("DVD;") || trimmed.startsWith("dual-layer DVD;")) { detected = "dvd"; break; }
        if (trimmed.startsWith("CD;") || trimmed.startsWith("CD ;")) { detected = "cd"; break; }
    }
    if (detected.isEmpty()) {
        if (details) *details = "Could not determine CD/DVD media type from hdl_dump output:\n" + output; return false;
    }
    if (media) *media = detected;
    if (details) *details = output;
    return true;
}

void MainWindow::toggleMarkedPcPath(const QModelIndex &index)
{
    if (!index.isValid() || !pcModel) return;
    const QString path = normalizedLocalPath(pcModel->filePath(index.sibling(index.row(), 0)));
    if (path.isEmpty() || !QFileInfo::exists(path)) return;
    if (markedPcPaths.contains(path)) markedPcPaths.remove(path);
    else markedPcPaths.insert(path);
    if (pcView) pcView->viewport()->update();
    updateMarkedStatus();
}

QStringList MainWindow::markedOrSelectedPcPaths() const
{
    QStringList paths;
    if (!markedPcPaths.isEmpty()) {
        for (const QString &path : markedPcPaths)
            if (QFileInfo::exists(path)) paths << path;
        paths.sort(Qt::CaseInsensitive);
        return paths;
    }
    if (!pcView || !pcView->selectionModel()) return paths;
    for (const QModelIndex &index : pcView->selectionModel()->selectedRows(0)) {
        const QString path = pcModel->filePath(index);
        if (QFileInfo::exists(path)) paths << path;
    }
    return paths;
}

void MainWindow::updateMarkedStatus()
{
    if (markedPcPaths.isEmpty()) {
        if (copyToPs2Button) copyToPs2Button->setText("F5  Install selected game(s)  →  PS2 HDD");
        statusBar()->showMessage("No red transfer marks. Ctrl/Shift selection works normally; right-click toggles a persistent red mark.");
    } else {
        if (copyToPs2Button) copyToPs2Button->setText(QString("F5  Install %1 red-marked item(s)  →  PS2 HDD").arg(markedPcPaths.size()));
        statusBar()->showMessage(QString("%1 PC item(s) marked in red for the next transfer. Right-click again to unmark.")
                .arg(markedPcPaths.size()));
    }
}

void MainWindow::installSelectedPcGames()
{
    installGameFiles(markedOrSelectedPcPaths());
}

std::vector<MainWindow::InstalledGameRef> MainWindow::installedGames(bool selectedOnly) const
{
    std::vector<InstalledGameRef> result;
    if (!ps2View) return result;

    QSet<QTreeWidgetItem *> selected;
    if (selectedOnly) {
        for (QTreeWidgetItem *item : ps2View->selectedItems())
            selected.insert(item);
    }

    std::function<void(QTreeWidgetItem *, bool)> visit = [&](QTreeWidgetItem *item, bool parentSelected) {
        if (!item) return;
        const bool thisSelected = parentSelected || selected.contains(item);
        if (item->data(0, KindRole).toInt() == NodeGame && (!selectedOnly || thisSelected)) {
            const QString id = item->text(1).trimmed().toUpper();
            if (!id.isEmpty()) result.push_back({ item->text(0), id });
        }
        for (int i = 0; i < item->childCount(); ++i)
            visit(item->child(i), thisSelected);
    };
    for (int i = 0; i < ps2View->topLevelItemCount(); ++i)
        visit(ps2View->topLevelItem(i), false);

    std::sort(result.begin(), result.end(), [](const InstalledGameRef &a, const InstalledGameRef &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const InstalledGameRef &a, const InstalledGameRef &b) {
        return a.gameId == b.gameId;
    }), result.end());
    return result;
}

bool MainWindow::downloadArtworkFile(const QUrl &url, const QString &cachePath, QString *error)
{
    if (QFileInfo(cachePath).isFile() && QFileInfo(cachePath).size() > 32)
        return true;
    if (!network) {
        if (error) *error = "Qt network manager is unavailable.";
        return false;
    }
    QDir().mkpath(QFileInfo(cachePath).absolutePath());

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QString("PS2-HDD-Manager/%1").arg(PS2_HDD_APP_VERSION));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = network->get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();
    timeout.stop();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    const QByteArray data = reply->readAll();
    reply->deleteLater();
    if (networkError != QNetworkReply::NoError || status != 200) {
        if (error) *error = status ? QString("HTTP %1").arg(status) : networkErrorText;
        return false;
    }
    static const QByteArray pngSignature("\x89PNG\r\n\x1a\n", 8);
    if (!data.startsWith(pngSignature)) {
        if (error) *error = "Downloaded artwork was not a PNG file.";
        return false;
    }
    QSaveFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = "Could not save artwork cache file: " + file.errorString();
        return false;
    }
    return true;
}

void MainWindow::addArtwork()
{
#ifdef __linux__
    if (!unlockHddSession(true)) return;
    QString reason;
    if (!selectedDiskCanBrowsePfs(&reason)) {
        QMessageBox::warning(this, "Artwork installation unavailable", reason);
        return;
    }

    // Ensure both the live HDL list and OPL PFS mapping are current.
    if (installedGames(false).empty() || currentOplPartition.isEmpty() || currentOplBase.isEmpty())
        refreshCurrentPs2Tree();
    const std::vector<InstalledGameRef> allGames = installedGames(false);
    const std::vector<InstalledGameRef> selectedGames = installedGames(true);
    if (allGames.empty()) {
        QMessageBox::information(this, "No installed games", "No HDL games with Game IDs were found on the selected PS2 HDD.");
        return;
    }
    if (currentOplPartition.isEmpty() || currentOplBase.isEmpty()) {
        QMessageBox::warning(this, "OPL storage not found", "The manager could not resolve the OPL PFS partition/root on this HDD.");
        return;
    }

    OplManagerMirrorProvider provider;
    QDialog dialog(this);
    dialog.setWindowTitle("Add OPL Artwork");
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
            QString("%1\n%2\n\nThe HDD game table supplies the Game IDs, so filenames are generated automatically and installed directly into OPL Storage/ART.")
                .arg(provider.displayName(), provider.description()), &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *scope = new QComboBox(&dialog);
    scope->addItem(QString("All installed games (%1)").arg(static_cast<qulonglong>(allGames.size())), "all");
    if (!selectedGames.empty())
        scope->addItem(QString("Selected game(s) (%1)").arg(static_cast<qulonglong>(selectedGames.size())), "selected");
    layout->addWidget(new QLabel("Games:", &dialog));
    layout->addWidget(scope);

    auto *cov = new QCheckBox("COV - cover", &dialog);
    auto *ico = new QCheckBox("ICO - icon / disc artwork", &dialog);
    auto *scr = new QCheckBox("SCR - screenshots (standard + numbered slots 00/01)", &dialog);
    cov->setChecked(true); ico->setChecked(true); scr->setChecked(true);
    layout->addWidget(cov); layout->addWidget(ico); layout->addWidget(scr);
    auto *cacheNote = new QLabel(
            "Artwork is cached under ~/.cache/ps2-hdd-manager/art. Existing cached files are reused across future HDD builds. Missing artwork is skipped without aborting the batch.", &dialog);
    cacheNote->setWordWrap(true);
    layout->addWidget(cacheNote);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Download + Install Art");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    QStringList types;
    if (cov->isChecked()) types << "COV";
    if (ico->isChecked()) types << "ICO";
    if (scr->isChecked()) types << "SCR" << "SCR_00" << "SCR_01";
    if (types.isEmpty()) {
        QMessageBox::information(this, "Nothing selected", "Select at least one artwork type.");
        return;
    }
    const std::vector<InstalledGameRef> targets = scope->currentData().toString() == "selected" ? selectedGames : allGames;

    QString cacheRoot = qEnvironmentVariable("XDG_CACHE_HOME");
    if (cacheRoot.isEmpty()) cacheRoot = QDir::homePath() + "/.cache";
    cacheRoot = QDir::cleanPath(cacheRoot + "/ps2-hdd-manager/art/PS2");

    const int totalRequests = static_cast<int>(targets.size()) * types.size();
    QProgressDialog progress("Finding/downloading OPL artwork...", "Cancel", 0, totalRequests, this);
    progress.setWindowTitle("Add OPL Artwork");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    QStringList localFiles;
    int downloaded = 0, cached = 0, missing = 0;
    int step = 0;
    QStringList missingExamples;
    for (const InstalledGameRef &game : targets) {
        for (const QString &type : types) {
            progress.setLabelText(QString("%1\n%2 — %3").arg(game.name, game.gameId, type));
            progress.setValue(step++);
            QApplication::processEvents();
            if (progress.wasCanceled()) {
                progress.close();
                statusLabel->setText("Artwork download cancelled. Cached files were kept; the PS2 HDD was not changed by the cancelled batch.");
                return;
            }
            const QString folder = QDir(cacheRoot).filePath(game.gameId);
            const QString fileName = game.gameId + "_" + type + ".png";
            const QString cachePath = QDir(folder).filePath(fileName);
            const bool existed = QFileInfo(cachePath).isFile() && QFileInfo(cachePath).size() > 32;
            QString error;
            if (downloadArtworkFile(provider.artworkUrl(game.gameId, type), cachePath, &error)) {
                localFiles << cachePath;
                if (existed) cached++; else downloaded++;
            } else {
                missing++;
                if (missingExamples.size() < 8)
                    missingExamples << QString("%1 %2 (%3)").arg(game.gameId, type, error);
            }
        }
    }
    progress.setValue(totalRequests);
    progress.close();

    if (localFiles.isEmpty()) {
        QMessageBox::information(this, "No artwork found",
                QString("No requested artwork was available for the selected games.\n\nMissing/unavailable: %1").arg(missing));
        return;
    }
    localFiles.removeDuplicates();

    const QString artPath = joinPfsPath(currentOplBase, "ART");
    QString manifestPath, error;
    int fileCount = 0;
    if (!createPfsCopyManifest(localFiles, artPath, false, &manifestPath, &fileCount, &error)) {
        QMessageBox::critical(this, "Could not prepare artwork install", error);
        return;
    }

    const int diskIndex = diskCombo ? diskCombo->currentIndex() : -1;
    if (diskIndex < 0 || static_cast<std::size_t>(diskIndex) >= disks.size()) {
        QFile::remove(manifestPath);
        return;
    }
    const auto &disk = disks[static_cast<std::size_t>(diskIndex)];
    transferProgress->setRange(0, 0);
    transferProgress->setVisible(true);
    progressDetail->setVisible(true);
    progressDetail->setText(QString("Installing %1 artwork file(s) into %2...").arg(fileCount).arg(artPath));
    statusLabel->setText("Installing artwork directly into OPL PFS storage...");
    QString output;
    const bool ok = runPrivilegedWriter(disk,
            { "--pfsshell", pfsshellPath, "--copy-manifest", manifestPath, "--partition", currentOplPartition }, &output);
    QFile::remove(manifestPath);
    resetProgress();
    if (!ok) {
        QMessageBox::critical(this, "Artwork PFS copy failed", output.right(10000));
        return;
    }

    // The managed OPL preset already enables cover art. 0.5 also asks the
    // writer to patch only enable_coverart on existing configs, preserving
    // every other user setting.
    QString configOutput;
    const bool configOk = runPrivilegedWriter(disk,
            { "--pfsshell", pfsshellPath, "--ensure-cover-art", "--partition", currentOplPartition,
              "--pfs-path", currentOplBase }, &configOutput);

    refreshCurrentPs2Tree();
    QString summary = QString("Artwork installed: %1 file(s)\nDownloaded now: %2\nReused from cache: %3\nUnavailable: %4")
            .arg(fileCount).arg(downloaded).arg(cached).arg(missing);
    if (!configOk)
        summary += "\n\nArtwork files are installed, but enable_coverart could not be patched automatically:\n" + configOutput.right(1200);
    if (!missingExamples.isEmpty())
        summary += "\n\nExamples not found:\n" + missingExamples.join('\n');
    statusLabel->setText(QString("Installed %1 OPL artwork file(s) directly from the local cache/database provider.").arg(fileCount));
    QMessageBox::information(this, "OPL artwork installed", summary);
#else
    QMessageBox::information(this, "Unavailable", "Direct PS2 HDD artwork installation is currently Fedora/Linux only.");
#endif
}

void MainWindow::resetProgress()
{
    if (!transferProgress || !progressDetail) return;
    transferProgress->setRange(0, 100); transferProgress->setValue(0); transferProgress->setVisible(false);
    progressDetail->clear(); progressDetail->setVisible(false);
}

void MainWindow::parseTransferProgress(const QString &text, const QString &prefix)
{
    static const QRegularExpression re(R"((\d{1,3})%,\s*([^,\r\n]+)\s+remaining,\s*([0-9.]+)\s+MB/sec)");
    auto matches = re.globalMatch(text);
    QRegularExpressionMatch last;
    while (matches.hasNext()) last = matches.next();
    if (!last.hasMatch()) return;
    const int pc = std::clamp(last.captured(1).toInt(), 0, 100);
    transferProgress->setRange(0, 100); transferProgress->setValue(pc); transferProgress->setVisible(true);
    progressDetail->setText(QString("%1%2% — %3 remaining — %4 MB/s")
            .arg(prefix.isEmpty() ? QString() : prefix + " — ").arg(pc).arg(last.captured(2).trimmed()).arg(last.captured(3)));
    progressDetail->setVisible(true);
}

void MainWindow::installGameFiles(const QStringList &inputPaths)
{
#ifdef __linux__
    if (!unlockHddSession(true)) return;
    QString reason;
    if (!selectedDiskCanInstallGames(&reason)) { QMessageBox::warning(this, "HDL game install unavailable", reason); return; }
    QSet<QString> seen; QStringList paths;
    for (const QString &p : inputPaths) {
        const QFileInfo info(p);
        if (!info.isFile() || !isDiscImagePath(p)) continue;
        QString key = info.canonicalFilePath(); if (key.isEmpty()) key = info.absoluteFilePath();
        if (!seen.contains(key)) { seen.insert(key); paths << info.absoluteFilePath(); }
    }
    if (paths.isEmpty()) {
        QMessageBox::information(this, "No PS2 images", "Drop/select one or more supported PS2 disc images (ISO/BIN/GI/IML/NRG/ZSO)."); return;
    }

    struct Game { QString path, name, media; };
    std::vector<Game> games; QStringList preview;
    setCursor(Qt::WaitCursor);
    for (const QString &path : paths) {
        QString media, probe;
        if (!probeGameImage(path, &media, &probe)) {
            unsetCursor(); QMessageBox::critical(this, "Not a supported PS2 disc image", path + "\n\n" + probe.right(5000)); return;
        }
        QString name = QFileInfo(path).completeBaseName().trimmed();
        if (name.isEmpty()) name = "PS2 Game"; if (name.size() > 159) name.truncate(159);
        games.push_back({ path, name, media }); preview << QString("%1 [%2]").arg(name, media.toUpper());
    }
    unsetCursor();

    const auto &disk = disks[static_cast<std::size_t>(diskCombo->currentIndex())];
    if (QMessageBox::question(this, "Install games to PS2 HDD",
            QString("Install %1 game(s) as normal HDL APA partitions?\n\n%2\n\n%3")
                .arg(static_cast<qulonglong>(games.size()))
                .arg(QString::fromStdString(disk.devicePath))
                .arg(preview.join('\n')),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;

    copyToPs2Button->setEnabled(false);
    transferProgress->setVisible(true); progressDetail->setVisible(true);
    for (std::size_t i = 0; i < games.size(); i++) {
        const Game &game = games[i];
        transferProgress->setRange(0,100); transferProgress->setValue(0);
        const QString prefix = QString("Game %1 of %2: %3").arg(i + 1).arg(static_cast<qulonglong>(games.size())).arg(game.name);
        progressDetail->setText(prefix); statusLabel->setText("Installing " + game.name + "...");
        QString output;
        const QStringList args = { "--hdl-dump", hdlDumpPath, "--install-game", game.path,
                "--game-name", game.name, "--media", game.media };
        if (!runPrivilegedWriter(disk, args, &output, true, prefix)) {
            copyToPs2Button->setEnabled(selectedDiskCanInstallGames()); resetProgress();
            QMessageBox::critical(this, "HDL game installation failed", game.name + "\n\n" + output.right(9000));
            refreshCurrentPs2Tree(); return;
        }
        transferProgress->setValue(100);
    }
    copyToPs2Button->setEnabled(selectedDiskCanInstallGames());
    progressDetail->setText(QString("Complete — %1 game(s) installed and verified").arg(static_cast<qulonglong>(games.size())));
    transferProgress->setValue(100);
    statusLabel->setText("HDL install complete. Refreshing the real game table...");
    refreshCurrentPs2Tree();
    QMessageBox::information(this, "HDL game installation complete",
            QString("%1 game(s) installed and verified. The right pane has been re-read from the HDD.").arg(static_cast<qulonglong>(games.size())));
#else
    Q_UNUSED(inputPaths);
#endif
}

bool MainWindow::createPfsCopyManifest(const QStringList &paths, const QString &targetPath,
        bool smartOplRoot, QString *manifestPath, int *fileCount, QString *error) const
{
    QTemporaryFile temp(QDir::tempPath() + "/ps2-hdd-pfs-copy-XXXXXX.manifest");
    temp.setAutoRemove(false);
    if (!temp.open()) { if (error) *error = temp.errorString(); return false; }
    QTextStream out(&temp);
    int files = 0;
    const QSet<QString> known = { "ART", "CFG", "APPS", "THM", "VMC", "LNG", "CHT" };

    auto writeDir = [&](const QString &remote) { out << "D\t" << remote << "\n"; };
    auto writeFile = [&](const QString &local, const QString &remote) {
        if (local.contains('\t') || local.contains('\n') || remote.contains('\t') || remote.contains('\n')) return false;
        out << "F\t" << local << "\t" << remote << "\n"; files++; return true;
    };

    for (const QString &sourcePath : paths) {
        QFileInfo info(sourcePath);
        if (!info.exists()) continue;
        QString base = targetPath;
        if (info.isDir()) {
            QString folder = info.fileName();
            if (smartOplRoot && known.contains(folder.toUpper())) folder = folder.toUpper();
            base = joinPfsPath(targetPath, folder);
            writeDir(base);
            QDir sourceDir(info.absoluteFilePath());
            QDirIterator it(info.absoluteFilePath(), QDir::AllEntries | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString local = it.next();
                const QFileInfo child = it.fileInfo();
                const QString relative = sourceDir.relativeFilePath(local);
                const QString remote = joinPfsPath(base, relative);
                if (child.isDir()) writeDir(remote);
                else if (child.isFile() && !writeFile(child.absoluteFilePath(), remote)) {
                    if (error) *error = "Tabs/newlines in file paths are not supported by the batch copy manifest.";
                    temp.remove(); return false;
                }
            }
        } else if (info.isFile()) {
            if (!writeFile(info.absoluteFilePath(), joinPfsPath(base, info.fileName()))) {
                if (error) *error = "Tabs/newlines in file paths are not supported by the batch copy manifest.";
                temp.remove(); return false;
            }
        }
    }
    out.flush(); temp.flush();
    if (files == 0) { if (error) *error = "No files were found to copy."; temp.remove(); return false; }
    if (manifestPath) *manifestPath = temp.fileName();
    if (fileCount) *fileCount = files;
    temp.close();
    return true;
}

void MainWindow::copyPcItemsToPfs(const QStringList &paths, QTreeWidgetItem *target)
{
#ifdef __linux__
    if (!unlockHddSession(true)) return;
    QString reason;
    if (!selectedDiskCanBrowsePfs(&reason)) { QMessageBox::warning(this, "PFS copy unavailable", reason); return; }
    if (!target) return;
    int kind = target->data(0, KindRole).toInt();
    if (kind == NodePfsFile) target = target->parent();
    if (!target || target->data(0, KindRole).toInt() != NodePfsDirectory) {
        QMessageBox::information(this, "Choose an OPL folder", "Drop files/folders onto OPL Storage or one of its PFS folders."); return;
    }
    const QString targetPath = target->data(0, PathRole).toString();
    const bool smartRoot = !currentOplBase.isEmpty() && targetPath == currentOplBase;
    QString manifest, error; int count = 0;
    if (!createPfsCopyManifest(paths, targetPath, smartRoot, &manifest, &count, &error)) {
        QMessageBox::warning(this, "Could not prepare PFS copy", error); return;
    }
    const auto &disk = disks[static_cast<std::size_t>(diskCombo->currentIndex())];
    if (QMessageBox::question(this, "Copy files to OPL storage",
            QString("Copy/merge %1 file(s) into %2?\n\nExisting files with the same name will be replaced completely.")
                .arg(count).arg(targetPath), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        QFile::remove(manifest); return;
    }

    transferProgress->setRange(0,0); transferProgress->setVisible(true);
    progressDetail->setText(QString("Copying %1 file(s) to %2...").arg(count).arg(targetPath)); progressDetail->setVisible(true);
    QString output;
    const bool ok = runPrivilegedWriter(disk,
            { "--pfsshell", pfsshellPath, "--copy-manifest", manifest, "--partition", "auto" }, &output);
    QFile::remove(manifest);
    resetProgress();
    if (!ok) { QMessageBox::critical(this, "PFS copy failed", output.right(9000)); return; }
    statusLabel->setText(QString("Copied %1 file(s) to OPL storage; pfsshell completed without command errors.").arg(count));
    target->setData(0, LoadedRole, false);
    populatePfsDirectory(target, targetPath, disk);
#else
    Q_UNUSED(paths); Q_UNUSED(target);
#endif
}

void MainWindow::installOrUpdateOplApps()
{
#ifdef __linux__
    if (!unlockHddSession(true)) return;
    QString reason;
    if (!selectedDiskCanBrowsePfs(&reason)) {
        QMessageBox::warning(this, "OPL Apps installation unavailable", reason);
        return;
    }
    if (!QFileInfo(fetchPayloadsPath).isExecutable()) {
        QMessageBox::warning(this, "Payload downloader unavailable",
                "fetch_payloads.sh is not executable. Run prepare_fedora_test.sh once for this source tree.");
        return;
    }

    const int index = diskCombo ? diskCombo->currentIndex() : -1;
    if (index < 0 || static_cast<std::size_t>(index) >= disks.size())
        return;
    const auto &disk = disks[static_cast<std::size_t>(index)];

    if (currentOplPartition.isEmpty() || currentOplBase.isEmpty()) {
        refreshCurrentPs2Tree();
        if (currentOplPartition.isEmpty() || currentOplBase.isEmpty()) {
            QMessageBox::warning(this, "OPL storage not found",
                    "The manager could not resolve an OPL PFS partition/root on this HDD.");
            return;
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Install / Update OPL Apps");
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
            "Install current payloads into the existing OPL PFS storage. No format is performed.\n\n"
            "Downloads and PS2 build tools are cached under ~/.cache/ps2-hdd-manager, so unchanged payloads/backends are reused.",
            &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *opl = new QCheckBox("OPL Beta - update managed FHDB OPNPS2LD.ELF", &dialog);
    const bool managedFhdb = currentOplPartition == "PP.FHDB.APPS";
    opl->setEnabled(managedFhdb);
    opl->setChecked(managedFhdb);
    opl->setToolTip(managedFhdb
            ? "Updates /OPL/OPNPS2LD.ELF in the PS2 HDD Manager / FHDB layout."
            : "Disabled because this HDD does not use the managed PP.FHDB.APPS layout; the manager will not guess where your booted OPL ELF lives.");
    auto *wle = new QCheckBox("wLaunchELF_ISR - latest normal BOOT.ELF", &dialog);
    auto *mca = new QCheckBox("Memory Card Annihilator - latest automated build", &dialog);
    auto *enabler = new QCheckBox("FHDB HDD Boot Configuration - Status / Enable / Disable / Verify", &dialog);
    auto *preset = new QCheckBox("Apply PS2 HDD Manager recommended OPL settings (replace conf_opl.cfg)", &dialog);
    wle->setChecked(true);
    mca->setChecked(true);
    enabler->setChecked(true);
    preset->setChecked(true);
    layout->addWidget(opl);
    layout->addWidget(wle);
    layout->addWidget(mca);
    layout->addWidget(enabler);
    layout->addWidget(preset);

    auto *note = new QLabel(
            "The first build of the dedicated FHDB utility may download the official PS2DEV toolchain once. "
            "Later builds reuse the cached toolchain. The EEPROM utility itself does not write anything until you explicitly choose Enable or Disable on the PS2.",
            &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Install / Update");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QStringList fetchArgs;
    if (opl->isChecked()) fetchArgs << "--opl";
    if (wle->isChecked()) fetchArgs << "--wle";
    if (mca->isChecked()) fetchArgs << "--mca";
    if (enabler->isChecked()) fetchArgs << "--hdd-enabler";
    if (fetchArgs.isEmpty() && !preset->isChecked()) {
        QMessageBox::information(this, "Nothing selected", "No payload or configuration action was selected.");
        return;
    }

    QString fetchOutput;
    if (!fetchArgs.isEmpty()) {
        transferProgress->setRange(0, 0);
        transferProgress->setVisible(true);
        progressDetail->setText("Resolving/downloading selected OPL Apps (persistent cache enabled)...");
        progressDetail->setVisible(true);
        statusLabel->setText("Preparing selected OPL Apps...");

        QProcess fetch(this);
        fetch.setWorkingDirectory(QFileInfo(fetchPayloadsPath).absolutePath());
        fetch.setProcessChannelMode(QProcess::MergedChannels);
        fetch.start(fetchPayloadsPath, fetchArgs);
        if (!fetch.waitForStarted(10000)) {
            resetProgress();
            QMessageBox::critical(this, "Could not start payload preparation", fetch.errorString());
            return;
        }
        while (fetch.state() != QProcess::NotRunning) {
            fetch.waitForReadyRead(100);
            const QString chunk = QString::fromLocal8Bit(fetch.readAll());
            if (!chunk.isEmpty()) {
                fetchOutput += chunk;
                const QStringList lines = chunk.split('\n', Qt::SkipEmptyParts);
                if (!lines.isEmpty()) progressDetail->setText(lines.last().trimmed());
            }
            QApplication::processEvents();
            fetch.waitForFinished(20);
        }
        fetchOutput += QString::fromLocal8Bit(fetch.readAll());
        if (fetch.exitStatus() != QProcess::NormalExit || fetch.exitCode() != 0) {
            resetProgress();
            QMessageBox::critical(this, "Payload preparation failed", fetchOutput.right(10000));
            return;
        }
    }

    QTemporaryFile manifestFile(QDir::tempPath() + "/ps2-hdd-app-update-XXXXXX.manifest");
    manifestFile.setAutoRemove(false);
    if (!manifestFile.open()) {
        resetProgress();
        QMessageBox::critical(this, "Could not prepare Apps update", manifestFile.errorString());
        return;
    }
    QTextStream manifest(&manifestFile);
    QStringList temporarySources;
    QString error;
    int copiedFiles = 0;

    auto addDirectory = [&](const QString &remote) {
        manifest << "D\t" << remote << "\n";
    };
    auto addFile = [&](const QString &local, const QString &remote) -> bool {
        if (!QFileInfo(local).isFile()) {
            error = "Required prepared payload is missing: " + local;
            return false;
        }
        manifest << "F\t" << local << "\t" << remote << "\n";
        copiedFiles++;
        return true;
    };
    auto addText = [&](const QByteArray &data, const QString &remote) -> bool {
        auto *file = new QTemporaryFile(QDir::tempPath() + "/ps2-hdd-app-text-XXXXXX.tmp", &dialog);
        file->setAutoRemove(false);
        if (!file->open() || file->write(data) != data.size()) {
            error = "Could not create a temporary OPL metadata/config file: " + file->errorString();
            delete file;
            return false;
        }
        file->flush();
        const QString local = file->fileName();
        file->close();
        temporarySources << local;
        delete file;
        manifest << "F\t" << local << "\t" << remote << "\n";
        copiedFiles++;
        return true;
    };
    auto addApp = [&](const QString &folder, const QString &elf, const QString &title) -> bool {
        const QString appRoot = joinPfsPath(joinPfsPath(currentOplBase, "APPS"), folder);
        addDirectory(joinPfsPath(currentOplBase, "APPS"));
        addDirectory(appRoot);
        return addFile(elf, joinPfsPath(appRoot, "BOOT.ELF")) &&
                addText(QString("title=%1\nboot=BOOT.ELF\n").arg(title).toUtf8(), joinPfsPath(appRoot, "title.cfg"));
    };

    bool prepared = true;
    if (opl->isChecked())
        prepared = addFile(runtimePayloadPath + "/opl/OPNPS2LD.ELF", joinPfsPath(currentOplBase, "OPNPS2LD.ELF"));
    if (prepared && wle->isChecked())
        prepared = addApp("wLaunchELF", runtimePayloadPath + "/wle/BOOT.ELF", "wLaunchELF");
    if (prepared && mca->isChecked())
        prepared = addApp("Memory-Card-Annihilator", runtimePayloadPath + "/mca/BOOT.ELF", "Memory Card Annihilator");
    if (prepared && enabler->isChecked())
        prepared = addApp("FHDB-HDD-Boot-Config", runtimePayloadPath + "/fhdb-enabler/FHDB-Boot-Config.ELF", "FHDB HDD Boot Configuration");
    if (prepared && preset->isChecked()) {
        Ps2::OplPresetOptions options;
        options.enableApps = true;
        prepared = addText(QByteArray::fromStdString(Ps2::OplConfig::BuildInternalHddPreset(options)),
                joinPfsPath(currentOplBase, "conf_opl.cfg"));
    }
    manifest.flush();
    manifestFile.flush();
    const QString manifestPath = manifestFile.fileName();
    manifestFile.close();

    if (!prepared) {
        QFile::remove(manifestPath);
        for (const QString &path : temporarySources) QFile::remove(path);
        resetProgress();
        QMessageBox::critical(this, "Could not prepare Apps update", error);
        return;
    }

    transferProgress->setRange(0, 0);
    transferProgress->setVisible(true);
    progressDetail->setText(QString("Installing %1 prepared file(s) into OPL storage...").arg(copiedFiles));
    progressDetail->setVisible(true);
    statusLabel->setText("Installing/updating OPL Apps on the existing HDD...");
    QString writerOutput;
    const bool ok = runPrivilegedWriter(disk,
            { "--pfsshell", pfsshellPath, "--copy-manifest", manifestPath, "--partition", currentOplPartition },
            &writerOutput);
    QFile::remove(manifestPath);
    for (const QString &path : temporarySources) QFile::remove(path);
    resetProgress();

    if (!ok) {
        QMessageBox::critical(this, "OPL Apps update failed", writerOutput.right(10000));
        return;
    }

    statusLabel->setText("Selected OPL Apps/settings installed; pfsshell completed without command errors. Refreshing OPL Storage...");
    refreshCurrentPs2Tree();
    QMessageBox::information(this, "OPL Apps updated",
            "Selected OPL Apps and settings were installed on the existing HDD without formatting it.\n\n"
            "The new FHDB HDD Boot Configuration utility starts read-only and exposes Status, Enable, Disable and Verify on the PS2.");
#else
    QMessageBox::information(this, "Unavailable", "Physical OPL Apps installation is currently Fedora/Linux only.");
#endif
}

void MainWindow::applyRecommendedOplDefaults()
{
#ifdef __linux__
    if (!unlockHddSession(true)) return;
    QString reason;
    if (!selectedDiskCanBrowsePfs(&reason)) {
        QMessageBox::warning(this, "OPL configuration unavailable", reason);
        return;
    }

    const int index = diskCombo ? diskCombo->currentIndex() : -1;
    if (index < 0 || static_cast<std::size_t>(index) >= disks.size())
        return;
    const auto &disk = disks[static_cast<std::size_t>(index)];

    // Ensure we have resolved the actual OPL PFS root (+OPL => /, custom => /OPL).
    if (currentOplPartition.isEmpty() || currentOplBase.isEmpty()) {
        refreshCurrentPs2Tree();
        if (currentOplPartition.isEmpty() || currentOplBase.isEmpty()) {
            QMessageBox::warning(this, "OPL storage not found",
                    "The manager could not resolve the OPL PFS partition/root on this HDD.");
            return;
        }
    }

    Ps2::OplPresetOptions preset;
    preset.enableApps = true;
    // Do not set a direct HDD/PFS IGR target here. OPL's EE IGR loader does not
    // initialize HDD/PFS before LoadElf(), so the safe preset leaves exit_path
    // alone instead of generating a path that may fail after an in-game reset.
    const QByteArray cfg = QByteArray::fromStdString(Ps2::OplConfig::BuildInternalHddPreset(preset));

    QTemporaryFile cfgFile(QDir::tempPath() + "/ps2-hdd-conf-opl-XXXXXX.cfg");
    cfgFile.setAutoRemove(false);
    if (!cfgFile.open() || cfgFile.write(cfg) != cfg.size()) {
        QMessageBox::critical(this, "Could not prepare OPL config", cfgFile.errorString());
        return;
    }
    cfgFile.flush();
    const QString cfgPath = cfgFile.fileName();
    cfgFile.close();

    QTemporaryFile manifestFile(QDir::tempPath() + "/ps2-hdd-opl-config-XXXXXX.manifest");
    manifestFile.setAutoRemove(false);
    if (!manifestFile.open()) {
        QFile::remove(cfgPath);
        QMessageBox::critical(this, "Could not prepare OPL config", manifestFile.errorString());
        return;
    }
    const QString remote = joinPfsPath(currentOplBase, "conf_opl.cfg");
    QTextStream manifest(&manifestFile);
    manifest << "F\t" << cfgPath << "\t" << remote << "\n";
    manifest.flush();
    manifestFile.flush();
    const QString manifestPath = manifestFile.fileName();
    manifestFile.close();

    const QString explanation =
            "This writes PS2 HDD Manager's recommended OPL defaults to:\n\n" + remote +
            "\n\nInternal HDD: AUTO + default screen\n"
            "Apps: AUTO\nCover art: enabled\nWrite/delete/rename: enabled\n"
            "HDD game-list cache: enabled\nAuto-refresh/sort: enabled\nUSB/ETH startup: disabled\n\n"
            "IGR exit_path is intentionally left unset. Stock OPL's IGR loader does not bring up HDD/PFS "
            "before loading a custom exit ELF, so we will not automate an unsafe HDD return path.";
    if (QMessageBox::question(this, "Apply recommended OPL defaults", explanation,
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes) != QMessageBox::Yes) {
        QFile::remove(manifestPath);
        QFile::remove(cfgPath);
        return;
    }

    transferProgress->setRange(0, 0);
    transferProgress->setVisible(true);
    progressDetail->setText("Writing conf_opl.cfg...");
    progressDetail->setVisible(true);
    QString output;
    const bool ok = runPrivilegedWriter(disk,
            { "--pfsshell", pfsshellPath, "--copy-manifest", manifestPath, "--partition", currentOplPartition },
            &output);
    QFile::remove(manifestPath);
    QFile::remove(cfgPath);
    resetProgress();

    if (!ok) {
        QMessageBox::critical(this, "OPL configuration failed", output.right(9000));
        return;
    }
    statusLabel->setText("Recommended OPL defaults written successfully. OPL should now auto-start on Internal HDD games.");
    QMessageBox::information(this, "OPL configured",
            "Recommended OPL defaults were written successfully.\n\n"
            "On the next OPL launch, Internal HDD should initialize automatically and be the default device.");
#else
    QMessageBox::information(this, "Unavailable", "Physical OPL configuration is currently Fedora/Linux only.");
#endif
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (pcView && watched == pcView->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::RightButton) {
            const QModelIndex index = pcView->indexAt(mouse->position().toPoint());
            if (index.isValid()) {
                toggleMarkedPcPath(index);
                return true;
            }
        }
    }
    if (ps2View && watched == ps2View->viewport()) {
        if (event->type() == QEvent::DragEnter) {
            auto *drag = static_cast<QDragEnterEvent *>(event);
            if (!localUrls(drag->mimeData()).isEmpty()) { drag->acceptProposedAction(); return true; }
        } else if (event->type() == QEvent::DragMove) {
            auto *drag = static_cast<QDragMoveEvent *>(event);
            if (!localUrls(drag->mimeData()).isEmpty()) { drag->acceptProposedAction(); return true; }
        } else if (event->type() == QEvent::Drop) {
            auto *drop = static_cast<QDropEvent *>(event);
            const QStringList paths = localUrls(drop->mimeData());
            if (paths.isEmpty()) return false;
            QTreeWidgetItem *target = ps2View->itemAt(drop->position().toPoint());
            int kind = target ? target->data(0, KindRole).toInt() : NodeNone;
            if (kind == NodeGame && target->parent()) { target = target->parent(); kind = target->data(0, KindRole).toInt(); }
            const bool gameTarget = kind == NodeGamesRoot || kind == NodeGamesMedia;
            if (gameTarget) {
                drop->acceptProposedAction(); installGameFiles(paths); return true;
            }
            if (kind == NodePfsDirectory || kind == NodePfsFile) {
                drop->acceptProposedAction(); copyPcItemsToPfs(paths, target); return true;
            }
            // Dropping disc images anywhere else on the PS2 pane still means install games.
            if (std::all_of(paths.begin(), paths.end(), [](const QString &p) { return QFileInfo(p).isFile() && isDiscImagePath(p); })) {
                drop->acceptProposedAction(); installGameFiles(paths); return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}
