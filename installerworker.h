#ifndef INSTALLERWORKER_H
#define INSTALLERWORKER_H

#include "qprocess.h"
#include <QObject>
#include <QString>

class InstallerWorker : public QObject {
    Q_OBJECT
public:
    enum InstallMode {
        WipeDrive,
        UsePartition,
        UseFreeSpace
    };

    Q_ENUM(InstallMode)
    explicit InstallerWorker(QObject *parent = nullptr);
    void setDrive(const QString &drive);
    void setMode(InstallMode mode);
    void setTargetPartition(const QString &partition);
    void setEfiInstall(bool efi);
    void mountStandardPartitions(const QString &drive);

signals:
    void logMessage(const QString &message);
    void errorOccurred(const QString &message);
    void installComplete();

public slots:
    void run();

private:
    QString selectedDrive;
    InstallMode mode = InstallMode::WipeDrive;
    QString targetPartition; // used when mode == UsePartition
    bool efiMode = false;
    void setEfiMode(bool enabled);
    bool efiInstall = false;
    bool getPartitionGeometry(const QString &targetPartition, const QString &selectedDrive, QString &startMiB, QString &endMiB);
    void createFromFreeSpace(QProcess &process, const QString &partedBin, const QString &devPath);
    void recreateFromSelectedPartition(QProcess &process, const QString &partedBin, const QString &devPath);
    void wipeDriveAndPartition(QProcess &process, const QString &partedBin, const QString &devPath);
};

#endif // INSTALLERWORKER_H
