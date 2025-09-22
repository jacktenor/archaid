#ifndef INSTALLWIZARD_H
#define INSTALLWIZARD_H

#include <QWizard>
#include <QProgressBar>
#include <QStringList>
#include "installerworker.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class Installwizard;
}
QT_END_NAMESPACE

class Installwizard : public QWizard {
    Q_OBJECT

public:
    explicit Installwizard(QWidget *parent = nullptr);
    ~Installwizard();
    QString getCustomMirrorUrl() const;
    void setWizardButtonEnabled(QWizard::WizardButton which, bool enabled);
    void appendLog(const QString &message);
    void mountAllUnmoutedPartitions();
    void mountAllPartitionsOnDrive(const QString &drive);

private slots:
    void onPageChanged(int id);
    void onDependenciesCheckFinished(bool ok);
    void onPartitionPrepared();
    void onSystemInstallFinished();
    void hookWizardSignals();  // call this once from your ctor

private:
    QString customMirrorUrl;
    void maybeStartDependenciesCheck(bool force = false);
    bool depsCheckStarted_ = false;  // add this member if you don't already have it
    void runDependenciesInstall(const QStringList &packages, const QString &distro);
    bool depsOk_ = false;
    bool partitionPrepared_ = false;
    bool installFinished_ = false;
    bool confirmDestructive(const QString &message);
    void installDependencies();
    Ui::Installwizard *ui;
    QString selectedDrive;  // 🧠 TRACK THE CURRENT DRIVE
    bool efiInstall = false; // track chosen boot mode
    InstallerWorker::InstallMode installMode = InstallerWorker::InstallMode::UseFreeSpace;
    QString selectedPartition;
    QString getUserHome();
    void populateDrives(); // Populate the dropdown with available drives
    void downloadISO(QProgressBar *progressBar);
    void on_installButton_clicked();
    void unmountDrive(const QString &drive);
    // Declare the methods that were missing
    QStringList getAvailableDrives();        // Detect available drives
    void prepareDrive(const QString &drive);   // Prepare the selected drive
    void prepareExistingPartition(const QString &partition);
    void prepareFreeSpace(const QString &drive);
    void splitPartitionForEfi(const QString &partition);
    void populatePartitionTable(const QString &drive); // new
    void prepareForEfi(const QString &drive); // use free space for EFI
    void handleDriveChange(const QString &text);
    QString getParentDrive(const QString &partition);
    void mountStandardPartitions(const QString &drive);
    void onPartitionSelected(const QModelIndex &index);
    QString targetPartition;
};
#endif // INSTALLWIZARD_H
