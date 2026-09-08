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
#include <QWidget>
#include <QStyle>
#include <QPalette>
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
    const QFileInfo info(path);
    if (!info.exists())
        return QString();
    return QDir::cleanPath(info.absoluteFilePath());
}

class MarkedFileDelegate final : public QStyledItemDelegate
{
public:
    MarkedFileDelegate(QFileSystemModel *model, const QSet<QString> *marked, QObject *parent)
        : QStyledItemDelegate(parent), model(model), marked(marked) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
            const QModelIndex &index) const override
    {
        const QModelIndex rowIndex = index.sibling(index.row(), 0);
        const QString path = model ? normalizedLocalPath(model->filePath(rowIndex)) : QString();

        if (!model || !marked || !marked->contains(path)) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        const bool dark = option.palette.color(QPalette::Base).lightness() < 128;
        QStyleOptionViewItem markedOption(option);
        initStyleOption(&markedOption, index);

        const QColor background = dark
                ? QColor(139, 38, 48)
                : QColor(255, 205, 205);
        const QColor foreground = dark
                ? QColor(255, 255, 255)
                : QColor(105, 0, 16);

        markedOption.state &= ~QStyle::State_Selected;
        markedOption.backgroundBrush = background;
        markedOption.palette.setColor(QPalette::Text, foreground);
        markedOption.palette.setColor(QPalette::WindowText, foreground);

        const QWidget *widget = markedOption.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &markedOption, painter, widget);
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
    commandBar->addWidget(new QLabel("Game destination:", central));
    gameBankCombo = new QComboBox(central);
    gameBankCombo->setMinimumWidth(270);
    gameBankCombo->addItem(
            "AUTO — fill banks automatically",
            -1);
    gameBankCombo->addItem("Bank 0", 0);
    commandBar->addWidget(gameBankCombo);
    copyToPs2Button = new QPushButton("F5  Install selected game(s)  →  PS2 HDD", central);
    copyToPs2Button->setShortcut(QKeySequence(QStringLiteral("F5")));
    copyToPs2Button->setToolTip("Installs selected PS2 disc images as normal HDL APA game partitions.");
    commandBar->addWidget(copyToPs2Button);
    commandBar->addStretch();
    layout->addLayout(commandBar);

    overallProgressDetail = new QLabel(central);
    overallProgressDetail->setVisible(false);
    overallProgress = new QProgressBar(central);
    overallProgress->setRange(0, 100);
    overallProgress->setValue(0);
    overallProgress->setVisible(false);
    transferProgress = new QProgressBar(central);
    transferProgress->setRange(0, 100);
    transferProgress->setValue(0);
    transferProgress->setVisible(false);
    progressDetail = new QLabel(central);
    progressDetail->setWordWrap(true);
    progressDetail->setVisible(false);
    layout->addWidget(overallProgressDetail);
    layout->addWidget(overallProgress);
    layout->addWidget(progressDetail);
    layout->addWidget(transferProgress);

    statusLabel = new QLabel("Ready.", central);
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
    pcView->setContextMenuPolicy(Qt::NoContextMenu);
    pcView->viewport()->installEventFilter(this);
    pcView->setItemDelegate(new MarkedFileDelegate(pcModel, &markedPcPaths, pcView));
    pcView->setDragEnabled(true);
    pcView->setDragDropMode(QAbstractItemView::DragOnly);
    pcView->setSelectionBehavior(QAbstractItemView::SelectRows);
    pcView->setAlternatingRowColors(false);
    pcView->setUniformRowHeights(true);
    pcView->setSortingEnabled(true);
    pcView->sortByColumn(0, Qt::AscendingOrder);
    pcView->setRootIsDecorated(false);
    pcView->setItemsExpandable(false);
    pcView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < 4; i++) pcView->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    layout->addWidget(pcView, 1);
    auto *queueBar = new QHBoxLayout();
    auto *addSelectedToQueue = new QPushButton("Add Selected to Queue", group);
    auto *unmarkAll = new QPushButton("Unmark All", group);
    addSelectedToQueue->setToolTip("Append the currently selected PS2 disc images to the persistent transfer queue.");
    unmarkAll->setToolTip("Clear all persistent transfer marks / queued games.");
    queueBar->addWidget(addSelectedToQueue);
    queueBar->addWidget(unmarkAll);
    queueBar->addStretch();
    layout->addLayout(queueBar);

    connect(addSelectedToQueue, &QPushButton::clicked, this, [this]() {
        if (!pcView || !pcView->selectionModel())
            return;

        if (!gameBankCombo ||
                gameBankCombo->currentIndex() < 0) {
            statusBar()->showMessage(
                    "Choose AUTO or a destination bank before adding games to the queue.");
            return;
        }

        const int queueBank = gameBankCombo->currentData().toInt();

        QSet<QString> selectedPaths;
        const QModelIndexList indexes =
                pcView->selectionModel()->selectedIndexes();

        for (const QModelIndex &index : indexes) {
            if (!index.isValid())
                continue;

            const QModelIndex row = index.sibling(index.row(), 0);
            const QString path = normalizedLocalPath(pcModel->filePath(row));
            const QFileInfo info(path);

            if (info.isFile() && isDiscImagePath(path))
                selectedPaths.insert(path);
        }

        if (selectedPaths.isEmpty() && pcView->currentIndex().isValid()) {
            const QModelIndex row =
                    pcView->currentIndex().sibling(pcView->currentIndex().row(), 0);
            const QString path = normalizedLocalPath(pcModel->filePath(row));
            const QFileInfo info(path);

            if (info.isFile() && isDiscImagePath(path))
                selectedPaths.insert(path);
        }

        int added = 0;
        int reassigned = 0;

        for (const QString &path : selectedPaths) {
            if (!markedPcPaths.contains(path)) {
                markedPcPaths.insert(path);
                queuedPcBanks.insert(path, queueBank);
                ++added;
            } else if (queuedPcBanks.value(path, queueBank) != queueBank) {
                queuedPcBanks.insert(path, queueBank);
                ++reassigned;
            }
        }

        pcView->viewport()->update();
        pcView->update();
        updateMarkedStatus();

        QString message;
        if (added > 0)
            message +=
                    QString("Added %1 game(s) to %2 queue.")
                    .arg(added)
                    .arg(
                        queueBank < 0
                            ? QString("AUTO")
                            : QString("Bank %1").arg(queueBank));
        if (reassigned > 0) {
            if (!message.isEmpty())
                message += " ";
            message +=
                    QString("Reassigned %1 queued game(s) to %2.")
                    .arg(reassigned)
                    .arg(
                        queueBank < 0
                            ? QString("AUTO")
                            : QString("Bank %1").arg(queueBank));
        }

        if (message.isEmpty())
            message = "No new supported PS2 disc images were added to the queue.";

        statusBar()->showMessage(message);
    });

    connect(unmarkAll, &QPushButton::clicked, this, [this]() {
        markedPcPaths.clear();
        queuedPcBanks.clear();

        if (pcView) {
            pcView->viewport()->update();
            pcView->update();
        }

        updateMarkedStatus();
        statusBar()->showMessage("Transfer queue cleared.");
    });

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
    ps2View->setColumnCount(5);
    ps2View->setHeaderLabels({ "Name", "Game ID / Type", "Media", "Size", "Bank" });
    ps2View->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ps2View->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ps2View->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ps2View->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    ps2View->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    layout->addWidget(ps2View, 1);

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
    if (gameBankCombo) {
        const int previous = gameBankCombo->currentData().toInt();
        gameBankCombo->clear();
        gameBankCombo->addItem(
                "AUTO — fill banks automatically",
                -1);
        const auto plan =
                Ps2::HddLayoutPlanner::Plan(
                    disks[static_cast<std::size_t>(index)].size,
                    Ps2::HddLayoutMode::ExtendedApaBanks);
        for (const auto &bank : plan.banks)
            gameBankCombo->addItem(QString("Bank %1 — starts at %2 TiB").arg(bank.index).arg(bank.index * 2), static_cast<int>(bank.index));
        const int restore = gameBankCombo->findData(previous);
        gameBankCombo->setCurrentIndex(restore >= 0 ? restore : 0);
    }
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
        const bool banked = f.size() >= 6 && f[1] != "DVD" && f[1] != "CD";
        const int bank = banked ? cleanMachineField(f[1]).toInt() : 0;
        const int mediaIndex = banked ? 2 : 1;
        const int sizeIndex = banked ? 3 : 2;
        const int startupIndex = banked ? 4 : 3;
        const int nameIndex = banked ? 5 : 4;
        const QString media = cleanMachineField(f[mediaIndex]);
        bool ok = false;
        const qulonglong kb = cleanMachineField(f[sizeIndex]).toULongLong(&ok);
        const QString startup = cleanMachineField(f[startupIndex]);
        const QString name = cleanMachineField(f.mid(nameIndex).join(" "));
        QTreeWidgetItem *parent = media == "CD" ? cd : dvd;
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, name);
        item->setText(1, startup);
        item->setText(2, media);
        item->setText(3, ok ? formatBytes(static_cast<std::uint64_t>(kb) * 1024ULL) : QString());
        item->setText(4, QString("Bank %1").arg(bank));
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
    if (!index.isValid() || !pcModel)
        return;

    const QString path = normalizedLocalPath(
            pcModel->filePath(index.sibling(index.row(), 0)));
    const QFileInfo info(path);

    if (path.isEmpty() || !info.isFile() || !isDiscImagePath(path)) {
        statusBar()->showMessage(
                "Only supported PS2 disc images can be added to the transfer queue.");
        return;
    }

    if (markedPcPaths.contains(path)) {
        markedPcPaths.remove(path);
        queuedPcBanks.remove(path);
    } else {
        if (!gameBankCombo ||
                gameBankCombo->currentIndex() < 0) {
            statusBar()->showMessage(
                    "Choose AUTO or a destination bank before adding a game to the queue.");
            return;
        }

        const int bank = gameBankCombo->currentData().toInt();
        markedPcPaths.insert(path);
        queuedPcBanks.insert(path, bank);
    }

    if (pcView) {
        pcView->viewport()->update();
        pcView->update();
    }
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
        if (copyToPs2Button)
            copyToPs2Button->setText(
                    "F5  Install selected game(s)  →  PS2 HDD");

        statusBar()->showMessage(
                "Transfer queue empty. AUTO is recommended; right-click or Add Selected to Queue.");
        return;
    }

    QMap<int, int> perBank;
    int autoCount = 0;
    for (const QString &path : markedPcPaths) {
        const int bank =
                queuedPcBanks.value(path, -1);
        if (bank < 0)
            ++autoCount;
        else
            perBank[bank] += 1;
    }

    QStringList bankSummary;
    if (autoCount > 0)
        bankSummary <<
                QString("AUTO: %1").arg(autoCount);
    for (auto it = perBank.cbegin(); it != perBank.cend(); ++it)
        bankSummary << QString("Bank %1: %2")
                .arg(it.key())
                .arg(it.value());

    if (copyToPs2Button)
        copyToPs2Button->setText(
                QString("F5  Install queue (%1 game(s))  →  PS2 HDD")
                        .arg(markedPcPaths.size()));

    statusBar()->showMessage(
            QString("%1 game(s) queued — %2")
                    .arg(markedPcPaths.size())
                    .arg(bankSummary.join("  |  ")));
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
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setMinimumWidth(760);
    progress.resize(820, 220);

    QStringList localFiles;
    int downloaded = 0, cached = 0, optionalScreenshotsMissing = 0, otherUnavailable = 0;
    int step = 0;
    QStringList unavailableExamples;
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
                const bool optionalScreenshot404 = type.startsWith("SCR") && error == "HTTP 404";
                if (optionalScreenshot404) {
                    optionalScreenshotsMissing++;
                } else {
                    otherUnavailable++;
                    if (unavailableExamples.size() < 8)
                        unavailableExamples << QString("%1 %2 (%3)").arg(game.gameId, type, error);
                }
            }
        }
    }
    progress.setValue(totalRequests);
    progress.close();

    if (localFiles.isEmpty()) {
        QString message = "No requested artwork files were available for the selected games.";
        if (optionalScreenshotsMissing)
            message += QString("\n\nOptional screenshots not present in the artwork database: %1").arg(optionalScreenshotsMissing);
        if (otherUnavailable)
            message += QString("\nOther artwork unavailable/download errors: %1").arg(otherUnavailable);
        QMessageBox::information(this, "No artwork found", message);
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
    QProgressDialog writeProgress(QString("Writing %1 artwork file(s) directly into %2...\n\nThe download/cache stage is complete; this stage writes the prepared batch to the PS2 HDD.")
            .arg(fileCount).arg(artPath), QString(), 0, 0, this);
    writeProgress.setWindowTitle("Installing OPL Artwork");
    writeProgress.setWindowModality(Qt::WindowModal);
    writeProgress.setMinimumDuration(0);
    writeProgress.setCancelButton(nullptr);
    writeProgress.setMinimumWidth(760);
    writeProgress.resize(820, 220);
    writeProgress.show();
    QApplication::processEvents();
    statusLabel->setText("Installing artwork directly into OPL PFS storage...");
    QString output;
    const bool ok = runPrivilegedWriter(disk,
            { "--pfsshell", pfsshellPath, "--copy-manifest", manifestPath, "--partition", currentOplPartition }, &output);
    writeProgress.close();
    QFile::remove(manifestPath);
    resetProgress();
    if (!ok) {
        QMessageBox::critical(this, "Artwork PFS copy failed", output.right(10000));
        return;
    }

    // The managed OPL preset already enables cover art. The initial alpha also asks the
    // writer to patch only enable_coverart on existing configs, preserving
    // every other user setting.
    QString configOutput;
    const bool configOk = runPrivilegedWriter(disk,
            { "--pfsshell", pfsshellPath, "--ensure-cover-art", "--partition", currentOplPartition,
              "--pfs-path", currentOplBase }, &configOutput);

    refreshCurrentPs2Tree();
    QString summary = QString("Artwork installed: %1 file(s)\nDownloaded now: %2\nReused from cache: %3")
            .arg(fileCount).arg(downloaded).arg(cached);
    if (optionalScreenshotsMissing)
        summary += QString("\nOptional screenshots not in database: %1").arg(optionalScreenshotsMissing);
    if (otherUnavailable)
        summary += QString("\nOther artwork unavailable/download errors: %1").arg(otherUnavailable);
    if (!configOk)
        summary += "\n\nArtwork files are installed, but enable_coverart could not be patched automatically:\n" + configOutput.right(1200);
    if (!unavailableExamples.isEmpty())
        summary += "\n\nExamples requiring attention:\n" + unavailableExamples.join('\n');
    statusLabel->setText(QString("Installed %1 OPL artwork file(s) directly from the local cache/database provider.").arg(fileCount));
    QMessageBox::information(this, "OPL artwork installed", summary);
#else
    QMessageBox::information(this, "Unavailable", "Direct PS2 HDD artwork installation is currently Fedora/Linux only.");
#endif
}

void MainWindow::resetProgress()
{
    if (transferProgress) {
        transferProgress->setRange(0, 100);
        transferProgress->setValue(0);
        transferProgress->setFormat("%p%");
        transferProgress->setVisible(false);
    }
    if (progressDetail) {
        progressDetail->clear();
        progressDetail->setVisible(false);
    }
    if (overallProgress) {
        overallProgress->setRange(0, 100);
        overallProgress->setValue(0);
        overallProgress->setFormat("%p%");
        overallProgress->setProperty("queueTotalBytes", QVariant());
        overallProgress->setProperty("queueCompletedBytes", QVariant());
        overallProgress->setProperty("queueCurrentBytes", QVariant());
        overallProgress->setProperty("queueGameIndex", QVariant());
        overallProgress->setProperty("queueGameCount", QVariant());
        overallProgress->setVisible(false);
    }
    if (overallProgressDetail) {
        overallProgressDetail->clear();
        overallProgressDetail->setVisible(false);
    }
}

void MainWindow::parseTransferProgress(const QString &text, const QString &prefix)
{
    static const QRegularExpression re(
            R"((\d{1,3})%,\s*([^,\r\n]+)\s+remaining,\s*([0-9.]+)\s+MB/sec)");
    auto matches = re.globalMatch(text);
    QRegularExpressionMatch last;
    while (matches.hasNext())
        last = matches.next();
    if (!last.hasMatch())
        return;

    const int currentPc = std::clamp(last.captured(1).toInt(), 0, 100);

    if (transferProgress) {
        transferProgress->setRange(0, 100);
        transferProgress->setValue(currentPc);
        transferProgress->setFormat(QString("Current game %1%").arg(currentPc));
        transferProgress->setVisible(true);
    }
    if (progressDetail) {
        progressDetail->setText(
                QString("%1 — %2% — %3 remaining — %4 MB/s")
                        .arg(prefix.isEmpty() ? QString("Current game") : prefix)
                        .arg(currentPc)
                        .arg(last.captured(2).trimmed())
                        .arg(last.captured(3)));
        progressDetail->setVisible(true);
    }

    if (overallProgress && overallProgressDetail) {
        const qulonglong totalBytes =
                overallProgress->property("queueTotalBytes").toULongLong();
        const qulonglong completedBytes =
                overallProgress->property("queueCompletedBytes").toULongLong();
        const qulonglong currentBytes =
                overallProgress->property("queueCurrentBytes").toULongLong();
        const int gameIndex =
                overallProgress->property("queueGameIndex").toInt();
        const int gameCount =
                overallProgress->property("queueGameCount").toInt();

        int overallPc = 0;
        if (totalBytes > 0) {
            const qulonglong weighted =
                    completedBytes * 100ULL +
                    currentBytes * static_cast<qulonglong>(currentPc);
            overallPc = static_cast<int>(
                    std::min<qulonglong>(100ULL, weighted / totalBytes));
        }

        overallProgress->setRange(0, 100);
        overallProgress->setValue(overallPc);
        overallProgress->setFormat(QString("Overall %1%").arg(overallPc));
        overallProgress->setVisible(true);

        overallProgressDetail->setText(
                QString("OVERALL — %1% — game %2 of %3")
                        .arg(overallPc)
                        .arg(gameIndex)
                        .arg(gameCount));
        overallProgressDetail->setVisible(true);
    }
}

void MainWindow::installGameFiles(const QStringList &inputPaths)
{
#ifdef __linux__
    if (!unlockHddSession(true))
        return;

    QString reason;
    if (!selectedDiskCanInstallGames(&reason)) {
        QMessageBox::warning(this, "HDL game install unavailable", reason);
        return;
    }

    if (!gameBankCombo ||
            gameBankCombo->currentIndex() < 0) {
        QMessageBox::warning(
                this,
                "Choose a destination",
                "Choose AUTO or an explicit bank before starting the queue.");
        return;
    }

    struct Game {
        QString path;
        QString key;
        QString name;
        QString media;
        int bank = 0;
        qulonglong bytes = 0;
    };

    const int fallbackBank = gameBankCombo->currentData().toInt();

    auto bankLabel = [&](int bank) -> QString {
        const int comboIndex = gameBankCombo ? gameBankCombo->findData(bank) : -1;
        if (comboIndex >= 0)
            return gameBankCombo->itemText(comboIndex);
        return QString("Bank %1").arg(bank);
    };

    auto makeGame = [&](const QString &path, int bank, Game *out, QString *error) -> bool {
        const QFileInfo info(path);
        if (!info.isFile() || !isDiscImagePath(path)) {
            if (error) *error = "Not a supported disc-image path.";
            return false;
        }

        QString media, probe;
        if (!probeGameImage(path, &media, &probe)) {
            if (error) *error = probe.right(1200);
            return false;
        }

        QString name = info.completeBaseName().trimmed();
        if (name.isEmpty())
            name = "PS2 Game";
        if (name.size() > 159)
            name.truncate(159);

        out->path = info.absoluteFilePath();
        out->key = normalizedLocalPath(out->path);
        out->name = name;
        out->media = media;
        out->bank = bank;
        out->bytes = static_cast<qulonglong>(info.size());
        return !out->key.isEmpty();
    };

    std::vector<Game> games;
    QSet<QString> knownQueued;
    QStringList failedResults;
    QStringList retriedResults;
    QStringList verificationWarnings;

    setCursor(Qt::WaitCursor);
    for (const QString &path : inputPaths) {
        const QString key = normalizedLocalPath(path);
        if (key.isEmpty() || knownQueued.contains(key))
            continue;

        const int bank = queuedPcBanks.value(key, fallbackBank);

        Game game;
        QString error;
        if (!makeGame(path, bank, &game, &error))
            continue;

        knownQueued.insert(game.key);
        games.push_back(game);
    }
    unsetCursor();

    if (games.empty()) {
        QMessageBox::information(
                this, "No PS2 images",
                "Select or queue one or more supported PS2 disc images.");
        return;
    }

    QMap<int, int> initialBankCounts;
    for (const Game &game : games)
        initialBankCounts[game.bank] += 1;

    QStringList initialDestinations;
    for (auto it = initialBankCounts.cbegin(); it != initialBankCounts.cend(); ++it)
        initialDestinations << QString("%1 — %2 game(s)")
                .arg(bankLabel(it.key()))
                .arg(it.value());

    const auto &disk =
            disks[static_cast<std::size_t>(diskCombo->currentIndex())];

    if (QMessageBox::question(
                this,
                "Install queued games to PS2 HDD",
                QString("Install %1 queued game(s) as HDL APA partitions?\n\n"
                        "%2\n\nDestinations:\n%3\n\n"
                        "You can change the bank selector and add more games "
                        "while this queue is running.")
                        .arg(static_cast<qulonglong>(games.size()))
                        .arg(QString::fromStdString(disk.devicePath))
                        .arg(initialDestinations.join('\n')),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes)
        return;

    qulonglong totalBytes = 0;
    for (const Game &game : games)
        totalBytes += game.bytes;

    qulonglong completedBytes = 0;

    copyToPs2Button->setEnabled(false);

    overallProgress->setRange(0, 100);
    overallProgress->setValue(0);
    overallProgress->setFormat("Overall 0%");
    overallProgress->setProperty("queueTotalBytes", totalBytes);
    overallProgress->setProperty("queueCompletedBytes", 0);
    overallProgress->setProperty("queueCurrentBytes", 0);
    overallProgress->setProperty("queueGameIndex", 0);
    overallProgress->setProperty("queueGameCount", static_cast<int>(games.size()));
    overallProgress->setVisible(true);

    overallProgressDetail->setText(
            QString("OVERALL — 0% — 0 of %1 games")
                    .arg(static_cast<qulonglong>(games.size())));
    overallProgressDetail->setVisible(true);

    transferProgress->setRange(0, 100);
    transferProgress->setValue(0);
    transferProgress->setFormat("Current game 0%");
    transferProgress->setVisible(true);
    progressDetail->setVisible(true);

    QStringList installedResults;

    auto scanBank = [&](int bank, QString *scanOutput) -> bool {
        QString local;
        const QStringList scanArgs = {
            "--hdl-dump", hdlDumpPath,
            "--list-games",
            "--bank", QString::number(bank)
        };
        const bool ok = runPrivilegedWriter(disk, scanArgs, &local);
        if (scanOutput)
            *scanOutput = local;
        return ok;
    };

    auto scanContainsGame =
            [&](const QString &wanted,
                    int bank,
                    QString *scanOutput,
                    bool *scanSucceeded) -> bool {
        QString local;
        const bool ok = scanBank(bank, &local);

        if (scanSucceeded)
            *scanSucceeded = ok;
        if (scanOutput)
            *scanOutput = local;

        if (!ok)
            return false;

        for (const QString &line :
                local.split('\n', Qt::SkipEmptyParts)) {
            if (!line.startsWith("GAME\t"))
                continue;

            const QStringList fields =
                    line.split('\t');

            if (!fields.isEmpty() &&
                    cleanMachineField(
                        fields.last()).compare(
                            wanted,
                            Qt::CaseInsensitive) == 0)
                return true;
        }

        return false;
    };

    struct BankFit
    {
        bool fits = false;
        int total = 0;
        int used = 0;
        int free = 0;
        int required = 0;
        int largest = 0;
        int runs = 0;
    };

    auto queryBankFit =
            [&](int bank,
                    qulonglong bytes,
                    BankFit *fit,
                    QString *error) -> bool {
        QString local;

        const QStringList args = {
            "--bank-space",
            "--required-bytes",
            QString::number(bytes),
            "--bank",
            QString::number(bank)
        };

        if (!runPrivilegedWriter(
                    disk,
                    args,
                    &local)) {
            if (error)
                *error = local;
            return false;
        }

        for (const QString &line :
                local.split(
                    '\n',
                    Qt::SkipEmptyParts)) {
            if (!line.startsWith("SPACE\t"))
                continue;

            const QStringList fields =
                    line.split('\t');

            if (fields.size() < 9)
                continue;

            if (fit) {
                fit->total =
                        cleanMachineField(
                            fields[2]).toInt();
                fit->used =
                        cleanMachineField(
                            fields[3]).toInt();
                fit->free =
                        cleanMachineField(
                            fields[4]).toInt();
                fit->required =
                        cleanMachineField(
                            fields[5]).toInt();
                fit->largest =
                        cleanMachineField(
                            fields[6]).toInt();
                fit->runs =
                        cleanMachineField(
                            fields[7]).toInt();
                fit->fits =
                        cleanMachineField(
                            fields[8]).toInt() != 0;
            }

            return true;
        }

        if (error)
            *error =
                    "Native bank-space query returned no SPACE record.\n" +
                    local.right(1400);

        return false;
    };

    const auto bankPlan =
            Ps2::HddLayoutPlanner::Plan(
                disk.size,
                Ps2::HddLayoutMode::ExtendedApaBanks);

    const int bankCount =
            bankPlan.valid
                ? static_cast<int>(
                    bankPlan.banks.size())
                : 0;

    int autoFloorBank = 0;
    bool queueAborted = false;
    QString queueAbortReason;

    auto appendLiveQueue = [&]() {
        QApplication::processEvents();

        QStringList marked = markedPcPaths.values();
        marked.sort(Qt::CaseInsensitive);

        int added = 0;
        for (const QString &path : marked) {
            const QString key = normalizedLocalPath(path);
            if (key.isEmpty() || knownQueued.contains(key))
                continue;

            const int bank = queuedPcBanks.value(key, fallbackBank);

            Game game;
            QString error;
            if (!makeGame(path, bank, &game, &error)) {
                knownQueued.insert(key);
                failedResults << QFileInfo(path).fileName()
                        + " — could not be added to live queue: " + error;
                continue;
            }

            knownQueued.insert(game.key);
            games.push_back(game);
            totalBytes += game.bytes;
            ++added;
        }

        if (added > 0) {
            overallProgress->setProperty("queueTotalBytes", totalBytes);
            overallProgress->setProperty(
                    "queueGameCount", static_cast<int>(games.size()));
            statusBar()->showMessage(
                    QString("%1 new game(s) appended; %2 total in this run.")
                            .arg(added)
                            .arg(static_cast<qulonglong>(games.size())));
        }
    };

    auto finishQueueItem = [&](const Game &game, std::size_t index) {
        completedBytes += game.bytes;

        overallProgress->setProperty("queueCompletedBytes", completedBytes);
        overallProgress->setProperty("queueCurrentBytes", 0);
        overallProgress->setProperty("queueGameIndex", static_cast<int>(index + 1));
        overallProgress->setProperty("queueGameCount", static_cast<int>(games.size()));

        const int overallPc = totalBytes
                ? static_cast<int>(
                        std::min<qulonglong>(
                                100ULL,
                                (completedBytes * 100ULL) / totalBytes))
                : 100;

        overallProgress->setValue(overallPc);
        overallProgress->setFormat(QString("Overall %1%").arg(overallPc));
        overallProgressDetail->setText(
                QString("OVERALL — %1% — %2 of %3 games processed")
                        .arg(overallPc)
                        .arg(index + 1)
                        .arg(static_cast<qulonglong>(games.size())));
        QApplication::processEvents();
    };

    for (std::size_t i = 0; i < games.size(); ++i) {
        appendLiveQueue();

        Game &game = games[i];

        if (markedPcPaths.contains(game.key) &&
                queuedPcBanks.contains(game.key))
            game.bank =
                    queuedPcBanks.value(
                        game.key,
                        game.bank);

        if (game.bank < 0) {
            bool resolved = false;
            QString spaceSummary;

            for (int bank = autoFloorBank;
                    bank < bankCount;
                    ++bank) {
                BankFit fit;
                QString error;

                statusLabel->setText(
                        QString(
                            "AUTO: checking Bank %1 space for %2...")
                            .arg(bank)
                            .arg(game.name));

                QApplication::processEvents();

                if (!queryBankFit(
                            bank,
                            game.bytes,
                            &fit,
                            &error)) {
                    queueAborted = true;
                    queueAbortReason =
                            QString(
                                "AUTO bank-space verification failed for Bank %1 before %2:\n%3")
                                .arg(bank)
                                .arg(game.name)
                                .arg(error.right(1800));
                    break;
                }

                spaceSummary +=
                        QString(
                            "Bank %1: %2 free chunk(s), %3 required. ")
                            .arg(bank)
                            .arg(fit.free)
                            .arg(fit.required);

                if (fit.fits) {
                    game.bank = bank;
                    resolved = true;
                    break;
                }

                // AUTO is deliberately sequential. Once the next game no
                // longer fits, later AUTO games move forward too rather than
                // fragmenting earlier banks with smaller titles.
                autoFloorBank = bank + 1;

                const int comboIndex =
                        gameBankCombo
                            ? gameBankCombo->findData(bank)
                            : -1;

                if (comboIndex >= 0)
                    gameBankCombo->setItemText(
                            comboIndex,
                            QString(
                                "Bank %1 — FULL / AUTO moved on")
                                .arg(bank));
            }

            if (queueAborted)
                break;

            if (!resolved) {
                failedResults <<
                        game.name +
                        " — AUTO: no APA bank can fit this image. " +
                        spaceSummary.trimmed();

                finishQueueItem(game, i);
                continue;
            }
        }

        const QString destination =
                bankLabel(game.bank);

        overallProgress->setProperty("queueTotalBytes", totalBytes);
        overallProgress->setProperty("queueCompletedBytes", completedBytes);
        overallProgress->setProperty("queueCurrentBytes", game.bytes);
        overallProgress->setProperty("queueGameIndex", static_cast<int>(i + 1));
        overallProgress->setProperty("queueGameCount", static_cast<int>(games.size()));

        transferProgress->setRange(0, 100);
        transferProgress->setValue(0);
        transferProgress->setFormat("Current game 0%");

        const QString prefix =
                QString("Game %1 of %2: %3 — %4")
                        .arg(i + 1)
                        .arg(static_cast<qulonglong>(games.size()))
                        .arg(game.name)
                        .arg(destination);

        progressDetail->setText(prefix);
        progressDetail->setVisible(true);

        statusLabel->setText(
                QString("Re-scanning Bank %1 before %2...")
                        .arg(game.bank)
                        .arg(game.name));
        QApplication::processEvents();

        QString preScan;
        if (!scanBank(game.bank, &preScan)) {
            queueAborted = true;
            queueAbortReason =
                    game.name +
                    QString(
                        " — Bank %1 pre-scan failed. No further writes were attempted:\n")
                        .arg(game.bank) +
                    preScan.right(1800).trimmed();

            failedResults << queueAbortReason;
            break;
        }

        const QStringList installArgs = {
            "--hdl-dump", hdlDumpPath,
            "--install-game", game.path,
            "--game-name", game.name,
            "--media", game.media,
            "--bank", QString::number(game.bank)
        };

        QString output;
        bool installed = runPrivilegedWriter(
                disk, installArgs, &output, true, prefix);
        bool usedRetry = false;

        if (!installed) {
            QString freshScan;
            bool freshScanOk = false;

            if (scanContainsGame(
                        game.name,
                        game.bank,
                        &freshScan,
                        &freshScanOk)) {
                installed = true;

                verificationWarnings <<
                        game.name +
                        QString(
                            " — writer reported an error, but a fresh Bank %1 scan confirms the title is present.")
                            .arg(game.bank);
            }
            else if (!freshScanOk) {
                queueAborted = true;

                queueAbortReason =
                        game.name +
                        QString(
                            " — install failed and the fresh Bank %1 verification scan also failed. No retry/no further writes:\n")
                            .arg(game.bank) +
                        freshScan.right(1800).trimmed();

                failedResults << queueAbortReason;
            }
            else {
                statusLabel->setText(
                        "Install attempt failed for " +
                        game.name +
                        ". Fresh destination-bank scan proves it is absent; retrying once...");

                QApplication::processEvents();

                usedRetry = true;
                output.clear();

                installed = runPrivilegedWriter(
                        disk,
                        installArgs,
                        &output,
                        true,
                        prefix + " — retry");
            }
        }

        if (queueAborted)
            break;

        if (installed) {
            QString verifyScan;
            bool verifyScanOk = false;

            if (scanContainsGame(
                        game.name,
                        game.bank,
                        &verifyScan,
                        &verifyScanOk)) {
                installedResults << QString("%1 — Bank %2")
                        .arg(game.name)
                        .arg(game.bank);

                if (usedRetry)
                    retriedResults << QString("%1 — Bank %2")
                            .arg(game.name)
                            .arg(game.bank);

                markedPcPaths.remove(game.key);
                queuedPcBanks.remove(game.key);
            }
            else if (!verifyScanOk) {
                queueAborted = true;

                queueAbortReason =
                        game.name +
                        QString(
                            " — writer completed, but the fresh Bank %1 verification scan failed. No further writes:\n")
                            .arg(game.bank) +
                        verifyScan.right(1800).trimmed();

                failedResults << queueAbortReason;
            }
            else {
                failedResults <<
                        game.name +
                        QString(
                            " — writer completed, but a fresh Bank %1 scan did not find the title.")
                            .arg(game.bank);
            }
        }
        else {
            failedResults << QString("%1 — Bank %2 — %3")
                    .arg(game.name)
                    .arg(game.bank)
                    .arg(output.right(900).trimmed());
        }

        transferProgress->setValue(100);
        transferProgress->setFormat("Current game 100%");

        appendLiveQueue();
        finishQueueItem(game, i);

        if (pcView) {
            pcView->viewport()->update();
            pcView->update();
        }

        updateMarkedStatus();
        QApplication::processEvents();

        if (queueAborted)
            break;
    }

    copyToPs2Button->setEnabled(
            selectedDiskCanInstallGames());

    if (!queueAborted) {
        overallProgress->setValue(100);
        overallProgress->setFormat("Overall 100%");

        overallProgressDetail->setText(
                QString(
                    "OVERALL — complete — %1 game(s) processed")
                    .arg(
                        static_cast<qulonglong>(
                            games.size())));

        statusLabel->setText(
                "HDL queue finished. Refreshing the game table once...");
    }
    else {
        overallProgressDetail->setText(
                "OVERALL — queue stopped safely after verification failure");

        statusLabel->setText(
                "Queue stopped safely. No further HDD writes were attempted after verification failed.");
    }
    QApplication::processEvents();
    refreshCurrentPs2Tree();

    QString summary =
            QString("Installed: %1 / %2")
                    .arg(installedResults.size())
                    .arg(static_cast<qulonglong>(games.size()));

    if (!installedResults.isEmpty())
        summary += "\n\nInstalled destinations:\n  "
                + installedResults.join("\n  ");

    if (!retriedResults.isEmpty())
        summary += "\n\nSucceeded on automatic retry:\n  "
                + retriedResults.join("\n  ");

    if (!failedResults.isEmpty())
        summary += "\n\nFailed (left queued when applicable):\n  "
                + failedResults.join("\n  ");

    if (!verificationWarnings.isEmpty())
        summary +=
                "\n\nVerification notes:\n  " +
                verificationWarnings.join("\n  ");

    if (queueAborted)
        summary +=
                "\n\nQUEUE STOPPED SAFELY: " +
                queueAbortReason +
                "\nRemaining marked games were left queued and were not written.";

    QMessageBox::information(
            this,
            failedResults.isEmpty()
                    ? "HDL game installation complete"
                    : "HDL game queue completed with errors",
            summary);

    resetProgress();
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
    auto *mca =
            new QCheckBox(
                "Memory Card Annihilator - latest automated build",
                &dialog);

    auto *fceumm =
            new QCheckBox(
                "FCEUmm-PS2 SMB - NES emulator + HDD/SMB ROM browser",
                &dialog);
    fceumm->setEnabled(managedFhdb);
    fceumm->setChecked(managedFhdb);
    fceumm->setToolTip(
            managedFhdb
                ? "Creates /OPL/ROMS/NES and preconfigures this managed HDD layout."
                : "Requires managed PP.FHDB.APPS so HDD paths can be preconfigured safely.");

    auto *enabler = new QCheckBox("FHDB HDD Boot Configuration - Status / Enable / Disable / Verify", &dialog);
    auto *preset = new QCheckBox("Apply PS2 HDD Manager recommended OPL settings (replace conf_opl.cfg)", &dialog);
    wle->setChecked(true);
    mca->setChecked(true);
    enabler->setChecked(true);
    preset->setChecked(true);
    layout->addWidget(opl);
    layout->addWidget(wle);
    layout->addWidget(mca);
    layout->addWidget(fceumm);
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
    if (mca->isChecked())
        fetchArgs << "--mca";
    if (fceumm->isChecked())
        fetchArgs << "--fceumm";
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
        prepared =
                addApp(
                    "FHDB-HDD-Boot-Config",
                    runtimePayloadPath +
                            "/fhdb-enabler/FHDB-Boot-Config.ELF",
                    "FHDB HDD Boot Configuration");

    if (prepared && fceumm->isChecked()) {
        addDirectory(
                joinPfsPath(
                    currentOplBase,
                    "ROMS"));
        addDirectory(
                joinPfsPath(
                    currentOplBase,
                    "ROMS/NES"));
        addDirectory(
                joinPfsPath(
                    currentOplBase,
                    "SAVES"));
        addDirectory(
                joinPfsPath(
                    currentOplBase,
                    "SAVES/FCEUMM"));

        prepared =
                addApp(
                    "FCEUmm-PS2-SMB",
                    runtimePayloadPath +
                            "/fceumm/BOOT.ELF",
                    "FCEUmm-PS2 SMB");

        if (prepared) {
            const QString appRoot =
                    joinPfsPath(
                        joinPfsPath(
                            currentOplBase,
                            "APPS"),
                        "FCEUmm-PS2-SMB");

            const QByteArray cnf =
                    QByteArray(
                        "# PS2 HDD Manager FCEUmm HDD preset\r\n"
                        "Rompath = hdd0:/PP.FHDB.APPS/OPL/ROMS/NES/\r\n"
                        "Savepath = hdd0:/PP.FHDB.APPS/OPL/SAVES/FCEUMM/\r\n"
                        "CNFpath = hdd0:/PP.FHDB.APPS/OPL/APPS/FCEUmm-PS2-SMB/\r\n"
                        "Elfpath = hdd0:/PP.FHDB.APPS/OPL/APPS/FCEUmm-PS2-SMB/BOOT.ELF\r\n");

            prepared =
                    addText(
                        cnf,
                        joinPfsPath(
                            appRoot,
                            "FCEUltra.cnf"));
        }
    }
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
    if (pcView && watched == pcView->viewport()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::RightButton)
                return true;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::RightButton) {
                const QModelIndex index = pcView->indexAt(mouse->position().toPoint());
                if (index.isValid()) {
                    pcView->setCurrentIndex(index.sibling(index.row(), 0));
                    toggleMarkedPcPath(index);
                }
                return true;
            }
        }
    }

    if (ps2View && watched == ps2View->viewport()) {
        if (event->type() == QEvent::DragEnter) {
            auto *drag = static_cast<QDragEnterEvent *>(event);
            if (!localUrls(drag->mimeData()).isEmpty()) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::DragMove) {
            auto *drag = static_cast<QDragMoveEvent *>(event);
            if (!localUrls(drag->mimeData()).isEmpty()) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto *drop = static_cast<QDropEvent *>(event);
            const QStringList paths = localUrls(drop->mimeData());
            if (paths.isEmpty())
                return false;

            QTreeWidgetItem *target = ps2View->itemAt(drop->position().toPoint());
            int kind = target ? target->data(0, KindRole).toInt() : NodeNone;
            if (kind == NodeGame && target->parent()) {
                target = target->parent();
                kind = target->data(0, KindRole).toInt();
            }

            const bool gameTarget = kind == NodeGamesRoot || kind == NodeGamesMedia;
            if (gameTarget) {
                drop->acceptProposedAction();
                installGameFiles(paths);
                return true;
            }

            if (kind == NodePfsDirectory || kind == NodePfsFile) {
                drop->acceptProposedAction();
                copyPcItemsToPfs(paths, target);
                return true;
            }

            if (std::all_of(paths.begin(), paths.end(), [](const QString &p) {
                    return QFileInfo(p).isFile() && isDiscImagePath(p);
                })) {
                drop->acceptProposedAction();
                installGameFiles(paths);
                return true;
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}
