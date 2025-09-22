#include "Installwizard.h"
#include "systemworker.h"
#include "ui_Installwizard.h"
#include "installerworker.h"
#include <QMessageBox>
#include <QThread>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextStream>
#include <QTreeWidgetItem>
#include <QRegularExpression>
#include <QStandardItem>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QComboBox>

Installwizard::Installwizard(QWidget *parent)
    : QWizard(parent), ui(new Ui::Installwizard)
{
    ui->setupUi(this);

    connect(ui->mirrorLineEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        customMirrorUrl = text.trimmed();
        qDebug() << "Custom Mirror set to:" << customMirrorUrl;
    });

    // Connect Download button: use existing ISO if present, otherwise download.
    // Uses your existing downloadISO(...) and installDependencies() implementations.
    connect(ui->downloadButton, &QPushButton::clicked, this, [this]() {
        const QString isoPath = QDir::tempPath() + "/archlinux.iso";  // same path used by downloadISO()

        if (QFileInfo::exists(isoPath)) {
            QMessageBox msg(this);
            msg.setWindowTitle("Arch ISO");
            msg.setText(QString("Found ISO:\n%1\n\nUse this file or download a new one?")
                            .arg(isoPath));
            QPushButton *useBtn     = msg.addButton("Use existing", QMessageBox::AcceptRole);
            QPushButton *replaceBtn = msg.addButton("Download new", QMessageBox::DestructiveRole);
            msg.addButton(QMessageBox::Cancel);
            msg.exec();

            if (msg.clickedButton() == useBtn) {
                appendLog("Using existing ISO: " + isoPath);

                // Show “complete” on your existing progress bar so the UI looks consistent.
                if (ui->progressBar) {
                    ui->progressBar->setRange(0, 100);
                    ui->progressBar->setValue(100);
                    ui->progressBar->setVisible(true);
                }

                // Run deps here only if they haven't already succeeded at startup
                if (!depsOk_) {
                    installDependencies();     // your existing routine (don’t touch it)
                } else {
                    appendLog("Dependencies already satisfied.");
                }
                return;
            } else if (msg.clickedButton() != replaceBtn) {
                appendLog("ISO action cancelled.");
                return;
            }
            // else: fall through → user chose “Download new”
        }

        // No ISO present, or user chose to replace → use your existing downloader.
        // It already updates ui->progressBar, shows the success popup,
        // and calls installDependencies() afterwards. No extra code needed.
        downloadISO(ui->progressBar);
    });


    connect(ui->partRefreshButton, &QPushButton::clicked, this, &Installwizard::populateDrives);

    mountStandardPartitions(selectedDrive);
    populateDrives();

    // initial gate states
    depsOk_ = false;
    partitionPrepared_ = false;
    installFinished_ = false;

    hookWizardSignals();                 // <-- make page changes drive the buttons
    onPageChanged(currentId());          // <-- apply correct disabled/enabled now


    connect(ui->driveDropdown, &QComboBox::currentTextChanged, this, &Installwizard::handleDriveChange);

    connect(ui->treePartitions, &QTreeView::clicked, this, &Installwizard::onPartitionSelected);

    // Populate desktop environments on entering the 3rd page
    connect(this, &QWizard::currentIdChanged, this, [this](int id) {
        if (id == 2) {
            if (ui->comboDesktopEnvironment->count() == 0) {
                ui->comboDesktopEnvironment->addItems({
                    "GNOME", "KDE Plasma", "XFCE", "LXQt", "Cinnamon", "MATE", "None,"   // , "i3"
                });
            }
        }
    });

    // Connect install button
    connect(ui->installButton, &QPushButton::clicked, this, &Installwizard::on_installButton_clicked);

    connect(ui->prepareButton, &QPushButton::clicked, this, [this]() {
        QString selectedMode = ui->comboInstallMode->currentText();

        if (selectedMode == "Use selected partition") {
            // Partition install mode
            if (targetPartition.isEmpty() || targetPartition == "/dev/") {
                QMessageBox::warning(this, "Error", "Please select a valid partition.");
                return;
            }
            efiInstall = false;
            prepareExistingPartition(targetPartition);
        } else {
            // Full drive or free space modes
            QString drive = ui->driveDropdown->currentText();
            if (drive.isEmpty() || drive == "No drives found") {
                QMessageBox::warning(this, "Error", "Please select a valid drive.");
                return;
            }
            efiInstall = false;
            prepareDrive(drive.mid(5)); // Removes '/dev/' prefix if present
        }
    });


// EFI/UEFI button
connect(ui->createPartButton, &QPushButton::clicked, this, [this]() {
    QString drive = ui->driveDropdown->currentText();
    if (drive.isEmpty() || drive == "No drives found") {
        QMessageBox::warning(this, "Error", "Please select a valid drive.");
        return;
    }
    efiInstall = true;
    prepareForEfi(drive.mid(5));
    });
}

Installwizard::~Installwizard() {
    delete ui;
}



QString Installwizard::getCustomMirrorUrl() const {
    return customMirrorUrl;
}

void Installwizard::mountStandardPartitions(const QString &drive)
{
    QProcess process;
    process.start("lsblk", QStringList() << "-ln" << "-o" << "NAME,TYPE" << QString("/dev/%1").arg(drive));
    process.waitForFinished();
    QStringList lines = QString(process.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);

    QString rootPart, efiPart;

    for (const QString &line : lines) {
        QStringList cols = line.split(QRegularExpression("\\s+"));
        if (cols.size() != 2 || cols[1] != "part")
            continue;
        QString partName = cols[0];
        if (partName.endsWith("1")) efiPart = "/dev/" + partName;
        else if (partName.endsWith("2")) rootPart = "/dev/" + partName;
    }

    if (!rootPart.isEmpty())
        QProcess::execute("sudo", {"mount", rootPart, "/mnt"});

    if (!efiPart.isEmpty()) {
        QProcess::execute("sudo", {"mkdir", "-p", "/mnt/boot/efi"});
        QProcess::execute("sudo", {"mount", efiPart, "/mnt/boot/efi"});
    }
}

// Runs a dependency install/check and gates the Page-1 Next button only on deps,
// ignoring whether the ISO is downloaded.
void Installwizard::runDependenciesInstall(const QStringList &packages, const QString &distro)
{
    // Lock Page-1 Next while we work
    depsOk_ = false;
    if (currentId() == 0) setWizardButtonEnabled(QWizard::NextButton, false);

    // Build the install command (idempotent where possible)
    QString installCmd;
    if (distro == "fedora") {
        installCmd = "pkexec dnf install -y " + packages.join(" ");
    } else if (distro == "arch" || distro == "archlinux") {
        installCmd = "pkexec pacman -S --noconfirm --needed " + packages.join(" ");
    } else {
        installCmd = "pkexec apt install -y " + packages.join(" ");
    }

    QProcess process;
    qDebug() << "Installing dependencies:" << installCmd;
    appendLog("Installing dependencies… (ISO status ignored)");

    process.start("/bin/bash", QStringList() << "-c" << installCmd);
    process.waitForFinished(-1);

    const QString output = process.readAllStandardOutput();
    const QString error  = process.readAllStandardError();
    qDebug() << "Dependency Install Output:"  << output;
    qDebug() << "Dependency Install Errors:"  << error;

    if (process.exitCode() != 0) {
        QMessageBox::critical(this, "Error",
                              "Failed to install required dependencies:\n" + error);
        appendLog("❌ Dependencies NOT satisfied.");
        // Keep Next disabled
        return;
    }

    // Success ➜ enable Page-1 Next. ISO presence is NOT required here.
    depsOk_ = true;
    appendLog("✔️ Dependencies installed/verified. You can click Next.");

    if (currentId() == 0) setWizardButtonEnabled(QWizard::NextButton, true);
}

void Installwizard::hookWizardSignals()
{
    // Keep buttons in sync with the active page
    connect(this, &QWizard::currentIdChanged, this, &Installwizard::onPageChanged);
}

void Installwizard::onPageChanged(int id)
{
    // Page indices: 0 = dependencies, 1 = partitioning, 2 = install
    switch (id) {
    case 0: // Page 1
        // DO NOT auto-run dependency checks here anymore.
        // Only gate Next based on depsOk_ (false until user action).
        setWizardButtonEnabled(QWizard::NextButton, depsOk_);
        setWizardButtonEnabled(QWizard::FinishButton, false);
        break;

    case 1: // Page 2
        setWizardButtonEnabled(QWizard::NextButton, true); //  partitionPrepared_
        setWizardButtonEnabled(QWizard::FinishButton, false);
        break;

    case 2: // Page 3
        setWizardButtonEnabled(QWizard::NextButton, false);
        setWizardButtonEnabled(QWizard::FinishButton, installFinished_);
        break;

    default:
        setWizardButtonEnabled(QWizard::NextButton, false);
        setWizardButtonEnabled(QWizard::FinishButton, false);
        break;
    }
}

// Called when your async dependency checker finishes.
// We IGNORE whether an ISO was downloaded; only deps matter here.
void Installwizard::onDependenciesCheckFinished(bool ok)
{
    depsOk_ = ok;
    if (!ok) {
        appendLog("Dependencies not satisfied. Next disabled.");
    }
    // Only enable the Page 1 Next button; others remain gated by their own flags.
    if (currentId() == 0) {
        setWizardButtonEnabled(QWizard::NextButton, depsOk_);
    }
}

// Called when the partitioning worker (InstallerWorker) reports success.
void Installwizard::onPartitionPrepared()
{
    partitionPrepared_ = true;
    if (currentId() == 1) {
        setWizardButtonEnabled(QWizard::NextButton, true);
    }
    appendLog("✔️ Partition prepared (flag set).");
}

// Called when the system install (SystemWorker) is fully done.
void Installwizard::onSystemInstallFinished()
{
    installFinished_ = true;
    if (currentId() == 2) {
        setWizardButtonEnabled(QWizard::FinishButton, true);
    }
    appendLog("✔️ System installation complete.");
}

void Installwizard::handleDriveChange(const QString &text)
{
    if (!text.isEmpty() && text != "No drives found") {
        selectedDrive = text.mid(5);
        // New: Mount all partitions
        mountStandardPartitions(selectedDrive);
        if (currentId() == 1)
            populatePartitionTable(selectedDrive);
    }
}

void Installwizard::prepareDrive(const QString &drive) {
    selectedDrive = drive;

    InstallerWorker *worker = new InstallerWorker;
    worker->setDrive(drive);

    const QString selectedMode = ui->comboInstallMode->currentText();
    const QString devMsg = drive.startsWith("/dev/") ? drive : ("/dev/" + drive);

    if (selectedMode == "Erase entire drive") {
        if (!confirmDestructive(QString(
                                    "You're about to ERASE ALL DATA on %1.\n\n"
                                    "Are you absolutely sure?\n"
                                    "This is IRREVERSIBLE!!!").arg(devMsg))) {
            appendLog("User cancelled: Erase entire drive.");
            delete worker;
            return;
        }
        worker->setMode(InstallerWorker::InstallMode::WipeDrive);
    } else if (selectedMode == "Use selected partition") {
        worker->setMode(InstallerWorker::InstallMode::UsePartition);
        worker->setTargetPartition(targetPartition);
    } else { // "Use free space"
        worker->setMode(InstallerWorker::InstallMode::UseFreeSpace);

        // If a free row is selected, encode its exact extent
        if (QTreeWidgetItem *item = ui->treePartitions->currentItem()) {
            const QString type = item->text(2);
            if (type.compare("free", Qt::CaseInsensitive) == 0) {
                const double startMiB = item->data(0, Qt::UserRole + 1).toDouble();
                const double endMiB   = item->data(0, Qt::UserRole + 2).toDouble();
                if (startMiB > 0 && endMiB > startMiB) {
                    const QString token = QString("__FREE__:%1:%2")
                    .arg(startMiB, 0, 'f', 2)
                        .arg(endMiB,   0, 'f', 2);
                    worker->setTargetPartition(token);
                    appendLog(QString("Requested free-space install at %1").arg(token));
                }
            }
        }
    }

    worker->setEfiInstall(efiInstall);

    QThread *thread = new QThread;
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &InstallerWorker::run);
    connect(worker, &InstallerWorker::logMessage, this, [this](const QString &msg) { appendLog(msg); });
    connect(worker, &InstallerWorker::errorOccurred, this, [this](const QString &msg) {
        QMessageBox::critical(this, "Error", msg);
    });

    connect(worker, &InstallerWorker::installComplete, thread, &QThread::quit);
    connect(worker, &InstallerWorker::installComplete, this, [this]() {
        appendLog("✔️ Drive preparation complete.");
        setWizardButtonEnabled(QWizard::NextButton, true);
    });
    connect(worker, &InstallerWorker::installComplete, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    thread->start();
}

// NEW helper: single place to show a big red confirmation dialog.
// Returns true to proceed, false to cancel.
bool Installwizard::confirmDestructive(const QString &message)
{
    QMessageBox box(this);
    box.setIcon(QMessageBox::Critical);
    box.setWindowTitle("Confirm Destructive Action");
    box.setTextFormat(Qt::PlainText);
    box.setText(message);  // e.g. "You're about to delete ALL DATA on /dev/sdb. Are you absolutely sure? This is IRREVERSIBLE!!!"
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    return box.exec() == QMessageBox::Yes;
}

void Installwizard::prepareExistingPartition(const QString &partition) {
    selectedDrive = getParentDrive(partition);


    // Strong warning for delete & recreate of a specific partition
    if (!confirmDestructive(QString(
                                "You're about to delete ALL DATA on %1.\n\n"
                                "Are you absolutely sure?\n"
                                "This is IRREVERSIBLE!!!"
                                ).arg(partition))) {
        appendLog("User cancelled: Use selected partition.");
        return;
    }



    InstallerWorker *worker = new InstallerWorker;
    worker->setDrive(selectedDrive);
    worker->setMode(InstallerWorker::InstallMode::UsePartition);
    worker->setTargetPartition(partition);
    worker->setEfiInstall(efiInstall);

    QThread *thread = new QThread;
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &InstallerWorker::run);
    connect(worker, &InstallerWorker::logMessage, this,
            [this](const QString &msg) { appendLog(msg); });
    connect(worker, &InstallerWorker::errorOccurred, this,
            [this](const QString &msg) {
                QMessageBox::critical(this, "Error", msg);
            });
    connect(worker, &InstallerWorker::installComplete, thread, &QThread::quit);
    connect(worker, &InstallerWorker::installComplete, this, [this]() {
        appendLog("✔️ Partition prepared.");
        setWizardButtonEnabled(QWizard::NextButton, true);
    });

    connect(worker, &InstallerWorker::installComplete, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    thread->start();
}

void Installwizard::prepareFreeSpace(const QString &drive) {
    selectedDrive = drive;

    InstallerWorker *worker = new InstallerWorker;
    worker->setDrive(drive);
    worker->setMode(InstallerWorker::InstallMode::UseFreeSpace);
    worker->setEfiInstall(efiInstall);

    QThread *thread = new QThread;
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &InstallerWorker::run);
    connect(worker, &InstallerWorker::logMessage, this,
            [this](const QString &msg) { appendLog(msg); });
    connect(worker, &InstallerWorker::errorOccurred, this,
            [this](const QString &msg) {
                QMessageBox::critical(this, "Error", msg);
            });
    connect(worker, &InstallerWorker::installComplete, thread, &QThread::quit);
    connect(worker, &InstallerWorker::installComplete, this, [this]() {
        appendLog("✔️ Free space partition created.");
        setWizardButtonEnabled(QWizard::NextButton, true);
    });
    connect(worker, &InstallerWorker::installComplete, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    thread->start();
}

// Utility/helper (keep in wizard)
QString Installwizard::getParentDrive(const QString &partition) {
    QProcess proc;
    proc.start("lsblk", QStringList() << "-nr" << "-o" << "PKNAME" << partition);
    proc.waitForFinished();
    QString parent = QString(proc.readAllStandardOutput()).trimmed();
    return parent.isEmpty() ? selectedDrive : parent;
}

void Installwizard::appendLog(const QString &message)
{
    if (ui->logWidget3)
        ui->logWidget3->appendPlainText(message);
    if (ui->logView1)
        ui->logView1->appendPlainText(message);
    if (ui->logView2)
        ui->logView2->appendPlainText(message);
}

void Installwizard::setWizardButtonEnabled(QWizard::WizardButton which, bool enabled) {
    if (QAbstractButton *btn = button(which))
        btn->setEnabled(enabled);
}

void Installwizard::downloadISO(QProgressBar *progressBar) {
    QNetworkAccessManager *networkManager = new QNetworkAccessManager(this);

    // Get mirror URL from wizard field
    QString mirrorUrl = getCustomMirrorUrl();
    QString isoUrl;

    if (!mirrorUrl.isEmpty()) {
        if (!mirrorUrl.endsWith("/"))
            mirrorUrl += "/";
        isoUrl = mirrorUrl + "iso/latest/archlinux-x86_64.iso";
    } else {
        isoUrl = "https://mirror.csclub.uwaterloo.ca/archlinux/iso/latest/archlinux-x86_64.iso";
    }

    QUrl url(isoUrl);
    QNetworkRequest request(url);
    QNetworkReply *reply = networkManager->get(request);

    QString finalIsoPath = QDir::tempPath() + "/archlinux.iso";
    QFile *file = new QFile(finalIsoPath);

    if (!file->open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, "Error",
                              "Unable to open file for writing: " + finalIsoPath);
        reply->abort();
        reply->deleteLater();
        delete file;
        return;
    }

    appendLog(QString("Downloading ISO from %1").arg(isoUrl));
    connect(reply, &QNetworkReply::downloadProgress, this,
            [progressBar](qint64 bytesReceived, qint64 bytesTotal) {
                if (bytesTotal > 0) {
                    progressBar->setValue(
                        static_cast<int>((bytesReceived * 100) / bytesTotal));
                }
            });

    connect(reply, &QNetworkReply::readyRead, this, [file, reply]() {
        if (file->isOpen()) {
            file->write(reply->readAll());
        }
    });

    connect(
        reply, &QNetworkReply::finished, this,
        [this, file, reply, finalIsoPath]() {
            file->close();

            if (reply->error() == QNetworkReply::NoError) {
                // Set file permissions: readable by everyone
                QFile::setPermissions(finalIsoPath,
                                      QFile::ReadOwner | QFile::WriteOwner |
                                          QFile::ReadGroup | QFile::ReadOther);

                QMessageBox::information(
                    this, "Success",
                    "Arch Linux ISO downloaded successfully\nto: " + finalIsoPath +
                        " \nNext is Installing dependencies and extracting ISO...");
                installDependencies();

            } else {
                QFile::remove(finalIsoPath);
                QMessageBox::critical(
                    this, "Error", "Failed to download ISO: " + reply->errorString());
            }

            reply->deleteLater();
            file->deleteLater();
        });

    connect(reply, &QNetworkReply::errorOccurred, this,
            [this, file, reply, finalIsoPath](QNetworkReply::NetworkError) {
                QFile::remove(finalIsoPath);
                QMessageBox::critical(this, "Error",
                                      "Network error while downloading ISO: " +
                                          reply->errorString());

                reply->deleteLater();
                file->deleteLater();
            });
}
void Installwizard::installDependencies()
{
    // Packages required for the installer to run
    QStringList packages = {
        "arch-install-scripts", // arch-chroot, pacstrap
        "parted",
        "dosfstools",           // mkfs.vfat
        "e2fsprogs",            // mkfs.ext4
        "squashfs-tools",
        "wget"
    };

    // Detect distribution by reading /etc/os-release
    QString distro;
    QFile osRelease("/etc/os-release");
    if (osRelease.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&osRelease);
        while (!in.atEnd()) {
            const QString line = in.readLine();
            if (line.startsWith("ID=")) {
                distro = line.section('=', 1).remove('"').trimmed();
                break;
            }
        }
        osRelease.close();
    }
    if (distro.isEmpty())
        distro = "archlinux"; // sensible default if ID is missing

    // Hand off to the helper that installs deps and gates the Page-1 Next button
    runDependenciesInstall(packages, distro);
}



// Start dependencies check/install if not already started (or force re-run).
void Installwizard::maybeStartDependenciesCheck(bool force /*=false*/)
{
    // Already satisfied and not forcing? Nothing to do.
    if (!force && (depsOk_ || depsCheckStarted_))
        return;

    depsCheckStarted_ = true;        // mark so we don't auto-run twice
    depsOk_ = false;                 // pessimistically gate Page-1 Next
    if (currentId() == 0)
        setWizardButtonEnabled(QWizard::NextButton, false);

    // Reuse your existing "installDependencies()" which calls runDependenciesInstall(...)
    // (If you renamed it, call that instead.)
    installDependencies();
}

QStringList Installwizard::getAvailableDrives() {
    QProcess process;
    process.start("lsblk", QStringList() << "-o" << "NAME,SIZE,TYPE" << "-d" << "-n");
    process.waitForFinished();
    QString output = process.readAllStandardOutput();

    QStringList drives;
    for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
        QStringList tokens = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (tokens.size() >= 3 && tokens[2] == "disk") {
            QString deviceName = tokens[0];
            if (!deviceName.startsWith("loop"))
                drives << deviceName;
        }
    }
    return drives;
}

void Installwizard::populateDrives() {
    ui->driveDropdown->clear();
    QStringList drives = getAvailableDrives();
    if (drives.isEmpty()) {
        ui->driveDropdown->addItem("No drives found");
        ui->treePartitions->clear();
    } else {
        for (const QString &drive : std::as_const(drives))
            ui->driveDropdown->addItem(QString("/dev/%1").arg(drive));
        populatePartitionTable(drives.first());
    }
}

void Installwizard::onPartitionSelected(const QModelIndex &index) {
    QTreeWidgetItem *item = ui->treePartitions->currentItem();
    if (!item) return;

    const QString type = item->text(2);

    if (type.compare("free", Qt::CaseInsensitive) == 0) {
        bool ok1=false, ok2=false;
        const double startMiB = item->data(0, Qt::UserRole + 1).toDouble(&ok1);
        const double endMiB   = item->data(0, Qt::UserRole + 2).toDouble(&ok2);
        targetPartition.clear(); // not a partition path
        appendLog(QString("Free-space selected: %1 MiB → %2 MiB")
                      .arg(ok1 ? startMiB : -1, 0, 'f', 0)
                      .arg(ok2 ? endMiB   : -1, 0, 'f', 0));
        return;
    }

    // Partition/device path
    QString partitionPath = item->text(0);
    if (!partitionPath.startsWith("/dev/") && !partitionPath.isEmpty())
        partitionPath = "/dev/" + partitionPath;
    targetPartition = partitionPath;
    appendLog(QString("Partition selected: %1").arg(targetPartition));
}

void Installwizard::populatePartitionTable(const QString &drive) {
    if (drive.isEmpty()) return;

    // Inline helper: normalize to parent disk (/dev/sda, /dev/nvme0n1, etc.)
    auto toDisk = [](const QString &path) -> QString {
        QString dev = path;
        if (!dev.startsWith("/dev/")) dev = "/dev/" + dev;

        QRegularExpression rxNvme("^(/dev/nvme\\d+n\\d+)p\\d+$");   // /dev/nvme0n1p3 -> /dev/nvme0n1
        QRegularExpression rxMmc("^(/dev/mmcblk\\d+)p\\d+$");       // /dev/mmcblk0p2 -> /dev/mmcblk0
        QRegularExpression rxSd("^(/dev/[shv]d[a-zA-Z]+)\\d+$");    // /dev/sda3 -> /dev/sda ; /dev/vda2 -> /dev/vda
        QRegularExpression rxXvd("^(/dev/xvd[a-zA-Z]+)\\d+$");      // /dev/xvda1 -> /dev/xvda

        for (const QRegularExpression &rx : {rxNvme, rxMmc, rxSd, rxXvd}) {
            auto m = rx.match(dev);
            if (m.hasMatch()) return m.captured(1);
        }
        return dev; // already a disk or unknown pattern
    };

    // Setup the widget
    ui->treePartitions->clear();
    if (ui->treePartitions->columnCount() < 4)
        ui->treePartitions->setColumnCount(4);
    ui->treePartitions->setHeaderLabels(QStringList() << "Name" << "Size" << "Type" << "Mount");

    const QString deviceShown     = drive.startsWith("/dev/") ? drive : ("/dev/" + drive);
    const QString deviceForParted = toDisk(deviceShown);   // parent disk for both lsblk and parted

    auto addRow = [&](const QString &name,
                      const QString &size,
                      const QString &type,
                      const QString &mount,
                      bool isFree = false,
                      double startMiB = -1.0,
                      double endMiB   = -1.0)
    {
        auto *item = new QTreeWidgetItem(ui->treePartitions);
        item->setText(0, name);
        item->setText(1, size);
        item->setText(2, type);
        item->setText(3, mount);

        if (isFree) {
            item->setData(0, Qt::UserRole, true);          // mark as free
            item->setData(0, Qt::UserRole + 1, startMiB);  // start MiB
            item->setData(0, Qt::UserRole + 2, endMiB);    // end MiB
            item->setFlags(item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        }
    };

    // 1) Existing disk/partitions via lsblk (include UNMOUNTED by accepting 3-col rows)
    {
        QProcess p;
        p.start("lsblk", QStringList() << "-r" << "-n" << "-o"
                                       << "NAME,SIZE,TYPE,MOUNTPOINT"
                                       << deviceForParted);
        p.waitForFinished(-1);

        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);

        for (const QString &line : lines) {
            const QStringList cols = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            // Accept 4 columns (mounted) or 3 columns (unmounted)
            if (cols.size() == 4 || cols.size() == 3) {
                QString name = cols.at(0);
                if (!name.startsWith("/dev/")) name = "/dev/" + name;

                const QString size  = (cols.size() >= 2) ? cols.at(1) : "?";
                const QString type  = (cols.size() >= 3) ? cols.at(2) : "?";
                const QString mount = (cols.size() == 4) ? cols.at(3) : "unmounted";

                addRow(name, size, type, mount);
            }
            // else: ignore malformed line
        }
    }

    // 2) Free-space extents via parted (your robust parser, unchanged)
    {
        QProcess p;
        p.start("parted", QStringList() << deviceForParted << "-m" << "unit" << "MiB" << "print" << "free");
        p.waitForFinished(-1);

        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        const QString err = QString::fromUtf8(p.readAllStandardError());

        // Log parted output so we can see what it returned
        appendLog(QString("[parted] device=%1").arg(deviceForParted));
        if (!err.trimmed().isEmpty()) appendLog(QString("[parted stderr]\n%1").arg(err.trimmed()));
        appendLog(QString("[parted stdout]\n%1").arg(out.trimmed().isEmpty() ? "<empty>" : out.trimmed()));

        int freeCount = 0;
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);

        for (QString l : lines) {
            l = l.trimmed();
            if (l.isEmpty()) continue;
            if (l.startsWith("BYT")) continue;         // header
            if (l.startsWith("/dev/")) continue;       // device summary line

            // Rows appear like:
            // <num or 'free'>:<startMiB>:<endMiB>:<sizeMiB>:<...>:free;
            if (l.endsWith(';')) l.chop(1);
            QStringList cols = l.split(':');

            if (cols.size() < 4) continue;

            // Treat as FREE if any field equals "free" (case-insensitive)
            bool isFreeRow = false;
            for (const QString &c : cols) {
                if (c.compare("free", Qt::CaseInsensitive) == 0) { isFreeRow = true; break; }
            }
            if (!isFreeRow) continue;

            auto toMiB = [](QString s) -> double {
                s.remove(QRegularExpression("[^0-9\\.]"));
                return s.toDouble();
            };

            const double startMiB = toMiB(cols[1]);
            const double endMiB   = toMiB(cols[2]);
            const double sizeMiB  = toMiB(cols[3]);
            if (!(startMiB > 0.0 && endMiB > startMiB && sizeMiB > 1.0)) continue;

            const QString human = (sizeMiB >= 1024.0)
                                      ? QString::number(sizeMiB / 1024.0, 'f', 1) + "G"
                                      : QString::number(sizeMiB, 'f', 0) + "M";
            const QString label = QString("free %1MiB–%2MiB")
                                      .arg((qulonglong)startMiB)
                                      .arg((qulonglong)endMiB);

            addRow(label, human, "free", "", /*isFree*/true, startMiB, endMiB);
            ++freeCount;
        }

        if (freeCount == 0) {
            appendLog("No free-space extents detected by parser (rows lacked 'free' token or were < 1 MiB).");
        }
    }

    ui->treePartitions->expandAll();
    for (int c = 0; c < ui->treePartitions->columnCount(); ++c)
        ui->treePartitions->resizeColumnToContents(c);
}

void Installwizard::prepareForEfi(const QString &drive)
{
    // EFI should honor the same 3 modes as legacy. Only wipe when user chose it.
    efiInstall = true;

    const QString selectedMode = ui->comboInstallMode->currentText();
    appendLog(QString("DEBUG (EFI): comboInstallMode currentText = '%1'").arg(selectedMode));

    if (selectedMode == "Use selected partition") {
        // Must have a valid target partition
        if (targetPartition.isEmpty() || targetPartition == "/dev/") {
            QMessageBox::warning(this, "Error", "Please select a valid partition.");
            return;
        }
        // Reuse the partition flow (it creates the worker, wires signals, etc.)
        prepareExistingPartition(targetPartition);
        return;
    }

    // For "Erase entire drive" and "Use free space", operate on the drive
    QString driveText = drive;
    if (driveText.isEmpty()) {
        driveText = ui->driveDropdown->currentText();
        if (driveText.isEmpty() || driveText == "No drives found") {
            QMessageBox::warning(this, "Error", "Please select a valid drive.");
            return;
        }
    }

    // Normalize /dev/ prefix if present (your code elsewhere does this)
    if (driveText.startsWith("/dev/"))
        driveText = driveText.mid(5);

    // Reuse the drive-prep path (it sets mode from combo and passes efiInstall=true)
    prepareDrive(driveText);
}

void Installwizard::on_installButton_clicked() {
    QString username = ui->lineEditUsername->text().trimmed();
    QString password = ui->lineEditPassword->text();
    QString passwordAgain = ui->lineEditPasswordAgain->text();

    QString rootPassword = ui->lineEditRootPassword->text();
    QString rootPasswordAgain = ui->lineEditRootPasswordAgain->text();

    QString desktopEnv = ui->comboDesktopEnvironment->currentText();

    // Defensive: repopulate desktop envs if needed
    if (ui->comboDesktopEnvironment->count() == 0) {
        ui->comboDesktopEnvironment->addItems(
            {"GNOME", "KDE Plasma", "XFCE", "LXQt", "Cinnamon", "MATE", "i3"});
    }

    // Validate user inputs
    if (username.isEmpty() || password.isEmpty() || rootPassword.isEmpty()) {
        QMessageBox::warning(this, "Input Error", "Please fill out all fields.");
        return;
    }
    if (password != passwordAgain) {
        QMessageBox::warning(this, "Password Mismatch", "User passwords do not match.");
        return;
    }
    if (rootPassword != rootPasswordAgain) {
        QMessageBox::warning(this, "Password Mismatch", "Root passwords do not match.");
        return;
    }
    if (desktopEnv.isEmpty()) {
        QMessageBox::warning(this, "Desktop Environment", "Please select a desktop environment.");
        return;
    }

    // --- Threaded system install using SystemWorker ---
    SystemWorker *worker = new SystemWorker;
    worker->setParameters(
        selectedDrive,
        username, password, rootPassword,
        desktopEnv,
        efiInstall // true if EFI install, else legacy
        );

    setWizardButtonEnabled(QWizard::FinishButton, false); // can't finish until install completes

    QThread *thread = new QThread;
    worker->moveToThread(thread);
    appendLog("Starting system installation…");

    connect(thread, &QThread::started, worker, &SystemWorker::run);
    connect(worker, &SystemWorker::logMessage, this, [this](const QString &msg) {
        appendLog(msg);
    });
    connect(worker, &SystemWorker::errorOccurred, this, [this](const QString &msg) {
        QMessageBox::critical(this, "Error", msg);
    });
    connect(worker, &SystemWorker::finished, this, [this]() {
        appendLog("✔️ Installation complete.");
        setWizardButtonEnabled(QWizard::FinishButton, true);
        QMessageBox::information(this, "Complete", "System installation finished.");
    });
    connect(worker, &SystemWorker::finished, thread, &QThread::quit);
    connect(worker, &SystemWorker::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    thread->start();
}
