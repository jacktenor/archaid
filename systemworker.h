#ifndef SYSTEMWORKER_H
#define SYSTEMWORKER_H

#include <QObject>
#include <QString>
#include <QStringList>

class SystemWorker : public QObject {
    Q_OBJECT
public:
    explicit SystemWorker(QObject *parent = nullptr);

    void setParameters(const QString &drive,
                       const QString &username,
                       const QString &password,
                       const QString &rootPassword,
                       const QString &desktopEnv,
                       bool useEfi);
    void setCustomMirrorUrl(const QString &url) { customMirrorUrl = url; }

signals:
    void logMessage(const QString &msg);
    void errorOccurred(const QString &msg);
    void finished();

public slots:
    void run();

private:
    bool hasTargetPartition();
    QString targetPartitionPath();
    QString m_targetPartition;  // <— add this single field
    QString normalizePartitionPath(const QString &in) const;
    void setTargetPartition(const QString &sel);
    bool neutralizeLoginNoise();
    bool runCommandCapture(const QString &command, QString *output);
    bool applyLxqtIconTheme(const QString &user);
    bool installGrubRobust(const QString &targetDisk, bool efiInstall);
    QString customMirrorUrl;
    bool installDesktopAndDM();
    QString drive;
    QString username;
    QString password;
    QString rootPassword;
    QString desktopEnv;
    bool useEfi = false;
    bool generateGrubWithOsProber();
    bool runCommand(const QString &cmd);
};

#endif // SYSTEMWORKER_H
