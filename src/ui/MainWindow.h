#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <vector>
#include <functional>

#include "core/PhysicalDisk.h"

class QComboBox;
class QCheckBox;
class QModelIndex;
class QEvent;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QProgressBar;
class QNetworkAccessManager;
class QPushButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;
class QUrl;
class PrivilegedSession;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum Ps2NodeKind {
        NodeNone = 0,
        NodeHddRoot,
        NodeGamesRoot,
        NodeGamesMedia,
        NodeGame,
        NodePfsDirectory,
        NodePfsFile
    };

    void buildUi();
    QWidget *buildPcPane();
    QWidget *buildPs2Pane();
    void populatePcDrives();
    void navigatePc(const QString &path);
    void refreshDisks();
    void selectDisk(int index);
    void populatePs2Tree(const Ps2::PhysicalDiskCandidate &disk);
    void refreshCurrentPs2Tree();
    void populateInstalledGames(QTreeWidgetItem *gamesRoot, const Ps2::PhysicalDiskCandidate &disk);
    void populatePfsDirectory(QTreeWidgetItem *item, const QString &path,
            const Ps2::PhysicalDiskCandidate &disk, bool showErrors = true);

    void installSelectedPcGames();
    void installGameFiles(const QStringList &paths);
    struct InstalledGameRef;
    void renameSelectedInstalledGame();
    void renameInstalledGamesFromLatestDatabase(bool allInstalled);
    bool renameInstalledGameOnHdd(const InstalledGameRef &game,
            const QString &newName, QString *error);
    bool invalidateOplHddGameListCache(QString *error);
    bool fetchLatestGameTitleDatabase(QHash<QString, QString> *titles,
            QString *error);
    void toggleMarkedPcPath(const QModelIndex &index);
    QStringList markedOrSelectedPcPaths() const;
    void updateMarkedStatus();

    bool unlockHddSession(bool showFailure = true);
    void updateUnlockUi();

    struct InstalledGameRef {
        QString name;
        QString gameId;
        int bank = 0;
    };
    std::vector<InstalledGameRef> installedGames(bool selectedOnly = false) const;
    void addArtwork();
    bool downloadArtworkFile(const QUrl &url, const QString &cachePath, QString *error);
    bool probeGameImage(const QString &path, QString *media, QString *details) const;
    bool selectedDiskCanInstallGames(QString *reason = nullptr) const;
    bool selectedDiskCanBrowsePfs(QString *reason = nullptr) const;

    bool runPrivilegedWriter(const Ps2::PhysicalDiskCandidate &disk, const QStringList &modeArguments,
            QString *output, bool parseProgress = false, const QString &progressPrefix = QString(),
            const std::function<void(const QString &)> &outputCallback = {});
    void copyPcItemsToPfs(const QStringList &paths, QTreeWidgetItem *target);
    void applyRecommendedOplDefaults();
    void installOrUpdateOplApps();
    bool createPfsCopyManifest(const QStringList &paths, const QString &targetPath,
            bool smartOplRoot, QString *manifestPath, int *fileCount, QString *error) const;
    void resetProgress();
    void parseTransferProgress(const QString &text, const QString &prefix);

    static QString formatBytes(std::uint64_t bytes);
    static QString joinPfsPath(const QString &base, const QString &name);
    static QString pfsParentPath(const QString &path);

    QFileSystemModel *pcModel = nullptr;
    QTreeView *pcView = nullptr;
    QComboBox *pcDriveCombo = nullptr;
    QLineEdit *pcPath = nullptr;
    QComboBox *diskCombo = nullptr;
    QComboBox *gameBankCombo = nullptr;
    QCheckBox *renameOnTransferCheck = nullptr;
    QTreeWidget *ps2View = nullptr;
    QLabel *statusLabel = nullptr;
    QLabel *overallProgressDetail = nullptr;
    QLabel *progressDetail = nullptr;
    QProgressBar *overallProgress = nullptr;
    QProgressBar *transferProgress = nullptr;
    QPushButton *copyToPs2Button = nullptr;
    QPushButton *applyOplDefaultsButton = nullptr;
    QPushButton *installAppsButton = nullptr;
    QPushButton *addArtButton = nullptr;
    QPushButton *unlockButton = nullptr;
    QLabel *unlockLabel = nullptr;
    std::vector<Ps2::PhysicalDiskCandidate> disks;
    QString writerPath;
    QString hdlDumpPath;
    QString pfsshellPath;
    QString fetchPayloadsPath;
    QString runtimePayloadPath;
    QString currentOplPartition;
    QString currentOplBase;
    QSet<QString> markedPcPaths;
    QHash<QString, int> queuedPcBanks;
    quint64 batchTotalBytes = 0;
    quint64 batchCompletedBytes = 0;
    quint64 batchCurrentBytes = 0;
    int batchCurrentIndex = 0;
    int batchGameCount = 0;
    PrivilegedSession *privilegedSession = nullptr;
    QNetworkAccessManager *network = nullptr;

    // Synchronous raw-HDD operations keep the Qt UI responsive by pumping the
    // event loop. These guards prevent a queued refresh/disk-selection event
    // from clearing QTreeWidget items or rebuilding the disk vector while the
    // current stack still owns pointers/references into them.
    bool hddOperationInProgress = false;
    bool ps2RefreshInProgress = false;
    bool ps2RefreshPending = false;
    bool diskRescanPending = false;

    // Incremented before every whole-tree clear/rebuild. Long-running helper
    // calls capture this value and refuse to touch old QTreeWidgetItem pointers
    // if another code path somehow replaced the tree while they were waiting.
    quint64 ps2TreeGeneration = 0;

    // True for the complete install queue, including the gaps between helper
    // transactions. PC browsing/marking remains interactive; raw-HDD controls
    // are held stable until the queue finishes.
    bool transferQueueRunning = false;
};

#endif // MAINWINDOW_H
