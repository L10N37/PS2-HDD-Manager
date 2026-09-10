#include "MainWindow.h"
#include "DebugTrace.h"

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
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPointer>
#include <QPainter>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSet>
#include <QSplitter>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QWidget>
#include <QStyle>
#include <QPalette>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QTimer>
#include <QTextStream>
#include <QTextCursor>
#include <QTreeView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <limits>

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

// PS2_HDD_LIVE_GAMEID_DATABASE_V2
bool parseLiveGameIdDatabase(const QByteArray &data,
        QHash<QString, QString> *titles, QString *error)
{
    if (!titles)
        return false;

    titles->clear();
    int duplicateCount = 0;
    int lineNumber = 0;

    QTextStream stream(data);
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        ++lineNumber;

        while (!line.isEmpty() &&
               (line.endsWith(' ') || line.endsWith('\t') || line.endsWith(QChar('\0'))))
            line.chop(1);

        if (line.isEmpty())
            continue;

        if (line.size() < 13 || line.at(11) != QChar(' ')) {
            if (error)
                *error = QString("Current gameid.txt is malformed at line %1.")
                        .arg(lineNumber);
            titles->clear();
            return false;
        }

        const QString id = line.left(11).toUpper();
        QString title = line.mid(12).trimmed();
        title.replace("^!", "!");

        if (title.isEmpty()) {
            if (error)
                *error = QString("Current gameid.txt has an empty title for %1.")
                        .arg(id);
            titles->clear();
            return false;
        }

        if (titles->contains(id)) {
            ++duplicateCount;
            continue;
        }

        titles->insert(id, title);
    }

    if (titles->size() < 13000) {
        if (error)
            *error = QString("Current gameid.txt is unexpectedly small (%1 records).")
                    .arg(titles->size());
        titles->clear();
        return false;
    }

    if (duplicateCount != 0) {
        if (error)
            *error = QString("Current gameid.txt contains %1 duplicate Game ID(s).")
                    .arg(duplicateCount);
        titles->clear();
        return false;
    }

    if (error)
        error->clear();
    return true;
}

QString portableGameFilenameError(const QString &stem)
{
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");

    for (const QChar ch : stem) {
        if (ch.unicode() < 32 || forbidden.contains(ch))
            return "database title contains a character forbidden in a portable filename";
    }

    if (stem.isEmpty() || stem.endsWith(' ') || stem.endsWith('.'))
        return "database title ends with a character forbidden in a portable filename";

    const QString lower = stem.toLower();
    if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul")
        return "database title is a reserved Windows filename";

    static const QRegularExpression reservedPort(
            QStringLiteral("^(com|lpt)[1-9]$"),
            QRegularExpression::CaseInsensitiveOption);
    if (reservedPort.match(stem).hasMatch())
        return "database title is a reserved Windows filename";

    return QString();
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

// PS2_HDD_LEFT_GAME_ID_FOLDER_FAST_V4
quint32 readIsoLe32(const QByteArray &bytes, qsizetype offset)
{
    if (offset < 0 || offset + 4 > bytes.size())
        return 0;

    const auto *p = reinterpret_cast<const unsigned char *>(
            bytes.constData() + offset);

    return static_cast<quint32>(p[0]) |
            (static_cast<quint32>(p[1]) << 8U) |
            (static_cast<quint32>(p[2]) << 16U) |
            (static_cast<quint32>(p[3]) << 24U);
}

QString parsePs2SystemCnfGameId(const QByteArray &systemCnf)
{
    // PS2 discs use BOOT2. Requiring BOOT2 rather than BOOT keeps ordinary
    // ISO9660 images and PS1 discs from being misidentified.
    static const QRegularExpression boot2(
            QStringLiteral(
                "^\\s*BOOT2\\s*=\\s*cdrom0:\\\\+"
                "([A-Z]{4}_[0-9]{3}\\.[0-9]{2})(?:;1)?"),
            QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::MultilineOption);

    const QRegularExpressionMatch match =
            boot2.match(QString::fromLatin1(systemCnf));

    return match.hasMatch()
            ? match.captured(1).toUpper()
            : QString();
}

QString identifyIsoGameIdFast(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();

    // Reject tiny/random files before doing any ISO work.
    if (file.size() < (17LL * 2048LL))
        return QString();

    auto readAt = [&](quint64 offset, qsizetype amount) -> QByteArray {
        if (offset > static_cast<quint64>(
                    std::numeric_limits<qint64>::max()))
            return {};

        if (!file.seek(static_cast<qint64>(offset)))
            return {};

        const QByteArray bytes = file.read(amount);
        return bytes.size() == amount ? bytes : QByteArray();
    };

    // Cheap ISO9660 sanity check: one 2048-byte PVD read.
    const QByteArray pvd = readAt(16ULL * 2048ULL, 2048);
    if (pvd.size() != 2048 ||
            static_cast<unsigned char>(pvd[0]) != 1U ||
            pvd.mid(1, 5) != QByteArrayLiteral("CD001") ||
            static_cast<unsigned char>(pvd[6]) != 1U)
        return QString();

    const quint16 logicalBlockSize =
            static_cast<quint16>(
                static_cast<unsigned char>(pvd[128])) |
            (static_cast<quint16>(
                static_cast<unsigned char>(pvd[129])) << 8U);

    if (logicalBlockSize != 2048U)
        return QString();

    // ISO9660 root directory record lives in the PVD at byte 156.
    const quint32 rootSector = readIsoLe32(pvd, 156 + 2);
    const quint32 rootBytes = readIsoLe32(pvd, 156 + 10);
    if (rootSector == 0 || rootBytes == 0)
        return QString();

    // No huge negative scans: cap root-directory inspection at 64 KiB.
    const quint32 rootReadBytes =
            std::min<quint32>(rootBytes, 64U * 1024U);

    const QByteArray root = readAt(
            static_cast<quint64>(rootSector) * 2048ULL,
            static_cast<qsizetype>(rootReadBytes));
    if (root.isEmpty())
        return QString();

    qsizetype pos = 0;
    while (pos < root.size()) {
        const quint8 recordLength =
                static_cast<quint8>(
                    static_cast<unsigned char>(root[pos]));

        if (recordLength == 0) {
            // ISO9660 pads the rest of a directory sector with zeroes.
            const qsizetype nextSector =
                    ((pos / 2048) + 1) * 2048;
            if (nextSector <= pos)
                break;
            pos = nextSector;
            continue;
        }

        if (recordLength < 34 ||
                pos + recordLength > root.size())
            break;

        const quint8 nameLength =
                static_cast<quint8>(
                    static_cast<unsigned char>(root[pos + 32]));

        if (33 + nameLength <= recordLength) {
            QByteArray name =
                    root.mid(pos + 33, nameLength);

            const int versionSeparator = name.indexOf(';');
            if (versionSeparator >= 0)
                name.truncate(versionSeparator);

            if (name.compare(
                        QByteArrayLiteral("SYSTEM.CNF"),
                        Qt::CaseInsensitive) == 0) {
                const quint32 systemSector =
                        readIsoLe32(root, pos + 2);
                const quint32 systemBytes =
                        readIsoLe32(root, pos + 10);

                if (systemSector == 0 || systemBytes == 0)
                    return QString();

                const quint32 cnfReadBytes =
                        std::min<quint32>(
                            systemBytes,
                            4096U);

                const QByteArray systemCnf = readAt(
                        static_cast<quint64>(systemSector) * 2048ULL,
                        static_cast<qsizetype>(cnfReadBytes));

                return parsePs2SystemCnfGameId(systemCnf);
            }
        }

        pos += recordLength;
    }

    return QString();
}

QString identifyFolderGameIdFast(const QString &path)
{
    const QDir dir(path);

    // Extracted-disc folder: check the normal root SYSTEM.CNF names only.
    // This is deliberately not recursive.
    const QStringList cnfNames = {
        QStringLiteral("SYSTEM.CNF"),
        QStringLiteral("system.cnf"),
        QStringLiteral("System.cnf"),
        QStringLiteral("SYSTEM.CNF;1")
    };

    for (const QString &name : cnfNames) {
        QFile cnf(dir.filePath(name));
        if (!cnf.exists() || !cnf.open(QIODevice::ReadOnly))
            continue;

        const QString id =
                parsePs2SystemCnfGameId(cnf.read(4096));
        if (!id.isEmpty())
            return id;
    }

    // Common collection layout: one ISO directly inside a game folder.
    // Inspect at most 64 direct child files and never recurse.
    QDirIterator it(
            path,
            QDir::Files | QDir::NoDotAndDotDot,
            QDirIterator::NoIteratorFlags);

    int examined = 0;
    while (it.hasNext() && examined < 64) {
        it.next();
        ++examined;

        const QFileInfo child = it.fileInfo();
        if (child.suffix().compare(
                    QStringLiteral("iso"),
                    Qt::CaseInsensitive) != 0)
            continue;

        const QString id =
                identifyIsoGameIdFast(
                    child.absoluteFilePath());
        if (!id.isEmpty())
            return id;
    }

    return QString();
}

QString identifyPathGameIdFast(const QString &path)
{
    const QFileInfo info(path);

    if (info.isDir())
        return identifyFolderGameIdFast(path);

    if (!info.isFile() ||
            info.suffix().compare(
                QStringLiteral("iso"),
                Qt::CaseInsensitive) != 0)
        return QString();

    return identifyIsoGameIdFast(path);
}

class Ps2FileSystemModel final : public QFileSystemModel
{
public:
    explicit Ps2FileSystemModel(QObject *parent = nullptr)
        : QFileSystemModel(parent)
    {
    }

    int columnCount(
            const QModelIndex &parent = QModelIndex()) const override
    {
        return QFileSystemModel::columnCount(parent) + 1;
    }

    QVariant headerData(
            int section,
            Qt::Orientation orientation,
            int role = Qt::DisplayRole) const override
    {
        if (orientation == Qt::Horizontal &&
                role == Qt::DisplayRole &&
                section == 4)
            return QStringLiteral("Game ID");

        return QFileSystemModel::headerData(
                section,
                orientation,
                role);
    }

    QVariant data(
            const QModelIndex &index,
            int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.column() != 4)
            return QFileSystemModel::data(index, role);

        const QModelIndex nameIndex =
                index.sibling(index.row(), 0);
        const QString path =
                QDir::cleanPath(filePath(nameIndex));
        const QFileInfo info(path);

        // Probe directories as well as ISO files. Directories are cheap:
        // root SYSTEM.CNF first, then at most 64 direct child files looking
        // for an ISO. There is deliberately no recursive scan.
        const bool candidate =
                info.isDir() ||
                (info.isFile() &&
                 info.suffix().compare(
                     QStringLiteral("iso"),
                     Qt::CaseInsensitive) == 0);

        if (!candidate)
            return QVariant();

        if (role == Qt::ToolTipRole) {
            if (info.isDir())
                return QStringLiteral(
                        "Fast PS2 Game ID probe: root SYSTEM.CNF or one "
                        "direct child ISO only. No recursive folder scan.");
            return QStringLiteral(
                    "PS2 Game ID read directly from this ISO after a "
                    "small ISO9660 sanity check. Only small sectors are read.");
        }

        if (role != Qt::DisplayRole)
            return QVariant();

        const auto cached = gameIds.constFind(path);
        if (cached != gameIds.cend())
            return cached.value().isEmpty()
                    ? QStringLiteral("Not detected")
                    : cached.value();

        scheduleGameIdScan(path);
        return QStringLiteral("Scanning...");
    }

    void clearGameIdCache()
    {
        gameIds.clear();
        pending.clear();
        emit layoutChanged();
    }

private:
    void scheduleGameIdScan(const QString &path) const
    {
        if (pending.contains(path))
            return;

        pending.insert(path);

        QPointer<Ps2FileSystemModel> self(
                const_cast<Ps2FileSystemModel *>(this));

        QThreadPool::globalInstance()->start(
                [self, path]() {
            const QString id =
                    identifyPathGameIdFast(path);

            if (!self)
                return;

            QMetaObject::invokeMethod(
                    self,
                    [self, path, id]() {
                if (!self)
                    return;

                self->pending.remove(path);
                self->gameIds.insert(path, id);

                const QModelIndex sourceIndex =
                        self->index(path);
                if (!sourceIndex.isValid())
                    return;

                const QModelIndex idIndex =
                        self->index(
                            sourceIndex.row(),
                            4,
                            sourceIndex.parent());

                if (idIndex.isValid())
                    emit self->dataChanged(
                            idIndex,
                            idIndex,
                            { Qt::DisplayRole,
                              Qt::ToolTipRole });
            },
                    Qt::QueuedConnection);
        });
    }

    mutable QHash<QString, QString> gameIds;
    mutable QSet<QString> pending;
};
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    PS2_TRACE_SCOPE("MainWindow::MainWindow");
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

    renameOnTransferCheck = new QCheckBox(
            "Auto-rename source files using latest gameid.txt", central);
    {
        QSettings transferSettings("VajskiDs", "PS2-HDD-Manager");
        renameOnTransferCheck->setChecked(
                transferSettings.value("transfer/renameFromGameId", true).toBool());
    }
    renameOnTransferCheck->setToolTip(
            "At the start of every transfer queue, downloads the current "
            "gameid.txt from L10N37/PS2-ISO-Batch-Renamer- main. "
            "The database title is used for the HDL game name and the PC source "
            "file is renamed only after a successful install or verified exact skip. "
            "Existing destination files are never overwritten.");
    connect(renameOnTransferCheck, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings transferSettings("VajskiDs", "PS2-HDD-Manager");
        transferSettings.setValue("transfer/renameFromGameId", enabled);
    });
    commandBar->addWidget(renameOnTransferCheck);

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
        if (transferQueueRunning) {
            statusBar()->showMessage(
                    "PS2 HDD Setup is disabled while the game queue is running.");
            return;
        }
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
    pcModel = new Ps2FileSystemModel(group);
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
    for (int i = 1; i < 5; i++)
        pcView->header()->setSectionResizeMode(
                i,
                QHeaderView::ResizeToContents);
    pcView->setToolTip(
            "The Game ID column uses the PS2 Batch Renamer direct ISO algorithm. "
            "It intentionally scans ISO files only; CHD is not an OPL HDD game format.");
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
        if (auto *model =
                dynamic_cast<Ps2FileSystemModel *>(pcModel))
            model->clearGameIdCache();
        populatePcDrives();
        navigatePc(pcPath->text());
    });
    return group;
}

QWidget *MainWindow::buildPs2Pane()
{
    auto *group = new QGroupBox("PlayStation 2 HDD - live APA / HDL / PFS view", this);
    auto *layout = new QVBoxLayout(group);
    auto *toolbar = new QHBoxLayout();
    auto *refresh = new QPushButton("Refresh HDD", group);
    auto *renameGameButton =
            new QPushButton("Rename Game...", group);
    auto *fixTitlesButton =
            new QPushButton("Fix Installed Titles...", group);
    renameGameButton->setToolTip(
            "Rename one selected installed HDL game in place. "
            "The game data is not retransferred.");
    fixTitlesButton->setToolTip(
            "Download the current PS2 Batch Renamer gameid.txt and "
            "rename installed HDL titles in place by their Game IDs.");
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
    toolbar->addWidget(renameGameButton);
    toolbar->addWidget(fixTitlesButton);
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
    ps2View->setContextMenuPolicy(Qt::CustomContextMenu);
    ps2View->setColumnCount(5);
    ps2View->setHeaderLabels({ "Name", "Game ID / Type", "Media", "Size", "Bank" });
    ps2View->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ps2View->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ps2View->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ps2View->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    ps2View->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    layout->addWidget(ps2View, 1);

    connect(refresh, &QPushButton::clicked, this, [this]() {
        if (transferQueueRunning) {
            statusBar()->showMessage(
                    "Refresh HDD is disabled while the game queue is running.");
            return;
        }
        if (unlockHddSession(true))
            refreshCurrentPs2Tree();
    });
    connect(renameGameButton, &QPushButton::clicked,
            this, &MainWindow::renameSelectedInstalledGame);
    connect(fixTitlesButton, &QPushButton::clicked,
            this, [this]() {
        renameInstalledGamesFromLatestDatabase(true);
    });
    connect(addArtButton, &QPushButton::clicked, this, &MainWindow::addArtwork);
    connect(installAppsButton, &QPushButton::clicked, this, &MainWindow::installOrUpdateOplApps);
    connect(applyOplDefaultsButton, &QPushButton::clicked, this, &MainWindow::applyRecommendedOplDefaults);

    connect(ps2View, &QTreeWidget::customContextMenuRequested,
            this, [this](const QPoint &position) {
        QTreeWidgetItem *item =
                ps2View->itemAt(position);

        if (!item ||
                item->data(0, KindRole).toInt() != NodeGame)
            return;

        if (!item->isSelected()) {
            ps2View->clearSelection();
            item->setSelected(true);
            ps2View->setCurrentItem(item);
        }

        QMenu menu(ps2View);
        QAction *manual =
                menu.addAction("Rename installed game...");
        QAction *latest =
                menu.addAction("Rename selected from latest gameid.txt");

        QAction *chosen =
                menu.exec(
                    ps2View->viewport()->mapToGlobal(position));

        if (chosen == manual)
            renameSelectedInstalledGame();
        else if (chosen == latest)
            renameInstalledGamesFromLatestDatabase(false);
    });

    connect(ps2View, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (!item || item->data(0, KindRole).toInt() != NodePfsDirectory ||
                item->data(0, LoadedRole).toBool())
            return;

        // Do not nest PFS reads inside another raw-HDD operation/tree rebuild.
        // The item remains unloaded and can be expanded again after the current
        // operation completes.
        if (transferQueueRunning || hddOperationInProgress || ps2RefreshInProgress)
            return;

        const int index = diskCombo ? diskCombo->currentIndex() : -1;
        if (index >= 0 && static_cast<std::size_t>(index) < disks.size()) {
            const auto disk = disks[static_cast<std::size_t>(index)];
            populatePfsDirectory(
                    item,
                    item->data(0, PathRole).toString(),
                    disk);
        }
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
    PS2_TRACE_SCOPE("MainWindow::refreshDisks");
    DebugTrace::write(
            "refreshDisks: transferQueueRunning=" +
            std::to_string(transferQueueRunning) +
            " hddOperationInProgress=" +
            std::to_string(hddOperationInProgress) +
            " ps2RefreshInProgress=" +
            std::to_string(ps2RefreshInProgress));
    if (transferQueueRunning) {
        statusBar()->showMessage(
                "Disk rescan is disabled while the game queue is running. "
                "PC browsing and queue additions remain available.");
        return;
    }

    if (hddOperationInProgress || ps2RefreshInProgress) {
        diskRescanPending = true;
        DebugTrace::write(
                "refreshDisks: DEFERRED because a right-pane/HDD read is active");
        statusBar()->showMessage(
                "Disk rescan queued until the current PS2 HDD read finishes.");
        return;
    }

    QString previous;
    const int oldIndex = diskCombo ? diskCombo->currentIndex() : -1;
    if (oldIndex >= 0 && static_cast<std::size_t>(oldIndex) < disks.size())
        previous = QString::fromStdString(disks[static_cast<std::size_t>(oldIndex)].devicePath);

    diskCombo->blockSignals(true);
    diskCombo->clear();
    disks.clear();
    ++ps2TreeGeneration;
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
        if (disks.empty())
            statusLabel->setText("No supported physical disks were found.");
        statusBar()->showMessage("Disk scan complete. No disk was modified.");

        // Keep signals blocked while restoring the index. We explicitly call
        // selectDisk once below; otherwise currentIndexChanged plus the manual
        // call performs two complete HDD reads back-to-back.
        if (!disks.empty())
            diskCombo->setCurrentIndex(restore >= 0 ? restore : 0);

        diskCombo->blockSignals(false);

        if (!disks.empty())
            selectDisk(diskCombo->currentIndex());
    } catch (const std::exception &e) {
        diskCombo->blockSignals(false);
        statusLabel->setText(QString::fromLocal8Bit(e.what()));
        statusBar()->showMessage("Disk scan failed.");
    }
    if (copyToPs2Button) copyToPs2Button->setEnabled(selectedDiskCanInstallGames());
}

void MainWindow::selectDisk(int index)
{
    PS2_TRACE_SCOPE("MainWindow::selectDisk");
    DebugTrace::write(
            "selectDisk: index=" +
            std::to_string(index) +
            " disks.size=" +
            std::to_string(disks.size()) +
            " transferQueueRunning=" +
            std::to_string(transferQueueRunning));
    if (transferQueueRunning) {
        statusBar()->showMessage(
                "The target PS2 HDD is locked for this transfer queue.");
        return;
    }

    if (hddOperationInProgress || ps2RefreshInProgress) {
        ps2RefreshPending = true;
        DebugTrace::write(
                "selectDisk: DEFERRED because the previous right-pane read is active");
        statusBar()->showMessage(
                "Disk selection queued until the current PS2 HDD read finishes.");
        return;
    }

    resetProgress();
    currentOplPartition.clear();
    currentOplBase.clear();
    if (index < 0 || static_cast<std::size_t>(index) >= disks.size()) {
        ++ps2TreeGeneration;
        ps2View->clear();
        return;
    }
    const auto selectedDisk =
            disks[static_cast<std::size_t>(index)];
    populatePs2Tree(selectedDisk);
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
    PS2_TRACE_SCOPE("MainWindow::refreshCurrentPs2Tree");
    DebugTrace::write(
            "refreshCurrentPs2Tree: comboIndex=" +
            std::to_string(
                diskCombo ? diskCombo->currentIndex() : -1) +
            " disks.size=" +
            std::to_string(disks.size()) +
            " transfer=" +
            std::to_string(transferQueueRunning) +
            " hddOp=" +
            std::to_string(hddOperationInProgress) +
            " refreshInProgress=" +
            std::to_string(ps2RefreshInProgress) +
            " refreshPending=" +
            std::to_string(ps2RefreshPending));
    // A raw-HDD helper command and populatePs2Tree() are synchronous, but both
    // intentionally keep the GUI painting. Any refresh event arriving inside
    // that nested event loop must be deferred rather than recursively clearing
    // the live tree.
    if (transferQueueRunning || hddOperationInProgress || ps2RefreshInProgress) {
        ps2RefreshPending = true;
        return;
    }

    const int index =
            diskCombo ? diskCombo->currentIndex() : -1;

    if (index < 0 ||
            static_cast<std::size_t>(index) >= disks.size())
        return;

    const auto selectedDisk =
            disks[static_cast<std::size_t>(index)];

    populatePs2Tree(selectedDisk);
}

void MainWindow::populatePs2Tree(const Ps2::PhysicalDiskCandidate &disk)
{
    PS2_TRACE_SCOPE("MainWindow::populatePs2Tree");
    DebugTrace::write(
            "populatePs2Tree: device=" +
            disk.devicePath +
            " size=" +
            std::to_string(disk.size) +
            " transfer=" +
            std::to_string(transferQueueRunning) +
            " hddOp=" +
            std::to_string(hddOperationInProgress) +
            " refreshInProgress=" +
            std::to_string(ps2RefreshInProgress));
    if (transferQueueRunning || hddOperationInProgress || ps2RefreshInProgress) {
        ps2RefreshPending = true;
        return;
    }

    QScopedValueRollback<bool> refreshGuard(
            ps2RefreshInProgress,
            true);

    // The caller normally already passes a copy, but keep a local copy here as
    // the final guarantee that no event-driven disk-vector rebuild can dangle
    // the descriptor used by the game/PFS reads.
    const Ps2::PhysicalDiskCandidate diskCopy = disk;

    ++ps2TreeGeneration;
    DebugTrace::write(
            "populatePs2Tree: tree generation=" +
            std::to_string(ps2TreeGeneration));
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
    // Populated HDD scan in progress. hdl_dump does not expose a reliable
    // percentage for hdl_toc, so show a busy progress bar rather than
    // leaving the interface looking frozen on heavily populated disks.
    // KDE can render Qt's range(0, 0) busy bar as a solid/static fill.
    // Use a deliberately non-percentage activity sweep instead. The label
    // never claims this is completion percentage; it only shows that the
    // long populated-HDD scan is still alive.
    transferProgress->setRange(0, 100);
    transferProgress->setValue(0);
    transferProgress->setFormat("Scanning installed PS2 games: preparing...");
    transferProgress->setVisible(true);
    progressDetail->setText(
            "Scanning installed HDL games... heavily populated disks can take a while.");
    progressDetail->setVisible(true);
    statusLabel->setText("Scanning installed PS2 games...");
    QApplication::processEvents();

    DebugTrace::write("populatePs2Tree: BEGIN populateInstalledGames");
    populateInstalledGames(games, diskCopy);
    DebugTrace::write("populatePs2Tree: END populateInstalledGames");
    // v14b: PFS has no percentage denominator yet; hide the completed HDL bar.
    transferProgress->setVisible(false);
    progressDetail->setText(
            QString("HDL scan complete — %1. Scanning OPL/PFS storage...")
                    .arg(games->text(3)));
    transferProgress->setFormat("Scanning OPL/PFS storage...");
    statusLabel->setText("Scanning OPL/PFS storage...");
    QApplication::processEvents();

    auto *opl = new QTreeWidgetItem(root);
    opl->setText(0, "OPL Storage");
    opl->setText(1, "PFS");
    opl->setData(0, KindRole, NodePfsDirectory);
    // @opl asks the privileged backend to map OPL's logical storage root:
    // custom partitions use /OPL, while the canonical +OPL partition uses /.
    opl->setData(0, PathRole, "@opl");
    DebugTrace::write("populatePs2Tree: BEGIN populatePfsDirectory @opl");
    populatePfsDirectory(opl, "@opl", diskCopy, false);
    DebugTrace::write("populatePs2Tree: END populatePfsDirectory @opl");

    transferProgress->setRange(0, 100);
    transferProgress->setValue(100);
    transferProgress->setFormat("HDD scan complete");
    progressDetail->setText(
            QString("HDD scan complete — %1.")
                    .arg(games->text(3)));
    QApplication::processEvents();

    QTimer::singleShot(800, this, [this]() {
        if (!transferQueueRunning &&
                !hddOperationInProgress &&
                !ps2RefreshInProgress) {
            transferProgress->setVisible(false);
            progressDetail->setVisible(false);
        }
    });

    root->setExpanded(true);
    games->setExpanded(true);
    opl->setExpanded(true);
    statusLabel->setText(
            "Live PS2 HDD view refreshed. Drag ISO(s) to HDL Games or ART/CFG/APPS files and folders to OPL Storage.");

    // Coalesce every request that arrived while this population was alive.
    // A physical rescan supersedes a simple tree refresh.
    if (diskRescanPending) {
        diskRescanPending = false;
        ps2RefreshPending = false;
        DebugTrace::write(
                "populatePs2Tree: scheduling deferred physical disk rescan");
        QTimer::singleShot(
                0,
                this,
                &MainWindow::refreshDisks);
    }
    else if (ps2RefreshPending) {
        ps2RefreshPending = false;
        DebugTrace::write(
                "populatePs2Tree: scheduling deferred right-pane refresh");
        QTimer::singleShot(
                0,
                this,
                &MainWindow::refreshCurrentPs2Tree);
    }
}

bool MainWindow::runPrivilegedWriter(const Ps2::PhysicalDiskCandidate &disk,
        const QStringList &modeArguments, QString *output, bool parseProgress,
        const QString &progressPrefix,
        const std::function<void(const QString &)> &outputCallback)
{
    PS2_TRACE_SCOPE("MainWindow::runPrivilegedWriter");
    DebugTrace::write(
            "runPrivilegedWriter: device=" +
            disk.devicePath +
            " args=[" +
            modeArguments.join(" ").toStdString() +
            "] parseProgress=" +
            std::to_string(parseProgress) +
            " transfer=" +
            std::to_string(transferQueueRunning) +
            " hddOp=" +
            std::to_string(hddOperationInProgress));
#ifdef __linux__
    if (!privilegedSession || !privilegedSession->isUnlocked()) {
        if (output)
            *output =
                    "PS2 HDD access is locked. Authenticate once with the Unlock button.";
        return false;
    }

    if (hddOperationInProgress) {
        if (output)
            *output =
                    "A PS2 HDD operation is already active. "
                    "The nested operation was deferred/refused safely.";
        ps2RefreshPending = true;
        return false;
    }

    QScopedValueRollback<bool> operationGuard(
            hddOperationInProgress,
            true);

    QStringList args;
    args << "--device" << QString::fromStdString(disk.devicePath)
         << "--expected-size" << QString::number(disk.size);
    args << modeArguments;

    QString progressWindow;
    const bool ok =
            privilegedSession->run(
                    args,
                    output,
                    [this,
                     parseProgress,
                     progressPrefix,
                     outputCallback,
                     &progressWindow](
                            const QString &chunk) {
        if (parseProgress) {
            progressWindow += chunk;
            if (progressWindow.size() > 4096)
                progressWindow =
                        progressWindow.right(4096);
            parseTransferProgress(
                    progressWindow,
                    progressPrefix);
        }

        const QStringList lines =
                chunk.split(
                    '\n',
                    Qt::SkipEmptyParts);

        for (const QString &line : lines)
            if (line.startsWith("STAGE:"))
                statusLabel->setText(
                        line.mid(6).trimmed());

        if (outputCallback)
            outputCallback(chunk);
    });

    // If UI requests arrived during a standalone writer operation, service
    // exactly one after this stack unwinds. During a transfer queue the target
    // HDD is intentionally fixed, so nothing is scheduled until the queue ends.
    if (!transferQueueRunning &&
            diskRescanPending &&
            !ps2RefreshInProgress) {
        diskRescanPending = false;
        ps2RefreshPending = false;
        DebugTrace::write(
                "runPrivilegedWriter: scheduling deferred physical disk rescan");
        QTimer::singleShot(
                0,
                this,
                &MainWindow::refreshDisks);
    }
    else if (!transferQueueRunning &&
            ps2RefreshPending &&
            !ps2RefreshInProgress) {
        ps2RefreshPending = false;
        DebugTrace::write(
                "runPrivilegedWriter: scheduling deferred right-pane refresh");
        QTimer::singleShot(
                0,
                this,
                &MainWindow::refreshCurrentPs2Tree);
    }

    DebugTrace::write(
            std::string("runPrivilegedWriter: return ok=") +
            (ok ? "1" : "0") +
            " outputBytes=" +
            std::to_string(
                output ? output->toUtf8().size() : 0));
    return ok;
#else
    Q_UNUSED(disk); Q_UNUSED(modeArguments); Q_UNUSED(output); Q_UNUSED(parseProgress); Q_UNUSED(progressPrefix);
    return false;
#endif
}

void MainWindow::populateInstalledGames(QTreeWidgetItem *gamesRoot, const Ps2::PhysicalDiskCandidate &disk)
{
    PS2_TRACE_SCOPE("MainWindow::populateInstalledGames");
    DebugTrace::write(
            "populateInstalledGames: device=" +
            disk.devicePath);
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

    const quint64 treeGenerationBeforeRead =
            ps2TreeGeneration;

    QString output;
    QString scanProgressWindow;
    DebugTrace::write(
            "populateInstalledGames: BEGIN writer --list-games generation=" +
            std::to_string(treeGenerationBeforeRead));
    const bool gameListOk =
            runPrivilegedWriter(
            disk,
            { "--hdl-dump", hdlDumpPath, "--list-games" },
            &output,
            false,
            QString(),
            [&](const QString &chunk)
            {
                scanProgressWindow += chunk;

                int newline = -1;
                while ((newline = scanProgressWindow.indexOf('\n')) >= 0)
                {
                    const QString line = scanProgressWindow.left(newline).trimmed();
                    scanProgressWindow.remove(0, newline + 1);
                    const QStringList fields = line.split('\t');

                    if (fields.size() >= 5 && fields[0] == "APA_SCAN_PROGRESS")
                    {
                        bool bankOk=false, sliceOk=false, sectorOk=false, totalOk=false;
                        const int bank = fields[1].toInt(&bankOk);
                        const int slice = fields[2].toInt(&sliceOk);
                        const qulonglong sector = fields[3].toULongLong(&sectorOk);
                        const qulonglong total = fields[4].toULongLong(&totalOk);
                        if (bankOk && sliceOk && sectorOk && totalOk && total > 0)
                        {
                            const int value = static_cast<int>(
                                    std::min<qulonglong>(1000ULL,
                                        sector * 1000ULL / total));
                            transferProgress->setRange(0, 1000);
                            transferProgress->setValue(
                                    std::max(transferProgress->value(), value));
                            transferProgress->setFormat(
                                    QString("Bank %1 APA chain: %p%").arg(bank));
                            progressDetail->setText(
                                    QString("Scanning APA chain — Bank %1, slice %2: %3% through bank address space")
                                            .arg(bank).arg(slice).arg(value / 10));
                            transferProgress->setVisible(true);
                            QApplication::processEvents();
                        }
                        continue;
                    }

                    if (fields.size() >= 4 && fields[0] == "APA_SCAN_DONE")
                    {
                        bool bankOk=false, sliceOk=false, totalOk=false;
                        const int bank = fields[1].toInt(&bankOk);
                        const int slice = fields[2].toInt(&sliceOk);
                        fields[3].toULongLong(&totalOk);
                        if (bankOk && sliceOk && totalOk)
                        {
                            transferProgress->setRange(0, 1000);
                            transferProgress->setValue(1000);
                            transferProgress->setFormat(
                                    QString("Bank %1 APA chain: complete").arg(bank));
                            progressDetail->setText(
                                    QString("APA chain complete — Bank %1, slice %2. Reading game headers...")
                                            .arg(bank).arg(slice));
                            QApplication::processEvents();
                        }
                        continue;
                    }

                    if (fields.size() >= 3 && fields[0] == "HDL_SCAN_TOTAL")
                    {
                        bool bankOk=false, totalOk=false;
                        const int bank = fields[1].toInt(&bankOk);
                        const int total = fields[2].toInt(&totalOk);
                        if (bankOk && totalOk && total >= 0)
                        {
                            transferProgress->setRange(0, std::max(1, total));
                            transferProgress->setValue(0);
                            transferProgress->setFormat(
                                    QString("Bank %1 game headers: %p%").arg(bank));
                            progressDetail->setText(
                                    QString("Reading installed game headers — Bank %1: 0 / %2")
                                            .arg(bank).arg(total));
                            transferProgress->setVisible(true);
                            QApplication::processEvents();
                        }
                        continue;
                    }

                    if (fields.size() >= 4 && fields[0] == "HDL_SCAN_PROGRESS")
                    {
                        bool bankOk=false, doneOk=false, totalOk=false;
                        const int bank = fields[1].toInt(&bankOk);
                        const int done = fields[2].toInt(&doneOk);
                        const int total = fields[3].toInt(&totalOk);
                        if (!bankOk || !doneOk || !totalOk || total <= 0)
                            continue;

                        const int bounded = std::clamp(done, 0, total);
                        const int percent = bounded * 100 / total;
                        transferProgress->setRange(0, total);
                        transferProgress->setValue(bounded);
                        transferProgress->setFormat(
                                QString("Bank %1 game headers: %p%").arg(bank));
                        progressDetail->setText(
                                QString("Reading installed game headers — Bank %1: %2 / %3 (%4%)")
                                        .arg(bank).arg(bounded).arg(total).arg(percent));
                        statusLabel->setText(
                                QString("Scanning installed PS2 games — Bank %1: %2 / %3")
                                        .arg(bank).arg(bounded).arg(total));
                        transferProgress->setVisible(true);
                        QApplication::processEvents();
                    }
                }
            });
    DebugTrace::write(
            std::string("populateInstalledGames: END writer --list-games ok=") +
            (gameListOk ? "1" : "0") +
            " outputBytes=" +
            std::to_string(output.toUtf8().size()) +
            " generationNow=" +
            std::to_string(ps2TreeGeneration));

    if (treeGenerationBeforeRead != ps2TreeGeneration) {
        DebugTrace::write(
                "populateInstalledGames: ABORTING parse because tree generation changed");
        ps2RefreshPending = true;
        return;
    }

    if (!gameListOk) {
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
    DebugTrace::write(
            "populateInstalledGames: parsed game count=" +
            std::to_string(count));
    gamesRoot->setText(3, QString("%1 game(s)").arg(count));
    dvd->setExpanded(true); cd->setExpanded(true);
}

void MainWindow::populatePfsDirectory(QTreeWidgetItem *item, const QString &path,
        const Ps2::PhysicalDiskCandidate &disk, bool showErrors)
{
    PS2_TRACE_SCOPE("MainWindow::populatePfsDirectory");
    DebugTrace::write(
            "populatePfsDirectory: path=" +
            path.toStdString() +
            " device=" +
            disk.devicePath +
            " showErrors=" +
            std::to_string(showErrors));
    if (!item) return;
    QString reason;
    if (!selectedDiskCanBrowsePfs(&reason)) {
        while (item->childCount() > 0) delete item->takeChild(0);
        auto *note = new QTreeWidgetItem(item); note->setText(0, "(PFS unavailable)"); note->setText(1, reason);
        item->setData(0, LoadedRole, true);
        return;
    }

    const quint64 treeGenerationBeforeRead =
            ps2TreeGeneration;

    QString output;
    DebugTrace::write(
            "populatePfsDirectory: BEGIN writer --list-pfs path=" +
            path.toStdString() +
            " generation=" +
            std::to_string(treeGenerationBeforeRead));
    const bool pfsListOk =
            runPrivilegedWriter(
                disk,
                { "--pfsshell", pfsshellPath, "--list-pfs",
                  "--partition", "auto", "--pfs-path", path },
                &output);
    DebugTrace::write(
            std::string("populatePfsDirectory: END writer --list-pfs ok=") +
            (pfsListOk ? "1" : "0") +
            " outputBytes=" +
            std::to_string(output.toUtf8().size()) +
            " generationNow=" +
            std::to_string(ps2TreeGeneration));

    if (treeGenerationBeforeRead != ps2TreeGeneration) {
        DebugTrace::write(
                "populatePfsDirectory: ABORTING parse because tree generation changed");
        ps2RefreshPending = true;
        return;
    }

    if (!pfsListOk) {
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
    DebugTrace::write(
            "populatePfsDirectory: completed childCount=" +
            std::to_string(item->childCount()) +
            " partition=" +
            currentOplPartition.toStdString() +
            " base=" +
            currentOplBase.toStdString());
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

// PS2_HDD_INSTALLED_TITLE_RENAME_V3
bool MainWindow::fetchLatestGameTitleDatabase(
        QHash<QString, QString> *titles,
        QString *error)
{
    if (!titles) {
        if (error)
            *error = "Internal database target is null.";
        return false;
    }

    if (!network) {
        if (error)
            *error = "Qt network manager is unavailable.";
        return false;
    }

    const QUrl url(
            "https://raw.githubusercontent.com/L10N37/"
            "PS2-ISO-Batch-Renamer-/refs/heads/main/gameid.txt");

    QNetworkRequest request(url);
    request.setHeader(
            QNetworkRequest::UserAgentHeader,
            QString("PS2-HDD-Manager/%1")
                    .arg(PS2_HDD_APP_VERSION));
    request.setAttribute(
            QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = network->get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);

    connect(&timeout,
            &QTimer::timeout,
            reply,
            &QNetworkReply::abort);
    connect(reply,
            &QNetworkReply::finished,
            &loop,
            &QEventLoop::quit);

    timeout.start(30000);
    loop.exec();
    timeout.stop();

    const int status =
            reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute)
                .toInt();
    const auto networkError =
            reply->error();
    const QString networkErrorText =
            reply->errorString();
    const QByteArray data =
            reply->readAll();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError ||
            status != 200) {
        if (error) {
            *error = status
                    ? QString("HTTP %1").arg(status)
                    : networkErrorText;
        }
        return false;
    }

    return parseLiveGameIdDatabase(
            data,
            titles,
            error);
}

bool MainWindow::renameInstalledGameOnHdd(
        const InstalledGameRef &game,
        const QString &newName,
        QString *error)
{
#ifdef __linux__
    const int diskIndex =
            diskCombo
                ? diskCombo->currentIndex()
                : -1;

    if (diskIndex < 0 ||
            static_cast<std::size_t>(diskIndex) >=
                disks.size()) {
        if (error)
            *error = "No PS2 HDD is selected.";
        return false;
    }

    QString title =
            cleanMachineField(newName);

    if (title.isEmpty() ||
            title.size() > 159) {
        if (error)
            *error =
                    "HDL title must contain 1-159 characters.";
        return false;
    }

    if (title.startsWith('+') ||
            title.startsWith('*') ||
            title.startsWith('-') ||
            title.startsWith(
                "0x",
                Qt::CaseInsensitive)) {
        if (error)
            *error =
                    "This title begins with a token that hdl_dump "
                    "reserves for modify options.";
        return false;
    }

    const auto &disk =
            disks[static_cast<std::size_t>(diskIndex)];

    QString output;
    const bool ok =
            runPrivilegedWriter(
                disk,
                {
                    "--hdl-dump", hdlDumpPath,
                    "--rename-installed", game.name,
                    "--new-game-name", title,
                    "--startup-id", game.gameId,
                    "--bank", QString::number(game.bank)
                },
                &output);

    if (!ok) {
        if (error)
            *error = output.right(5000).trimmed();
        return false;
    }

    if (!output.contains("GAME_RENAMED\t")) {
        if (error)
            *error =
                    "The writer returned success without its "
                    "post-rename verification record.\n" +
                    output.right(3000);
        return false;
    }

    if (error)
        error->clear();
    return true;
#else
    Q_UNUSED(game);
    Q_UNUSED(newName);
    if (error)
        *error =
                "Installed-game renaming is currently Linux/Fedora only.";
    return false;
#endif
}

bool MainWindow::invalidateOplHddGameListCache(QString *error)
{
#ifdef __linux__
    // PS2_HDD_OPL_GAMELIST_CACHE_INVALIDATE_V1
    //
    // OPL caches hdl_game_info_t records in games.bin and may use that cache
    // on initial HDD-mode entry without rereading externally modified HDL
    // headers. Truncate games.bin after a PC-side title rename so OPL is
    // forced to rebuild the list from the real HDL metadata next time.
    const int diskIndex =
            diskCombo ? diskCombo->currentIndex() : -1;

    if (diskIndex < 0 ||
            static_cast<std::size_t>(diskIndex) >=
                disks.size()) {
        if (error)
            *error =
                    "The selected PS2 HDD disappeared before the OPL "
                    "game-list cache could be invalidated.";
        return false;
    }

    if (pfsshellPath.isEmpty() ||
            !QFileInfo::exists(pfsshellPath)) {
        if (error)
            *error =
                    "pfsshell is unavailable, so OPL games.bin could not "
                    "be invalidated.";
        return false;
    }

    if (currentOplPartition.isEmpty() ||
            currentOplBase.isEmpty()) {
        if (error)
            *error =
                    "The OPL PFS storage location is not currently known. "
                    "Use Refresh HDD, then retry Fix Installed Titles.";
        return false;
    }

    QTemporaryFile emptyCache(
            QDir::tempPath() +
            "/ps2-hdd-empty-games-XXXXXX.bin");
    emptyCache.setAutoRemove(true);

    if (!emptyCache.open() ||
            !emptyCache.resize(0) ||
            !emptyCache.flush()) {
        if (error)
            *error =
                    "Could not create the temporary empty OPL games.bin.";
        return false;
    }

    const QString remotePath =
            joinPfsPath(
                currentOplBase,
                "games.bin");

    QTemporaryFile manifest(
            QDir::tempPath() +
            "/ps2-hdd-opl-cache-XXXXXX.tsv");
    manifest.setAutoRemove(true);

    if (!manifest.open()) {
        if (error)
            *error =
                    "Could not create the OPL cache invalidation manifest.";
        return false;
    }

    const QByteArray row =
            ("F\t" +
             emptyCache.fileName() +
             "\t" +
             remotePath +
             "\n").toUtf8();

    if (manifest.write(row) != row.size() ||
            !manifest.flush()) {
        if (error)
            *error =
                    "Could not write the OPL cache invalidation manifest.";
        return false;
    }

    const auto &disk =
            disks[static_cast<std::size_t>(diskIndex)];

    QString output;
    const bool ok =
            runPrivilegedWriter(
                disk,
                {
                    "--pfsshell",
                        pfsshellPath,
                    "--copy-manifest",
                        manifest.fileName(),
                    "--partition",
                        currentOplPartition
                },
                &output);

    if (!ok) {
        if (error)
            *error =
                    "The HDL title change succeeded, but OPL games.bin "
                    "could not be invalidated.\n\n"
                    "Use Refresh in OPL before trusting displayed titles.\n\n" +
                    output.right(4000).trimmed();
        return false;
    }

    if (error)
        error->clear();

    return true;
#else
    if (error)
        *error =
                "OPL HDD game-list cache invalidation is currently "
                "Linux/Fedora only.";
    return false;
#endif
}

void MainWindow::renameSelectedInstalledGame()
{
#ifdef __linux__
    if (transferQueueRunning ||
            hddOperationInProgress ||
            ps2RefreshInProgress) {
        QMessageBox::information(
                this,
                "HDD operation busy",
                "Wait for the current PS2 HDD operation to finish.");
        return;
    }

    if (!unlockHddSession(true))
        return;

    QString reason;
    if (!selectedDiskCanInstallGames(&reason)) {
        QMessageBox::warning(
                this,
                "Installed-game rename unavailable",
                reason);
        return;
    }

    const auto selected =
            installedGames(true);

    if (selected.size() != 1) {
        QMessageBox::information(
                this,
                "Select one installed game",
                "Select exactly one installed HDL game to rename.");
        return;
    }

    const InstalledGameRef game =
            selected.front();

    bool accepted = false;
    QString title =
            QInputDialog::getText(
                this,
                "Rename installed HDL game",
                QString("%1\n%2 — Bank %3\n\nNew OPL/HDL title:")
                        .arg(
                            game.name,
                            game.gameId)
                        .arg(game.bank),
                QLineEdit::Normal,
                game.name,
                &accepted);

    if (!accepted)
        return;

    title = cleanMachineField(title);
    if (title == game.name)
        return;

    if (title.isEmpty() ||
            title.size() > 159) {
        QMessageBox::warning(
                this,
                "Invalid HDL title",
                "The title must contain 1-159 characters.");
        return;
    }

    if (QMessageBox::question(
                this,
                "Rename installed game?",
                QString("Rename in place on Bank %1?\n\n%2\n→\n%3\n\n"
                        "The game data is not retransferred.")
                        .arg(game.bank)
                        .arg(game.name, title),
                QMessageBox::Yes |
                    QMessageBox::Cancel,
                QMessageBox::Cancel) !=
            QMessageBox::Yes)
        return;

    statusLabel->setText(
            QString("Renaming %1 in place...")
                    .arg(game.name));
    QApplication::processEvents();

    QString error;
    if (!renameInstalledGameOnHdd(
                game,
                title,
                &error)) {
        QMessageBox::critical(
                this,
                "Installed-game rename failed",
                error);
        return;
    }

    QString oplCacheError;
    const bool oplCacheInvalidated =
            invalidateOplHddGameListCache(
                &oplCacheError);

    statusLabel->setText(
            QString("Renamed %1 → %2. Refreshing HDD game table...")
                    .arg(game.name, title));
    refreshCurrentPs2Tree();

    if (!oplCacheInvalidated) {
        QMessageBox::warning(
                this,
                "Title renamed - OPL cache warning",
                oplCacheError);
    }
#else
    QMessageBox::information(
            this,
            "Unavailable",
            "Installed-game renaming is currently Linux/Fedora only.");
#endif
}

void MainWindow::renameInstalledGamesFromLatestDatabase(
        bool allInstalled)
{
#ifdef __linux__
    if (transferQueueRunning ||
            hddOperationInProgress ||
            ps2RefreshInProgress) {
        QMessageBox::information(
                this,
                "HDD operation busy",
                "Wait for the current PS2 HDD operation to finish.");
        return;
    }

    if (!unlockHddSession(true))
        return;

    QString reason;
    if (!selectedDiskCanInstallGames(&reason)) {
        QMessageBox::warning(
                this,
                "Installed-title correction unavailable",
                reason);
        return;
    }

    const auto targets =
            installedGames(!allInstalled);

    if (targets.empty()) {
        QMessageBox::information(
                this,
                "No installed games selected",
                allInstalled
                    ? "No installed HDL games with Game IDs were found."
                    : "Select one or more installed HDL games first.");
        return;
    }

    statusLabel->setText(
            "Downloading current PS2 Batch Renamer gameid.txt...");
    QApplication::processEvents();

    QHash<QString, QString> database;
    QString databaseError;

    if (!fetchLatestGameTitleDatabase(
                &database,
                &databaseError)) {
        QMessageBox::critical(
                this,
                "Latest gameid.txt unavailable",
                databaseError);
        return;
    }

    struct RenamePlan
    {
        InstalledGameRef game;
        QString title;
    };

    std::vector<RenamePlan> plan;
    int missing = 0;
    int alreadyCorrect = 0;
    int tooLong = 0;

    for (const InstalledGameRef &game : targets) {
        const QString title =
                database.value(
                    game.gameId.toUpper());

        if (title.isEmpty()) {
            ++missing;
            continue;
        }

        if (title == game.name) {
            ++alreadyCorrect;
            continue;
        }

        if (title.size() > 159) {
            ++tooLong;
            continue;
        }

        plan.push_back(
                { game, title });
    }

    if (plan.empty()) {
        QString oplCacheError;
        const bool oplCacheInvalidated =
                invalidateOplHddGameListCache(
                    &oplCacheError);

        QString message =
                QString("No installed title needs changing.\n\n"
                        "Already correct: %1\n"
                        "Game IDs absent from current database: %2\n"
                        "Titles over HDL limit: %3")
                        .arg(alreadyCorrect)
                        .arg(missing)
                        .arg(tooLong);

        if (oplCacheInvalidated)
            message +=
                    "\n\nOPL games.bin was invalidated. "
                    "The next OPL HDD load will rebuild titles from "
                    "the real HDL headers.";
        else
            message +=
                    "\n\nOPL cache warning:\n" +
                    oplCacheError;

        QMessageBox::information(
                this,
                "Installed titles already current",
                message);
        return;
    }

    QStringList preview;
    const int previewCount =
            std::min<int>(
                12,
                static_cast<int>(plan.size()));

    for (int i = 0; i < previewCount; ++i) {
        preview <<
                QString("%1  →  %2  [%3 / Bank %4]")
                    .arg(
                        plan[static_cast<std::size_t>(i)].game.name,
                        plan[static_cast<std::size_t>(i)].title,
                        plan[static_cast<std::size_t>(i)].game.gameId)
                    .arg(
                        plan[static_cast<std::size_t>(i)].game.bank);
    }

    if (static_cast<int>(plan.size()) >
            previewCount)
        preview <<
                QString("... and %1 more")
                    .arg(
                        static_cast<int>(plan.size()) -
                        previewCount);

    if (QMessageBox::question(
                this,
                "Fix installed HDL titles?",
                QString("Latest live gameid.txt contains %1 Game IDs.\n\n"
                        "%2 installed title(s) will be renamed in place.\n"
                        "No game image data will be retransferred.\n\n%3")
                        .arg(database.size())
                        .arg(
                            static_cast<qulonglong>(
                                plan.size()))
                        .arg(preview.join('\n')),
                QMessageBox::Yes |
                    QMessageBox::Cancel,
                QMessageBox::Cancel) !=
            QMessageBox::Yes)
        return;

    // PS2_HDD_FAST_BATCH_RENAME_V1
    QSet<int> touchedBanks;
    for (const RenamePlan &entry : plan)
        touchedBanks.insert(entry.game.bank);

    QList<int> bankList = touchedBanks.values();
    std::sort(bankList.begin(), bankList.end());

    QProgressDialog progress(
            "Fast renaming installed HDL titles...",
            "Stop before next bank",
            0,
            bankList.size(),
            this);
    progress.setWindowTitle(
            "Fix Installed PS2 Game Titles");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setMinimumWidth(760);

    QStringList renamed;
    QStringList rawAlreadyCorrect;
    QStringList failures;
    int processedBanks = 0;

    const int diskIndex =
            diskCombo ? diskCombo->currentIndex() : -1;

    if (diskIndex < 0 ||
            static_cast<std::size_t>(diskIndex) >=
                disks.size()) {
        QMessageBox::critical(
                this,
                "Fast installed-title rename failed",
                "The selected PS2 HDD disappeared before the batch started.");
        return;
    }

    const auto &disk =
            disks[static_cast<std::size_t>(diskIndex)];

    for (const int bank : bankList) {
        if (progress.wasCanceled())
            break;

        int bankCount = 0;
        for (const RenamePlan &entry : plan)
            if (entry.game.bank == bank)
                ++bankCount;

        progress.setLabelText(
                QString("Bank %1: caching Game IDs + APA positions once, then applying %2 metadata rename(s)...")
                    .arg(bank)
                    .arg(bankCount));
        progress.setValue(processedBanks);
        QApplication::processEvents();

        QTemporaryFile manifest(
                QDir::tempPath() +
                "/ps2-hdd-fast-rename-XXXXXX.tsv");
        manifest.setAutoRemove(true);

        if (!manifest.open()) {
            failures <<
                    QString("Bank %1\nCould not create fast rename manifest.")
                        .arg(bank);
            break;
        }

        bool manifestOk = true;

        for (const RenamePlan &entry : plan) {
            if (entry.game.bank != bank)
                continue;

            const QString id =
                    cleanMachineField(entry.game.gameId);
            const QString oldName =
                    cleanMachineField(entry.game.name);
            const QString newName =
                    cleanMachineField(entry.title);

            const QByteArray line =
                    (id + '\t' +
                     oldName + '\t' +
                     newName + '\n').toUtf8();

            if (manifest.write(line) != line.size()) {
                manifestOk = false;
                failures <<
                        QString("Bank %1\nCould not write fast rename manifest.")
                            .arg(bank);
                break;
            }
        }

        if (!manifestOk)
            break;

        if (!manifest.flush()) {
            failures <<
                    QString("Bank %1\nCould not flush fast rename manifest.")
                        .arg(bank);
            break;
        }

        QString output;
        const bool ok =
                runPrivilegedWriter(
                    disk,
                    {
                        "--hdl-dump", hdlDumpPath,
                        "--rename-batch-manifest",
                            manifest.fileName(),
                        "--bank",
                            QString::number(bank)
                    },
                    &output);

        for (const QString &line :
                output.split('\n', Qt::SkipEmptyParts)) {
            const QStringList fields = line.split('\t');

            if (line.startsWith("GAME_ALREADY_CORRECT\t")) {
                if (fields.size() >= 4)
                    rawAlreadyCorrect <<
                            QString("%1 [%2 / Bank %3]")
                                .arg(
                                    cleanMachineField(fields[3]),
                                    cleanMachineField(fields[2]),
                                    cleanMachineField(fields[1]));
                continue;
            }

            if (!line.startsWith("GAME_RENAMED\t"))
                continue;

            if (fields.size() < 5)
                continue;

            renamed <<
                    QString("%1 → %2 [%3 / Bank %4]")
                        .arg(
                            cleanMachineField(fields[3]),
                            cleanMachineField(fields[4]),
                            cleanMachineField(fields[2]),
                            cleanMachineField(fields[1]));
        }

        if (!ok) {
            failures <<
                    QString("Bank %1\n%2")
                        .arg(bank)
                        .arg(output.right(9000).trimmed());
            break;
        }

        ++processedBanks;
        progress.setValue(processedBanks);
        QApplication::processEvents();
    }

    const bool cancelled =
            progress.wasCanceled();
    progress.close();

    QString oplCacheError;
    const bool oplCacheInvalidated =
            invalidateOplHddGameListCache(
                &oplCacheError);

    statusLabel->setText(
            "Installed-title pass finished. Refreshing HDD game table...");
    refreshCurrentPs2Tree();

    QString summary =
            QString("Renamed successfully: %1\n"
                    "Re-verified already correct from raw HDL header: %2\n"
                    "Already correct from initial table: %3\n"
                    "Missing from latest gameid.txt: %4\n"
                    "Over HDL title limit: %5")
                    .arg(renamed.size())
                    .arg(rawAlreadyCorrect.size())
                    .arg(alreadyCorrect)
                    .arg(missing)
                    .arg(tooLong);

    if (oplCacheInvalidated)
        summary +=
                "\nOPL game-list cache: invalidated "
                "(games.bin will rebuild from HDL headers)";
    else
        summary +=
                "\nOPL game-list cache: WARNING - " +
                oplCacheError;

    if (!renamed.isEmpty())
        summary +=
                "\n\nRenamed:\n  " +
                renamed.join("\n  ");

    if (!rawAlreadyCorrect.isEmpty())
        summary +=
                "\n\nRaw HDL header was already correct:\n  " +
                rawAlreadyCorrect.join("\n  ");

    if (!failures.isEmpty())
        summary +=
                "\n\nSTOPPED SAFELY after failure:\n  " +
                failures.join("\n  ");

    if (cancelled)
        summary +=
                "\n\nStopped by user before the next title. "
                "No in-progress metadata write was interrupted.";

    QDialog resultDialog(this);
    resultDialog.setWindowTitle(
            "Installed HDL title results");
    resultDialog.resize(860, 680);

    auto *layout =
            new QVBoxLayout(&resultDialog);
    auto *view =
            new QPlainTextEdit(&resultDialog);
    view->setReadOnly(true);
    view->setPlainText(summary);
    layout->addWidget(view, 1);

    auto *buttons =
            new QDialogButtonBox(
                QDialogButtonBox::Close,
                &resultDialog);
    connect(buttons,
            &QDialogButtonBox::rejected,
            &resultDialog,
            &QDialog::reject);
    layout->addWidget(buttons);
    resultDialog.exec();
#else
    Q_UNUSED(allInstalled);
    QMessageBox::information(
            this,
            "Unavailable",
            "Installed-game renaming is currently Linux/Fedora only.");
#endif
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
            const QString id =
                    item->text(1).trimmed().toUpper();
            const QRegularExpressionMatch bankMatch =
                    QRegularExpression(
                        QStringLiteral("^Bank\\s+(\\d+)$"))
                        .match(item->text(4).trimmed());
            const int bank =
                    bankMatch.hasMatch()
                        ? bankMatch.captured(1).toInt()
                        : 0;
            if (!id.isEmpty())
                result.push_back(
                        { item->text(0), id, bank });
        }
        for (int i = 0; i < item->childCount(); ++i)
            visit(item->child(i), thisSelected);
    };
    for (int i = 0; i < ps2View->topLevelItemCount(); ++i)
        visit(ps2View->topLevelItem(i), false);

    std::sort(result.begin(), result.end(), [](const InstalledGameRef &a, const InstalledGameRef &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    result.erase(
            std::unique(
                result.begin(),
                result.end(),
                [](const InstalledGameRef &a,
                        const InstalledGameRef &b) {
                    return a.gameId == b.gameId &&
                            a.bank == b.bank;
                }),
            result.end());
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
    if (transferQueueRunning) {
        statusBar()->showMessage("Artwork changes are disabled while the game queue is running.");
        return;
    }
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
    QProgressDialog writeProgress(
            QString("Writing %1 artwork file(s) directly into %2...\n\n"
                    "The download/cache stage is complete; this stage writes the prepared batch to the PS2 HDD.")
                    .arg(fileCount).arg(artPath),
            QString(), 0, 1000, this);
    writeProgress.setValue(0);
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
    QString pfsProgressWindow;

    const bool ok = runPrivilegedWriter(
            disk,
            { "--pfsshell", pfsshellPath, "--copy-manifest", manifestPath,
              "--partition", currentOplPartition },
            &output,
            false,
            QString(),
            [&](const QString &chunk) {
                pfsProgressWindow += chunk;

                int nl = -1;
                while ((nl = pfsProgressWindow.indexOf('\n')) >= 0)
                {
                    const QString line =
                            pfsProgressWindow.left(nl).trimmed();
                    pfsProgressWindow.remove(0, nl + 1);

                    if (!line.startsWith("PFS_PROGRESS\t"))
                        continue;

                    const QStringList f = line.split('\t');
                    if (f.size() < 5)
                        continue;

                    bool ok1=false, ok2=false, ok3=false, ok4=false;
                    const qulonglong filesDone = f[1].toULongLong(&ok1);
                    const qulonglong filesTotal = f[2].toULongLong(&ok2);
                    const qulonglong bytesDone = f[3].toULongLong(&ok3);
                    const qulonglong bytesTotal = f[4].toULongLong(&ok4);
                    if (!ok1 || !ok2 || !ok3 || !ok4)
                        continue;

                    const int value = bytesTotal
                            ? static_cast<int>(std::min<qulonglong>(
                                  1000ULL,
                                  bytesDone * 1000ULL / bytesTotal))
                            : 0;

                    writeProgress.setValue(value);
                    writeProgress.setLabelText(
                            QString("Writing artwork into %1...\n"
                                    "%2 / %3 files committed\n%4 / %5")
                                    .arg(artPath)
                                    .arg(filesDone)
                                    .arg(filesTotal)
                                    .arg(formatBytes(bytesDone))
                                    .arg(formatBytes(bytesTotal)));
                    QApplication::processEvents();
                }
            });

    if (ok)
        writeProgress.setValue(1000);
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
        QString gameId;
        QString databaseTitle;
        QString renameDestination;
        int bank = 0;
        qulonglong bytes = 0;
        qulonglong sourceKb = 0;
    };

    const bool renameFromDatabase =
            renameOnTransferCheck &&
            renameOnTransferCheck->isChecked();

    QHash<QString, QString> gameTitleDatabase;
    if (renameFromDatabase) {
        if (!network) {
            QMessageBox::critical(
                    this,
                    "Latest gameid.txt unavailable",
                    "Auto rename is enabled, but the network manager is unavailable. "
                    "No transfer was started. Untick auto rename to transfer without renaming.");
            return;
        }

        const QUrl databaseUrl(
                "https://raw.githubusercontent.com/L10N37/"
                "PS2-ISO-Batch-Renamer-/refs/heads/main/gameid.txt");

        statusLabel->setText(
                "Refreshing the latest PS2 Batch Renamer gameid.txt...");
        QApplication::processEvents();

        QNetworkRequest request(databaseUrl);
        request.setHeader(
                QNetworkRequest::UserAgentHeader,
                QString("PS2-HDD-Manager/%1").arg(PS2_HDD_APP_VERSION));
        request.setAttribute(
                QNetworkRequest::RedirectPolicyAttribute,
                QNetworkRequest::NoLessSafeRedirectPolicy);

        QNetworkReply *reply = network->get(request);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timeout.start(30000);
        loop.exec();
        timeout.stop();

        const int httpStatus =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError networkError = reply->error();
        const QString networkErrorText = reply->errorString();
        const QByteArray databaseBytes = reply->readAll();
        reply->deleteLater();

        if (networkError != QNetworkReply::NoError || httpStatus != 200) {
            QMessageBox::critical(
                    this,
                    "Latest gameid.txt unavailable",
                    QString("Auto rename is enabled, but the current gameid.txt could not "
                            "be downloaded from PS2-ISO-Batch-Renamer-.\n\n%1\n\n"
                            "No transfer was started. Untick auto rename if you want to "
                            "transfer without renaming.")
                            .arg(httpStatus
                                    ? QString("HTTP %1").arg(httpStatus)
                                    : networkErrorText));
            return;
        }

        QString databaseError;
        if (!parseLiveGameIdDatabase(
                    databaseBytes,
                    &gameTitleDatabase,
                    &databaseError)) {
            QMessageBox::critical(
                    this,
                    "Latest gameid.txt rejected",
                    databaseError +
                    "\n\nNo transfer was started. The live database must validate "
                    "before automatic renaming is allowed.");
            return;
        }

        statusLabel->setText(
                QString("Latest gameid.txt loaded: %1 audited Game ID(s).")
                        .arg(gameTitleDatabase.size()));
        QApplication::processEvents();
    }

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

        QString gameId;
        qulonglong sourceKb = 0;
        bool sourceSizeOk = false;

        for (const QString &line : probe.split('\n', Qt::SkipEmptyParts)) {
            const QStringList fields = line.trimmed().split(';');
            if (fields.size() < 4)
                continue;

            QString mediaField = fields[0].trimmed();
            if (mediaField.startsWith("dual-layer "))
                mediaField.remove(0, QString("dual-layer ").size());

            if (mediaField != "DVD" && mediaField != "CD")
                continue;

            gameId = cleanMachineField(fields[3]).toUpper();

            QString sizeField =
                    cleanMachineField(fields[1]);
            if (sizeField.endsWith(
                        "KB",
                        Qt::CaseInsensitive))
                sizeField.chop(2);

            sourceKb =
                    sizeField.trimmed()
                        .toULongLong(&sourceSizeOk);

            if (sourceSizeOk && sourceKb > 0)
                break;
        }

        if (!sourceSizeOk || sourceKb == 0) {
            if (error)
                *error =
                    "Could not preserve exact cdvd_info2 source size for "
                    "the transfer queue cache.";
            return false;
        }

        QString name = info.completeBaseName().trimmed();
        if (name.isEmpty())
            name = "PS2 Game";

        QString databaseTitle;
        QString renameDestination;

        if (renameFromDatabase && !gameId.isEmpty()) {
            databaseTitle = gameTitleDatabase.value(gameId);

            if (!databaseTitle.isEmpty()) {
                const QString filenameError =
                        portableGameFilenameError(databaseTitle);

                if (filenameError.isEmpty()) {
                    name = databaseTitle;

                    const QString suffix = info.suffix();
                    const QString destinationName =
                            databaseTitle +
                            (suffix.isEmpty() ? QString() : "." + suffix);

                    renameDestination =
                            QDir(info.absolutePath()).filePath(destinationName);
                }
            }
        }

        if (name.size() > 159)
            name.truncate(159);

        out->path = info.absoluteFilePath();
        out->key = normalizedLocalPath(out->path);
        out->name = name;
        out->media = media;
        out->gameId = gameId;
        out->databaseTitle = databaseTitle;
        out->renameDestination = renameDestination;
        out->bank = bank;
        out->bytes = static_cast<qulonglong>(info.size());
        out->sourceKb = sourceKb;
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

    // Copy the selected disk descriptor. Holding a reference into the
    // disks vector across QApplication::processEvents() can become dangling
    // if the UI refreshes/rebuilds the disk list during a long transfer.
    const auto disk =
            disks[static_cast<std::size_t>(
                diskCombo->currentIndex())];

    if (QMessageBox::question(
                this,
                "Install queued games to PS2 HDD",
                QString("Install %1 queued game(s) as HDL APA partitions?\n\n"
                        "%2\n\nDestinations:\n%3\n\n"
                        "You may keep browsing the PC and add more games while "
                        "this queue runs. New marks join only at safe game boundaries. "
                        "Existing titles are skipped only after startup identity/media "
                        "and exact hdl_toc/cdvd_info2 size match.%4")
                        .arg(static_cast<qulonglong>(games.size()))
                        .arg(QString::fromStdString(disk.devicePath))
                        .arg(initialDestinations.join('\n'))
                        .arg(renameFromDatabase
                                ? QString("\n\nAuto rename: ON — latest live gameid.txt "
                                          "(%1 IDs) loaded from PS2 Batch Renamer main.")
                                      .arg(gameTitleDatabase.size())
                                : QString("\n\nAuto rename: OFF — source filenames will not change.")),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes)
        return;

    qulonglong totalBytes = 0;
    for (const Game &game : games)
        totalBytes += game.bytes;

    qulonglong completedBytes = 0;

    transferQueueRunning = true;

    // Keep the target/raw-HDD side stable but leave the PC side and bank/AUTO
    // selector responsive so more source games can be queued.
    copyToPs2Button->setEnabled(false);
    if (diskCombo)
        diskCombo->setEnabled(false);
    if (ps2View)
        ps2View->setEnabled(false);
    if (addArtButton)
        addArtButton->setEnabled(false);
    if (installAppsButton)
        installAppsButton->setEnabled(false);
    if (applyOplDefaultsButton)
        applyOplDefaultsButton->setEnabled(false);
    if (unlockButton)
        unlockButton->setEnabled(false);

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
    QStringList skippedExistingResults;
    QStringList renamedSourceResults;
    QStringList renameWarnings;

    auto renameProcessedSource = [&](const Game &game) {
        if (!renameFromDatabase)
            return;

        if (game.gameId.isEmpty()) {
            renameWarnings << QFileInfo(game.path).fileName() +
                    " — no startup/Game ID reported; source filename left unchanged.";
            return;
        }

        if (game.databaseTitle.isEmpty()) {
            renameWarnings << QFileInfo(game.path).fileName() +
                    QString(" — %1 is not in the current gameid.txt; source filename left unchanged.")
                            .arg(game.gameId);
            return;
        }

        const QString filenameError =
                portableGameFilenameError(game.databaseTitle);
        if (!filenameError.isEmpty()) {
            renameWarnings << QFileInfo(game.path).fileName() +
                    " — " + filenameError + "; source filename left unchanged.";
            return;
        }

        if (game.renameDestination.isEmpty())
            return;

        const QString source =
                QDir::cleanPath(QFileInfo(game.path).absoluteFilePath());
        const QString destination =
                QDir::cleanPath(QFileInfo(game.renameDestination).absoluteFilePath());

        if (source == destination) {
            renamedSourceResults << QFileInfo(source).fileName() +
                    " — already named correctly";
            return;
        }

        if (QFileInfo::exists(destination)) {
            renameWarnings << QFileInfo(source).fileName() +
                    " — rename destination already exists: " +
                    QFileInfo(destination).fileName() +
                    " (nothing overwritten)";
            return;
        }

        QFile sourceFile(source);
        if (!sourceFile.rename(destination)) {
            renameWarnings << QFileInfo(source).fileName() +
                    " — rename failed: " + sourceFile.errorString();
            return;
        }

        renamedSourceResults <<
                QFileInfo(source).fileName() + "  ->  " +
                QFileInfo(destination).fileName();
    };

    enum class ExistingState
    {
        Absent,
        Match,
        Mismatch
    };

    struct ExistingCheck
    {
        ExistingState state =
                ExistingState::Absent;
        int bank = -1;
        qulonglong installedKb = 0;
        qulonglong sourceKb = 0;
        QString media;
        QString startup;
        QString installedName;
    };

    auto checkExistingGame =
            [&](const Game &game,
                    ExistingCheck *result,
                    QString *error) -> bool {
        QString local;

        const QStringList args = {
            "--hdl-dump",
            hdlDumpPath,
            "--check-existing-game",
            "--install-game",
            game.path,
            "--game-name",
            game.name
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
            if (!line.startsWith(
                        "EXISTING\t"))
                continue;

            const QStringList fields =
                    line.split('\t');

            if (fields.size() < 8)
                continue;

            ExistingCheck parsed;

            const QString state =
                    cleanMachineField(
                        fields[1]);

            if (state == "MATCH")
                parsed.state =
                        ExistingState::Match;
            else if (state == "MISMATCH")
                parsed.state =
                        ExistingState::Mismatch;
            else if (state == "ABSENT")
                parsed.state =
                        ExistingState::Absent;
            else
                continue;

            parsed.bank =
                    cleanMachineField(
                        fields[2]).toInt();
            parsed.installedKb =
                    cleanMachineField(
                        fields[3]).toULongLong();
            parsed.sourceKb =
                    cleanMachineField(
                        fields[4]).toULongLong();
            parsed.media =
                    cleanMachineField(
                        fields[5]);
            parsed.startup =
                    cleanMachineField(
                        fields[6]);
            parsed.installedName =
                    cleanMachineField(
                        fields[7]);

            if (result)
                *result = parsed;

            return true;
        }

        if (error)
            *error =
                    "Existing-game verification returned no machine record.\n" +
                    local.right(1800);

        return false;
    };

    // PS2_HDD_QUEUE_EXISTING_CACHE_V1
    // Cache every installed HDL identity once at queue start.  The old hot
    // loop launched --check-existing-game for each source image, causing a
    // complete all-bank hdl_toc rescan hundreds of times on large disks.
    std::vector<ExistingCheck> queueInstalledCache;

    {
        statusLabel->setText(
                "Caching installed HDL identities once for this transfer queue...");
        QApplication::processEvents();

        QString cacheOutput;
        const bool cacheOk =
                runPrivilegedWriter(
                    disk,
                    {
                        "--hdl-dump",
                        hdlDumpPath,
                        "--list-games"
                    },
                    &cacheOutput);

        if (!cacheOk) {
            transferQueueRunning = false;

            if (diskCombo)
                diskCombo->setEnabled(true);
            if (ps2View)
                ps2View->setEnabled(true);
            if (addArtButton)
                addArtButton->setEnabled(true);
            if (installAppsButton)
                installAppsButton->setEnabled(true);
            if (applyOplDefaultsButton)
                applyOplDefaultsButton->setEnabled(true);
            updateUnlockUi();
            copyToPs2Button->setEnabled(
                    selectedDiskCanInstallGames());

            QMessageBox::critical(
                    this,
                    "Could not cache installed games",
                    "The transfer queue was not started because the initial "
                    "read-only HDL game scan failed:\n\n" +
                    cacheOutput.right(5000));
            return;
        }

        for (const QString &line :
                cacheOutput.split(
                    '\n',
                    Qt::SkipEmptyParts)) {
            if (!line.startsWith("GAME\t"))
                continue;

            const QStringList fields =
                    line.split('\t');

            if (fields.size() < 6)
                continue;

            bool bankOk = false;
            bool sizeOk = false;

            ExistingCheck cached;
            cached.bank =
                    cleanMachineField(
                        fields[1]).toInt(&bankOk);
            cached.media =
                    cleanMachineField(
                        fields[2]).toUpper();
            cached.installedKb =
                    cleanMachineField(
                        fields[3]).toULongLong(&sizeOk);
            cached.sourceKb = cached.installedKb;
            cached.startup =
                    cleanMachineField(
                        fields[4]).toUpper();
            cached.installedName =
                    cleanMachineField(
                        fields.mid(5).join(" "));

            if (bankOk && sizeOk)
                queueInstalledCache.push_back(
                        std::move(cached));
        }

        statusLabel->setText(
                QString("Cached %1 installed HDL game(s); "
                        "per-game all-bank rescans disabled.")
                    .arg(
                        static_cast<qulonglong>(
                            queueInstalledCache.size())));
        QApplication::processEvents();
    }

    auto checkExistingFromQueueCache =
            [&](const Game &game,
                    ExistingCheck *result) -> bool {
        ExistingCheck parsed;
        parsed.state = ExistingState::Absent;
        parsed.sourceKb = game.sourceKb;
        parsed.media = game.media.toUpper();
        parsed.startup = game.gameId.toUpper();

        for (const ExistingCheck &cached :
                queueInstalledCache) {
            const bool sameIdentity =
                    !game.gameId.isEmpty()
                        ? cached.startup.compare(
                            game.gameId,
                            Qt::CaseInsensitive) == 0
                        : cached.installedName ==
                            game.name;

            if (!sameIdentity)
                continue;

            parsed = cached;
            parsed.sourceKb = game.sourceKb;

            parsed.state =
                    cached.media.compare(
                        game.media,
                        Qt::CaseInsensitive) == 0 &&
                    cached.installedKb ==
                        game.sourceKb
                        ? ExistingState::Match
                        : ExistingState::Mismatch;

            if (result)
                *result = parsed;
            return true;
        }

        if (result)
            *result = parsed;
        return true;
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

    // PC browsing and queue marking stay live during the transfer. New marks
    // are converted into Game records only at these explicit safe boundaries,
    // never from inside the hdl_dump/progress event loop.
    auto appendLiveQueue = [&]() {
        QApplication::processEvents();

        QStringList marked =
                markedPcPaths.values();
        marked.sort(Qt::CaseInsensitive);

        int added = 0;

        for (const QString &path : marked) {
            const QString key =
                    normalizedLocalPath(path);

            if (key.isEmpty() ||
                    knownQueued.contains(key))
                continue;

            const int bank =
                    queuedPcBanks.value(
                        key,
                        fallbackBank);

            Game newGame;
            QString error;

            if (!makeGame(
                        path,
                        bank,
                        &newGame,
                        &error)) {
                knownQueued.insert(key);
                failedResults <<
                        QFileInfo(path).fileName() +
                        " — could not be added to live queue: " +
                        error;
                continue;
            }

            knownQueued.insert(newGame.key);
            games.push_back(newGame);
            totalBytes += newGame.bytes;
            ++added;
        }

        if (added > 0) {
            overallProgress->setProperty(
                    "queueTotalBytes",
                    totalBytes);
            overallProgress->setProperty(
                    "queueGameCount",
                    static_cast<int>(
                        games.size()));

            statusBar()->showMessage(
                    QString(
                        "%1 new game(s) appended safely; "
                        "%2 total in this run.")
                        .arg(added)
                        .arg(
                            static_cast<qulonglong>(
                                games.size())));
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

        // Value copy: appendLiveQueue() may grow/reallocate `games`
        // later in this iteration without invalidating the current item.
        Game game = games[i];

        if (markedPcPaths.contains(game.key) &&
                queuedPcBanks.contains(game.key))
            game.bank =
                    queuedPcBanks.value(
                        game.key,
                        game.bank);

        statusLabel->setText(
                QString(
                    "Checking cached HDD identities for %1...")
                    .arg(game.name));
        QApplication::processEvents();

        ExistingCheck existing;
        QString existingError;

        if (!checkExistingFromQueueCache(
                    game,
                    &existing)) {
            queueAborted = true;
            queueAbortReason =
                    game.name +
                    " — queue-cache verification failed before any write.";
            failedResults <<
                    queueAbortReason;
            break;
        }

        if (existing.state ==
                ExistingState::Match) {
            skippedExistingResults <<
                    QString(
                        "%1 — %2 — Bank %3 — exact size match (%4 KB)%5")
                        .arg(game.name)
                        .arg(existing.media.isEmpty()
                                ? game.media.toUpper()
                                : existing.media.toUpper())
                        .arg(existing.bank)
                        .arg(existing.sourceKb)
                        .arg(
                            existing.startup.isEmpty()
                                ? QString()
                                : QString(" — %1")
                                    .arg(existing.startup));

            statusLabel->setText(
                    QString(
                        "%1 already exists on Bank %2 with an exact disc-size match — skipped.")
                        .arg(game.name)
                        .arg(existing.bank));

            renameProcessedSource(game);

            markedPcPaths.remove(game.key);
            queuedPcBanks.remove(game.key);

            transferProgress->setValue(100);
            transferProgress->setFormat(
                    "Existing game verified — skipped");

            finishQueueItem(game, i);

            if (pcView) {
                pcView->viewport()->update();
                pcView->update();
            }

            updateMarkedStatus();
            QApplication::processEvents();
            continue;
        }

        if (existing.state ==
                ExistingState::Mismatch) {
            queueAborted = true;
            queueAbortReason =
                    QString(
                        "%1 matches an installed game identity on Bank %2, "
                        "but the disc size/media does NOT match "
                        "(installed %3 KB, selected image %4 KB). "
                        "No duplicate was written.")
                        .arg(game.name)
                        .arg(existing.bank)
                        .arg(existing.installedKb)
                        .arg(existing.sourceKb);

            failedResults <<
                    queueAbortReason;
            break;
        }

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
                QString("Starting self-verifying Bank %1 transaction for %2...")
                        .arg(game.bank)
                        .arg(game.name));
        QApplication::processEvents();

        const QStringList installArgs = {
            "--hdl-dump", hdlDumpPath,
            "--install-game", game.path,
            "--game-name", game.name,
            "--media", game.media,
            "--bank", QString::number(game.bank)
        };

        QString output;
        const bool installed =
                runPrivilegedWriter(
                    disk,
                    installArgs,
                    &output,
                    true,
                    prefix);

        if (installed) {
            // PS2_HDD_QUEUE_EXISTING_CACHE_V1
            // Writer-side preflight/check and post-write verification remain
            // authoritative.  Mirror the committed identity into the queue
            // cache so later live-queued entries cannot duplicate it.
            bool cacheAlreadyHasIdentity = false;

            for (const ExistingCheck &cached :
                    queueInstalledCache) {
                const bool sameIdentity =
                        !game.gameId.isEmpty()
                            ? cached.startup.compare(
                                game.gameId,
                                Qt::CaseInsensitive) == 0
                            : cached.installedName ==
                                game.name;

                if (sameIdentity) {
                    cacheAlreadyHasIdentity = true;
                    break;
                }
            }

            if (!cacheAlreadyHasIdentity) {
                ExistingCheck cached;
                cached.state = ExistingState::Match;
                cached.bank = game.bank;
                cached.installedKb = game.sourceKb;
                cached.sourceKb = game.sourceKb;
                cached.media = game.media.toUpper();
                cached.startup = game.gameId.toUpper();
                cached.installedName = game.name;

                queueInstalledCache.push_back(
                        std::move(cached));
            }

            if (output.contains(
                        "GAME SKIPPED EXISTING SIZE MATCH")) {
                skippedExistingResults <<
                        QString(
                            "%1 — Bank %2 — exact size match "
                            "(writer-side race check)")
                            .arg(game.name)
                            .arg(game.bank);
            }
            else {
                installedResults <<
                        QString("%1 — %2 — Bank %3")
                        .arg(game.name)
                        .arg(game.media.toUpper())
                        .arg(game.bank);
            }

            renameProcessedSource(game);

            markedPcPaths.remove(game.key);
            queuedPcBanks.remove(game.key);
        }
        else {
            // Never blindly retry a failed raw-HDD transaction.
            queueAborted = true;
            queueAbortReason =
                    QString("%1 — Bank %2 writer transaction failed. "
                            "No automatic retry and no further HDD writes:\n%3")
                    .arg(game.name)
                    .arg(game.bank)
                    .arg(output.right(5000).trimmed());

            failedResults << queueAbortReason;
            break;
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

    transferQueueRunning = false;

    if (diskCombo)
        diskCombo->setEnabled(true);
    if (ps2View)
        ps2View->setEnabled(true);
    if (addArtButton)
        addArtButton->setEnabled(true);
    if (installAppsButton)
        installAppsButton->setEnabled(true);
    if (applyOplDefaultsButton)
        applyOplDefaultsButton->setEnabled(true);
    updateUnlockUi();

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

    const int successfulCount =
            installedResults.size() +
            skippedExistingResults.size();

    QString summary =
            QString(
                "Processed successfully: %1 / %2\n"
                "Newly installed: %3\n"
                "Already present + exact size match: %4")
                .arg(successfulCount)
                .arg(
                    static_cast<qulonglong>(
                        games.size()))
                .arg(installedResults.size())
                .arg(skippedExistingResults.size());

    if (!installedResults.isEmpty())
        summary +=
                "\n\nInstalled games — media / destination:\n  " +
                installedResults.join("\n  ");

    if (!skippedExistingResults.isEmpty())
        summary +=
                "\n\nAlready present — verified size match:\n  " +
                skippedExistingResults.join("\n  ");

    if (!failedResults.isEmpty())
        summary += "\n\nFailed (left queued when applicable):\n  "
                + failedResults.join("\n  ");

    if (!verificationWarnings.isEmpty())
        summary +=
                "\n\nVerification notes:\n  " +
                verificationWarnings.join("\n  ");

    if (!renamedSourceResults.isEmpty())
        summary +=
                "\n\nSource filenames — latest gameid.txt:\n  " +
                renamedSourceResults.join("\n  ");

    if (!renameWarnings.isEmpty())
        summary +=
                "\n\nSource rename notes (nothing overwritten):\n  " +
                renameWarnings.join("\n  ");

    if (queueAborted)
        summary +=
                "\n\nQUEUE STOPPED SAFELY: " +
                queueAbortReason +
                "\nRemaining marked games were left queued and were not written.";

    {
        QDialog resultDialog(this);
        resultDialog.setWindowTitle("HDL game queue results");
        resultDialog.resize(820, 720);

        auto *resultLayout = new QVBoxLayout(&resultDialog);

        auto *summaryView = new QPlainTextEdit(&resultDialog);
        summaryView->setReadOnly(true);
        summaryView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        summaryView->setPlainText(summary);
        summaryView->moveCursor(QTextCursor::Start);
        resultLayout->addWidget(summaryView, 1);

        auto *resultButtons = new QDialogButtonBox(
                QDialogButtonBox::Ok, &resultDialog);
        connect(resultButtons, &QDialogButtonBox::accepted,
                &resultDialog, &QDialog::accept);
        resultLayout->addWidget(resultButtons);

        resultDialog.exec();
    }

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
    if (transferQueueRunning) {
        statusBar()->showMessage(
                "PFS file writes are disabled while the game queue is running.");
        return;
    }
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
    if (transferQueueRunning) {
        statusBar()->showMessage("OPL app writes are disabled while the game queue is running.");
        return;
    }
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
    if (transferQueueRunning) {
        statusBar()->showMessage("OPL configuration writes are disabled while the game queue is running.");
        return;
    }
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
