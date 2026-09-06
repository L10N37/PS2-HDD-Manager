#include "ArtworkProvider.h"

QString OplManagerMirrorProvider::id() const
{
    return "opl-manager-db-mirror";
}

QString OplManagerMirrorProvider::displayName() const
{
    return "OPL Manager artwork database mirror";
}

QString OplManagerMirrorProvider::description() const
{
    return "Preserved OPL Manager GameArt database, indexed by PS2 Game ID. "
           "Downloads are cached locally and copied directly into OPL's ART folder.";
}

QStringList OplManagerMirrorProvider::defaultTypes() const
{
    return { "COV", "ICO", "SCR" };
}

QUrl OplManagerMirrorProvider::artworkUrl(const QString &gameId, const QString &type) const
{
    const QString id = gameId.trimmed().toUpper();
    const QString artType = type.trimmed().toUpper();
    const QString file = id + "_" + artType + ".png";
    return QUrl(QString("https://raw.githubusercontent.com/Luden02/psx-ps2-opl-art-database/refs/heads/main/PS2/%1/%2")
            .arg(id, file));
}
