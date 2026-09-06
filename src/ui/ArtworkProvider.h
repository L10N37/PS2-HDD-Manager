#ifndef ARTWORKPROVIDER_H
#define ARTWORKPROVIDER_H

#include <QUrl>
#include <QString>
#include <QStringList>

class ArtworkProvider
{
public:
    virtual ~ArtworkProvider() = default;
    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual QString description() const = 0;
    virtual QStringList defaultTypes() const = 0;
    virtual QUrl artworkUrl(const QString &gameId, const QString &type) const = 0;
};

class OplManagerMirrorProvider final : public ArtworkProvider
{
public:
    QString id() const override;
    QString displayName() const override;
    QString description() const override;
    QStringList defaultTypes() const override;
    QUrl artworkUrl(const QString &gameId, const QString &type) const override;
};

#endif // ARTWORKPROVIDER_H
