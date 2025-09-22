#include "installerworker.h"
#include <QProcess>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QRegularExpression>
#include <QSet>
#include <QChar>
#include <QJsonDocument>
#include <QJsonObject>
#include "Installwizard.h"

// --- Helper to locate parted ---
static QString locatePartedBinary() {
    QString p = QStandardPaths::findExecutable("parted");
    if (!p.isEmpty()) return p;
    const QStringList fallbacks{"/usr/sbin/parted", "/sbin/parted"};
    for (const QString &path : fallbacks)
        if (QFileInfo::exists(path))
            return path;
    return QString();
}

static QString targetStateFilePath()
{
    return QStringLiteral("/tmp/archaid-target.json");
}

static void recordTargetMountState(const QString &rootDev, const QString &espDev)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("root"), rootDev);
    if (!espDev.isEmpty())
        obj.insert(QStringLiteral("esp"), espDev);

    QFile f(targetStateFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "Failed to persist target mount state:" << f.errorString();
        return;
    }

    const QJsonDocument doc(obj);
    f.write(doc.toJson(QJsonDocument::Compact));
    f.write("\n");
    f.close();
}

// Return child partition kernel names for devPath (e.g. "sdb1", "nvme0n1p2")
static QSet<QString> childPartitionsSet(const QString &devPath)
{
    QString base = devPath.startsWith("/dev/") ? devPath.mid(5) : devPath;
    QSet<QString> out;

    QProcess p;
    p.start("lsblk", QStringList() << "-ln" << "-o" << "NAME,TYPE,PKNAME");
    p.waitForFinished();
    const QStringList rows = QString::fromUtf8(p.readAllStandardOutput())
                                 .split('\n', Qt::SkipEmptyParts);

    for (const QString &r : rows) {
        const QStringList cols = r.split(QRegularExpression("\\s+"),
                                         Qt::SkipEmptyParts);
        if (cols.size() < 3) continue;
        const QString name = cols[0], type = cols[1], pk = cols[2];
        if (type == "part" && pk == base) out.insert(name);
    }
    return out;
}

// Is this partition already VFAT/FAT32? (Used to validate existing ESP)
static bool isPartitionVfat(const QString &partPath)
{
    QProcess p;
    p.start("lsblk", QStringList() << "-no" << "FSTYPE" << partPath);
    p.waitForFinished();
    const QString fstype = QString::fromUtf8(p.readAllStandardOutput()).trimmed().toLower();
    return (fstype == "vfat" || fstype == "fat32" || fstype == "msdos");
}


// Detect exactly one newly created partition by diffing lsblk before/after.
// 'before' is the set of child partition kernel names (e.g., {"sdb1","sdb3"})
// taken before mkpart. Returns full /dev node (e.g., "/dev/sdb4") or empty on ambiguity.
static QString detectNewPartitionNode(const QString &devPath, const QSet<QString> &before)
{
    QSet<QString> after = childPartitionsSet(devPath);
    QSet<QString> diff  = after - before;
    if (diff.size() != 1)
        return QString(); // none or more than one new partition -> ambiguous
    const QString kernelName = *diff.begin();
    return "/dev/" + kernelName;
}

// Build a partition node name for a given base (sda, nvme0n1, mmcblk0) and number.
static QString partitionNodeFor(const QString &baseName, int partNum) {
    // nvme0n1 -> nvme0n1p1 ; mmcblk0 -> mmcblk0p1 ; sda -> sda1
    if (!baseName.isEmpty() && baseName.back().isDigit())
        return "/dev/" + baseName + "p" + QString::number(partNum);
    return "/dev/" + baseName + QString::number(partNum);
}

// Find an existing EFI System Partition (ESP) on this disk. Returns full /dev/… path or empty string.
static QString findExistingEsp(const QString &partedBin, const QString &devPath) {
    // 1) Try lsblk with PARTTYPE/PARTLABEL/FSTYPE (most robust).
    QProcess p;
    p.start("lsblk", QStringList() << "-ln" << "-o" << "NAME,TYPE,PKNAME,PARTTYPE,PARTLABEL,FSTYPE");
    p.waitForFinished();
    const QStringList rows = QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);

    QString base = devPath.startsWith("/dev/") ? devPath.mid(5) : devPath;
    const QString espGuid = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"; // EFI System Partition

    for (const QString &r : rows) {
        const QStringList cols = r.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (cols.size() < 6) continue;
        const QString name     = cols[0]; // e.g. sdb1 or nvme0n1p1
        const QString type     = cols[1]; // "part"
        const QString pk       = cols[2]; // parent kernel name (e.g. sdb)
        const QString parttype = cols[3].toLower();
        const QString label    = cols[4].toLower();
        const QString fstype   = cols[5].toLower();

        if (type == "part" && pk == base) {
            if (parttype == espGuid ||
                label.contains("esp") ||
                label.contains("efi system") ||
                fstype == "vfat" || fstype == "fat32") {
                return "/dev/" + name;
            }
        }
    }

    // 2) Fallback to parted flags.
    QProcess pp;
    pp.start("sudo", QStringList{partedBin, devPath, "-m", "unit", "MiB", "print"});
    pp.waitForFinished();
    const QStringList plines = QString::fromUtf8(pp.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    for (const QString &line : plines) {
        // parted -m lines look like: number:start:end:size:fs:name:flags
        const QStringList cols = line.split(':');
        if (cols.size() >= 7) {
            const QString number = cols[0];
            const QString flags  = cols[6];
            if (!number.isEmpty() && number[0].isDigit() && flags.contains("esp", Qt::CaseInsensitive)) {
                const int n = number.toInt();
                return partitionNodeFor(base, n);
            }
        }
    }

    return QString(); // none found
}

// Find an existing bios_grub partition (GPT BIOS boot partition) on this disk.
// Returns full /dev/… path or empty string if none is present.
static QString findExistingBiosGrub(const QString &partedBin, const QString &devPath)
{
    QProcess p;
    p.start("sudo", QStringList{partedBin, devPath, "-m", "unit", "MiB", "print"});
    p.waitForFinished();
    const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);

    QString base = devPath.startsWith("/dev/") ? devPath.mid(5) : devPath;

    for (const QString &line : lines) {
        const QStringList cols = line.split(':');
        if (cols.size() < 7)
            continue;

        const QString number = cols.at(0);
        if (number.isEmpty() || !number[0].isDigit())
            continue;

        const QString flags = cols.at(6).toLower();
        if (flags.contains("bios_grub")) {
            bool ok = false;
            const int idx = number.toInt(&ok);
            if (ok)
                return partitionNodeFor(base, idx);


            return partitionNodeFor(base, number.toInt());
        }
    }

    return QString();
}

// Return the base kernel name for /dev/sdX or /dev/nvme0n1
static QString baseNameOf(const QString &devPath) {
    return devPath.startsWith("/dev/") ? devPath.mid(5) : devPath;
}

// Determine the base disk for a node (walks /dev/mapper, dm-*, partitions → disk)
static QString resolveBaseDisk(const QString &devOrMapper)
{
    QString cur = devOrMapper;
    if (!cur.startsWith("/dev/"))
        return QString(); // unexpected

    // Walk up PKNAME until TYPE == "disk"
    for (int hop = 0; hop < 6; ++hop) { // depth guard for dm-crypt→lvm→part→disk chains
        QProcess p;
        p.start("lsblk", QStringList() << "-ln" << "-o" << "NAME,TYPE,PKNAME" << cur);
        p.waitForFinished();
        const QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        if (out.isEmpty()) break;

        const QStringList cols = out.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (cols.size() >= 2) {
            const QString name = cols.at(0);
            const QString type = cols.at(1);
            const QString pk   = (cols.size() >= 3 ? cols.at(2) : QString());
            if (type == "disk")
                return "/dev/" + name;
            if (!pk.isEmpty()) {
                cur = "/dev/" + pk;
                continue;
            }
        }
        break;
    }
    return QString();
}

// Where is "/" mounted from? (handles UUID=/LABEL= via blkid)
static QString rootSourceDevice()
{
    QProcess p;
    p.start("findmnt", QStringList() << "-no" << "SOURCE" << "/");
    p.waitForFinished();
    QString src = QString::fromUtf8(p.readAllStandardOutput()).trimmed();

    if (src.isEmpty()) {
        QFile f("/proc/mounts");
        if (f.open(QIODevice::ReadOnly|QIODevice::Text)) {
            while (!f.atEnd()) {
                const QByteArray line = f.readLine();
                const QList<QByteArray> cols = line.split(' ');
                if (cols.size() >= 2 && cols.at(1) == "/") {
                    src = QString::fromUtf8(cols.at(0));
                    break;
                }
            }
        }
    }

    if (!src.startsWith("/dev/") && (src.startsWith("UUID=") || src.startsWith("LABEL="))) {
        QProcess r;
        if (src.startsWith("UUID=")) {
            r.start("blkid", QStringList() << "-U" << src.mid(5));
        } else {
            r.start("blkid", QStringList() << "-L" << src.mid(6));
        }
        r.waitForFinished();
        const QString dev = QString::fromUtf8(r.readAllStandardOutput()).trimmed();
        if (dev.startsWith("/dev/"))
            src = dev;
    }
    return src;
}

// Is devPath ("/dev/sdX", "/dev/nvme0n1", etc.) the disk that backs our running root?
static bool isSystemDisk(const QString &devPath)
{
    const QString rootSrc  = rootSourceDevice();
    if (rootSrc.isEmpty()) return false;

    const QString rootDisk = resolveBaseDisk(rootSrc);
    const QString targetDisk = resolveBaseDisk(devPath);
    if (rootDisk.isEmpty() || targetDisk.isEmpty()) return false;

    return QFileInfo(rootDisk).canonicalFilePath() ==
           QFileInfo(targetDisk).canonicalFilePath();
}

// Safer preflight unmounts: always clean our staging, but never touch host mounts.
static void safePreflightUnmounts(const QString &devPath)
{
    // Always clean our staging
    QProcess::execute("sudo", {"umount", "-Rl", "/mnt/boot/efi"});
    QProcess::execute("sudo", {"umount", "-Rl", "/mnt/boot"});
    QProcess::execute("sudo", {"umount", "-Rl", "/mnt"});

    // If this is the system disk, stop here — do not touch host mounts.
    if (isSystemDisk(devPath)) {
        QProcess::execute("sudo", {"udevadm", "settle"});
        return;
    }

    // For non-system disks, unmount only "external" mountpoints on this disk.
    QProcess p;
    p.start("lsblk", QStringList() << "-ln" << "-o" << "NAME,TYPE,MOUNTPOINT" << devPath);
    p.waitForFinished();
    const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList cols = line.split(QRegularExpression("\\s+"));
        if (cols.size() >= 2 && cols[1] == "part") {
            const QString node = "/dev/" + cols[0];
            const QString mp   = (cols.size() >= 3 ? cols[2] : QString());
            if (!mp.isEmpty() && (mp.startsWith("/media/") || mp.startsWith("/run/media/") || mp.startsWith("/mnt/"))) {
                QProcess::execute("sudo", {"umount", "-l", node});
            }
        }
    }
    QProcess::execute("sudo", {"udevadm", "settle"});
}

// Strong device-detach to avoid "resource busy", BUT safe on the system disk.
static void bestEffortDetachDevice(const QString &devPath)
{
    // Always clean our staging points first
    QProcess::execute("sudo", {"umount", "-Rl", "/mnt/boot/efi"});
    QProcess::execute("sudo", {"umount", "-Rl", "/mnt/boot"});
    QProcess::execute("sudo", {"umount", "-Rl", "/mnt"});

    // If the target is the disk that hosts "/", DO NOT try to unmount/kill holders on it.
    if (isSystemDisk(devPath)) {
        qWarning() << "[detach] Target is system disk; skipping device-wide unmounts/kills for" << devPath;
        QProcess::execute("sudo", {"udevadm", "settle"});
        return;
    }

    // Non-system disk: proceed with a thorough detach.
    const QSet<QString> parts = childPartitionsSet(devPath);

    // 1) Ask udisks to unmount anything user-mounted from this disk
    for (const QString &pn : parts)
        QProcess::execute("sudo", {"udisksctl", "unmount", "-b", "/dev/" + pn});

    // 2) Swapoff any swap partitions on this disk
    {
        QProcess psw; psw.start("cat", QStringList() << "/proc/swaps"); psw.waitForFinished();
        const QString swaps = QString::fromUtf8(psw.readAllStandardOutput());
        for (const QString &pn : parts)
            if (swaps.contains("/dev/" + pn))
                QProcess::execute("sudo", {"swapoff", "/dev/" + pn});
    }

    // 3) Close any LUKS mappings whose PKNAME is on this disk
    {
        QProcess pl; pl.start("lsblk", QStringList() << "-ln" << "-o" << "NAME,TYPE,PKNAME");
        pl.waitForFinished();
        const QStringList rows = QString::fromUtf8(pl.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        const QSet<QString> partSet = parts; // fast lookup
        for (const QString &r : rows) {
            const QStringList cols = r.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (cols.size() < 3) continue;
            const QString name = cols[0], type = cols[1], pk = cols[2];
            if (type == "crypt" && (partSet.contains(pk))) {
                QProcess::execute("sudo", {"cryptsetup", "close", "/dev/" + name});
            }
        }
    }

    // 4) Deactivate LVM VGs that live on this disk
    {
        QSet<QString> vgs;
        QProcess pl; pl.start("lsblk", QStringList() << "-ln" << "-o" << "NAME,TYPE,PKNAME");
        pl.waitForFinished();
        const QStringList rows = QString::fromUtf8(pl.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        const QSet<QString> partSet = parts;
        for (const QString &r : rows) {
            const QStringList cols = r.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (cols.size() < 3) continue;
            const QString name = cols[0], type = cols[1], pk = cols[2];
            if ((type == "lvm" || type == "dm") && partSet.contains(pk)) {
                QProcess g; g.start("lvs", QStringList() << "--noheadings" << "-o" << "vg_name" << ("/dev/" + name));
                g.waitForFinished();
                const QString vg = QString::fromUtf8(g.readAllStandardOutput()).trimmed();
                if (!vg.isEmpty()) vgs.insert(vg);
            }
        }
        for (const QString &vg : vgs)
            QProcess::execute("sudo", {"vgchange", "-an", vg});
    }

    // 5) Kill remaining holders (safe here because we verified it's NOT the system disk)
    {
        QStringList nodes; nodes << devPath;
        for (const QString &pn : parts) nodes << ("/dev/" + pn);
        for (const QString &n : nodes)
            QProcess::execute("sudo", {"fuser", "-km", n}); // kill processes using n
    }

    // 6) Remove any dm holders
    for (const QString &pn : parts) {
        const QString holdersPath = "/sys/class/block/" + pn + "/holders";
        QDir holders(holdersPath);
        if (holders.exists()) {
            for (const QString &h : holders.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
                QProcess::execute("sudo", {"dmsetup", "remove", "-f", h});
        }
    }

    // 7) Reread table & settle; and (best-effort) power-off if removable
    QProcess::execute("sudo", {"blockdev", "--rereadpt", devPath});
    QProcess::execute("sudo", {"partprobe", devPath});
    QProcess::execute("sudo", {"udevadm", "settle"});
    QThread::sleep(1);
    QProcess::execute("sudo", {"udisksctl", "power-off", "-b", devPath}); // best-effort; harmless if non-removable
}

// Parse parted's "MiB" strings which may be decimal (e.g. "1.00MiB").
// Produces safe integer bounds: start = ceil(startMiB), end = floor(endMiB)
// Returns false if parsing failed or range is empty.
static bool parseMiBToBounds(const QString &startStrIn,
                             const QString &endStrIn,
                             long long &startMiB,
                             long long &endMiB)
{
    auto norm = [](QString s) -> QString {
        QString t = s;
        t.remove("MiB", Qt::CaseInsensitive);
        t.replace(',', '.');
        return t.trimmed();
    };

    bool ok1 = false, ok2 = false;
    double s = norm(startStrIn).toDouble(&ok1);
    double e = norm(endStrIn).toDouble(&ok2);
    if (!ok1 || !ok2) return false;

    // Ceil start, floor end so we never exceed exclusive end boundary
    startMiB = static_cast<long long>(std::ceil(s));
    endMiB   = static_cast<long long>(std::floor(e));

    // GPT typically starts usable space at 1MiB; clamp defensively
    if (startMiB < 1) startMiB = 1;

    return startMiB < endMiB;
}


// Extract trailing partition number from a device path.
// Examples:
//   /dev/sda3        -> "3"
//   /dev/nvme0n1p2   -> "2"
//   /dev/mmcblk0p1   -> "1"
// Returns empty QString if no trailing digits are present.
static QString partitionNumberFromPath(const QString &partPath)
{
    // Take last path component (handles /dev/sdb3 or /dev/disk/by-id/... -> ...-part3)
    QString name = partPath.section('/', -1);

    // Find the run of digits at the end
    int i = name.size() - 1;
    while (i >= 0 && name[i].isDigit())
        --i;

    if (i == name.size() - 1) {
        // No trailing digits -> not a partition node
        return QString();
    }
    return name.mid(i + 1);
}



InstallerWorker::InstallerWorker(QObject *parent) : QObject(parent) {}

void InstallerWorker::setDrive(const QString &drive) { selectedDrive = drive; }
void InstallerWorker::setMode(InstallMode m) { mode = m; }
void InstallerWorker::setTargetPartition(const QString &part) { targetPartition = part; }
void InstallerWorker::setEfiInstall(bool efi) { efiInstall = efi; }

void InstallerWorker::wipeDriveAndPartition(QProcess &process, const QString &partedBin, const QString &devPath)
{
    emit logMessage(QString("Preparing drive for %1 wipe (GPT)...").arg(efiInstall ? "EFI" : "BIOS/GRUB"));

    // Detach anything holding the disk
    bestEffortDetachDevice(devPath);

    // Extra safety: wipe signatures; zap any lingering GPT
    QProcess::execute("sudo", {"wipefs", "-a", devPath});
    const QString sgdisk = QStandardPaths::findExecutable("sgdisk");
    if (!sgdisk.isEmpty()) {
        QProcess::execute("sudo", {sgdisk, "--zap-all", "--clear", devPath});
    }

    QProcess::execute("sudo", {"blockdev", "--rereadpt", devPath});
    QProcess::execute("sudo", {"udevadm", "settle"});
    QThread::sleep(1);

    // Create GPT label
    if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mklabel", "gpt"}) != 0) {
        emit errorOccurred("Failed to create GPT partition table.");
        return;
    }
    QProcess::execute("sudo", {"partprobe", devPath});
    QProcess::execute("sudo", {"udevadm", "settle"});
    QThread::sleep(1);

    // Partition layout
    if (efiInstall) {
        // ESP: 1MiB-513MiB, root: 513MiB-end-1
        const QString espStart = "1MiB";
        const QString espEnd   = "513MiB";

        // Determine last MiB by asking parted for disk size
        process.start("sudo", {partedBin, devPath, "-m", "unit", "MiB", "print"});
        process.waitForFinished();
        long long diskEndMiB = 0;
        for (const QString &line : QString::fromUtf8(process.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
            // disk line example: /dev/sdb:51200MiB:scsi:512:512:gpt:...
            if (line.startsWith(devPath + ":")) {
                const QStringList cols = line.split(':');
                if (cols.size() >= 2) {
                    QString sz = cols[1];
                    sz.remove("MiB");
                    diskEndMiB = sz.toLongLong();
                }
            }
        }
        if (diskEndMiB <= 0) { emit errorOccurred("Could not determine disk size."); return; }
        const QString rootStart = espEnd;
        const QString rootEnd   = QString::number(diskEndMiB - 1) + "MiB";

        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "fat32", espStart, espEnd}) != 0 ||
            QProcess::execute("sudo", {partedBin, devPath, "--script", "name", "1", "ESP"}) != 0 ||
            QProcess::execute("sudo", {partedBin, devPath, "--script", "set",  "1", "esp", "on"}) != 0) {
            emit errorOccurred("Failed to create/flag ESP.");
            return;
        }
        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "ext4", rootStart, rootEnd}) != 0) {
            emit errorOccurred("Failed to create root partition.");
            return;
        }

        QProcess::execute("sudo", {"partprobe", devPath});
        QProcess::execute("sudo", {"udevadm", "settle"});
        QThread::sleep(1);

        // Detect devices (assume #1 is ESP, last is root)
        process.start("lsblk", QStringList() << "-ln" << "-o" << "NAME" << devPath);
        process.waitForFinished();
        const QStringList parts = QString::fromUtf8(process.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        if (parts.size() < 2) { emit errorOccurred("Could not detect created partitions."); return; }
        const QString espPart  = "/dev/" + parts.first();
        const QString rootPart = "/dev/" + parts.last();

        // Format + mount
        if (QProcess::execute("sudo", {"mkfs.fat", "-F32", espPart}) != 0) { emit errorOccurred("Failed to format ESP."); return; }
        if (QProcess::execute("sudo", {"mkfs.ext4", "-F", rootPart}) != 0) { emit errorOccurred("Failed to format root."); return; }
        QProcess::execute("sudo", {"e2fsck", "-f", rootPart});

        emit logMessage("Mounting new partitions...");
        QProcess::execute("sudo", {"mount", rootPart, "/mnt"});
        QProcess::execute("sudo", {"mkdir", "-p", "/mnt/boot/efi"});
        QProcess::execute("sudo", {"mount", espPart, "/mnt/boot/efi"});
        recordTargetMountState(rootPart, espPart);
        emit installComplete();
        return;
    } else {
        // BIOS/GRUB on GPT: bios_grub (1 MiB) + root to end-1
        const QString biosStart = "1MiB";
        const QString biosEnd   = "2MiB";

        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", biosStart, biosEnd}) != 0) {
            emit errorOccurred("Failed to create bios_grub partition.");
            return;
        }
        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "set", "1", "bios_grub", "on"}) != 0) {
            emit errorOccurred("Failed to set bios_grub flag.");
            return;
        }

        // Compute disk end and create root
        process.start("sudo", {partedBin, devPath, "-m", "unit", "MiB", "print"});
        process.waitForFinished();
        long long diskEndMiB = 0;
        for (const QString &line : QString::fromUtf8(process.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
            if (line.startsWith(devPath + ":")) {
                const QStringList cols = line.split(':');
                if (cols.size() >= 2) {
                    QString sz = cols[1]; sz.remove("MiB");
                    diskEndMiB = sz.toLongLong();
                }
            }
        }
        if (diskEndMiB <= 0) { emit errorOccurred("Could not determine disk size."); return; }

        const QString rootStart = "2MiB";
        const QString rootEnd   = QString::number(diskEndMiB - 1) + "MiB";

        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "ext4", rootStart, rootEnd}) != 0) {
            emit errorOccurred("Failed to create root partition.");
            return;
        }

        QProcess::execute("sudo", {"partprobe", devPath});
        QProcess::execute("sudo", {"udevadm", "settle"});
        QThread::sleep(1);

        // Detect root (assume last)
        process.start("lsblk", QStringList() << "-ln" << "-o" << "NAME" << devPath);
        process.waitForFinished();
        const QStringList parts = QString::fromUtf8(process.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        if (parts.isEmpty()) { emit errorOccurred("Could not detect created root partition."); return; }
        const QString rootPart = "/dev/" + parts.last();

        if (QProcess::execute("sudo", {"mkfs.ext4", "-F", rootPart}) != 0) { emit errorOccurred("Failed to format root."); return; }
        QProcess::execute("sudo", {"e2fsck", "-f", rootPart});

        emit logMessage("Mounting root partition...");
        QProcess::execute("sudo", {"mount", rootPart, "/mnt"});
        recordTargetMountState(rootPart, QString());
        emit installComplete();
        return;
    }
}

bool InstallerWorker::getPartitionGeometry(const QString &targetPartition, const QString &selectedDrive, QString &startMiB, QString &endMiB) {
    QProcess process;
    QString partName = targetPartition;
    if (partName.startsWith("/dev/"))
        partName = partName.mid(5);
    QRegularExpression partNumRe("(\\d+)$");
    QRegularExpressionMatch match = partNumRe.match(partName);
    if (!match.hasMatch()) {
        emit logMessage("DEBUG: Could not extract partition number from " + partName);
        return false;
    }
    QString partNum = match.captured(1);
    emit logMessage(QString("DEBUG: Extracted partition number: %1").arg(partNum));

    process.start("sudo", QStringList() << "parted" << "/dev/" + selectedDrive << "unit" << "MiB" << "print");
    process.waitForFinished();
    QString output = process.readAllStandardOutput();
    emit logMessage(QString("DEBUG: parted output:\n%1").arg(output));

    QRegularExpression partLineRe("^\\s*" + partNum + "\\s+([0-9.]+)MiB\\s+([0-9.]+)MiB", QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator it = partLineRe.globalMatch(output);

    emit logMessage(QString("DEBUG: parted output:\n%1").arg(output));

    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        QString line = match.captured(0);
        if (line.trimmed().startsWith(partNum)) {
            startMiB = match.captured(1);
            endMiB = match.captured(2);
            emit logMessage(QString("DEBUG: Partition geometry startMiB=%1 endMiB=%2").arg(startMiB, endMiB));
            return true;
        }
    }

    emit logMessage("DEBUG: Could not parse partition geometry for partition number " + partNum);
    return false;
}

void InstallerWorker::recreateFromSelectedPartition(QProcess &process, const QString &partedBin, const QString &devPath)
{
    // Query geometry before deletion
    QString startStr, endStr;
    if (!getPartitionGeometry(targetPartition, selectedDrive, startStr, endStr)) {
        emit errorOccurred("Could not query selected partition geometry (parted).");
        return;
    }
    long long startMiB = 0, endMiB = 0;
    if (!parseMiBToBounds(startStr, endStr, startMiB, endMiB)) {
        emit errorOccurred("Invalid geometry for selected partition.");
        return;
    }
    if (efiInstall) {
        const QString existingEsp = findExistingEsp(partedBin, devPath);
        if (!existingEsp.isEmpty() &&
            QFileInfo(targetPartition).canonicalFilePath() == QFileInfo(existingEsp).canonicalFilePath()) {
            emit errorOccurred("Selected partition is the EFI System Partition. Please choose a different partition for root.");
            return;
        }
    }

    // Unmount and delete selected partition
    QProcess::execute("sudo", {"umount", "-l", targetPartition});
    QString partNum = partitionNumberFromPath(targetPartition);
    if (partNum.isEmpty()) { emit errorOccurred("Could not determine selected partition number."); return; }
    if (QProcess::execute("sudo", {partedBin, devPath, "--script", "rm", partNum}) != 0) {
        emit errorOccurred("Failed to delete selected partition.");
        return;
    }
    QProcess::execute("sudo", {"partprobe", devPath});
    QProcess::execute("sudo", {"udevadm", "settle"});
    QThread::sleep(1);

    QString espPart, rootPart;

    if (efiInstall) {
        const QString existingEsp = findExistingEsp(partedBin, devPath);
        if (!existingEsp.isEmpty()) {
            // Reuse existing ESP; create root in freed region, detect by diff
            const QSet<QString> before = childPartitionsSet(devPath);

            const QString rootStart = QString::number(startMiB) + "MiB";
            const QString rootEnd   = QString::number(endMiB - 1) + "MiB";
            if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "ext4", rootStart, rootEnd}) != 0) {
                emit errorOccurred("Failed to create root (existing partition).");
                return;
            }

            QProcess::execute("sudo", {"partprobe", devPath});
            QProcess::execute("sudo", {"udevadm", "settle"});
            QThread::sleep(1);

            rootPart = detectNewPartitionNode(devPath, before);
            if (rootPart.isEmpty()) { emit errorOccurred("Could not uniquely detect new root partition."); return; }
            espPart  = existingEsp;

            if (QProcess::execute("sudo", {"mkfs.ext4", "-F", rootPart}) != 0) { emit errorOccurred("Failed to format root."); return; }
            QProcess::execute("sudo", {"e2fsck", "-f", rootPart});

            emit logMessage("Mounting root partition...");
            if (QProcess::execute("sudo", {"mount", rootPart, "/mnt"}) != 0) { emit errorOccurred("Failed to mount root at /mnt."); return; }
            QProcess::execute("sudo", {"mkdir", "-p", "/mnt/boot/efi"});
            if (QProcess::execute("sudo", {"mount", espPart, "/mnt/boot/efi"}) != 0) {
                emit errorOccurred("Failed to mount existing ESP at /mnt/boot/efi.");
                return;
            }

            recordTargetMountState(rootPart, espPart);

            emit installComplete();
            return;
        }

        // No ESP -> create ESP then root; detect each by diff
        const QSet<QString> baseline = childPartitionsSet(devPath);

        const QString espStart = QString::number(startMiB) + "MiB";
        const QString espEnd   = QString::number(startMiB + 512) + "MiB";
        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "fat32", espStart, espEnd}) != 0) {
            emit errorOccurred("Failed to create ESP (existing partition).");
            return;
        }
        QProcess::execute("sudo", {"partprobe", devPath});
        QProcess::execute("sudo", {"udevadm", "settle"});
        QThread::sleep(1);

        espPart = detectNewPartitionNode(devPath, baseline);
        if (espPart.isEmpty()) { emit errorOccurred("Could not uniquely detect new ESP."); return; }

        const QString espNum = partitionNumberFromPath(espPart);
        if (espNum.isEmpty()) { emit errorOccurred("Could not determine ESP partition number."); return; }
        QProcess::execute("sudo", {partedBin, devPath, "--script", "name", espNum, "ESP"});
        QProcess::execute("sudo", {partedBin, devPath, "--script", "set",  espNum, "esp", "on"});

        const QSet<QString> beforeRoot = childPartitionsSet(devPath);
        const QString rootStart = espEnd;
        const QString rootEnd   = QString::number(endMiB - 1) + "MiB";
        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "ext4", rootStart, rootEnd}) != 0) {
            emit errorOccurred("Failed to create root (existing partition).");
            return;
        }

        QProcess::execute("sudo", {"partprobe", devPath});
        QProcess::execute("sudo", {"udevadm", "settle"});
        QThread::sleep(1);

        rootPart = detectNewPartitionNode(devPath, beforeRoot);
        if (rootPart.isEmpty()) { emit errorOccurred("Could not uniquely detect new root partition."); return; }

        if (QProcess::execute("sudo", {"mkfs.fat", "-F32", espPart}) != 0) { emit errorOccurred("Failed to format ESP."); return; }
        if (QProcess::execute("sudo", {"mkfs.ext4", "-F", rootPart}) != 0) { emit errorOccurred("Failed to format root."); return; }
        QProcess::execute("sudo", {"e2fsck", "-f", rootPart});

        emit logMessage("Mounting root partition...");
        if (QProcess::execute("sudo", {"mount", rootPart, "/mnt"}) != 0) { emit errorOccurred("Failed to mount root at /mnt."); return; }
        QProcess::execute("sudo", {"mkdir", "-p", "/mnt/boot/efi"});
        QProcess::execute("sudo", {"mount", espPart, "/mnt/boot/efi"});

        recordTargetMountState(rootPart, espPart);

        emit installComplete();
        return;

    } else {
        const QString existingBios = findExistingBiosGrub(partedBin, devPath);
        if (!existingBios.isEmpty()) {
            emit logMessage(QString("Found existing bios_grub partition: %1").arg(existingBios));

            const QSet<QString> before = childPartitionsSet(devPath);

            const QString rootStart = QString::number(startMiB) + "MiB";
            const QString rootEnd   = QString::number(endMiB - 1) + "MiB";
            if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "ext4", rootStart, rootEnd}) != 0) {
                emit errorOccurred("Failed to create root (existing partition).");
                return;
            }
            QProcess::execute("sudo", {"partprobe", devPath});
            QProcess::execute("sudo", {"udevadm", "settle"});
            QThread::sleep(1);

            const QString rootDev = detectNewPartitionNode(devPath, before);
            if (rootDev.isEmpty()) { emit errorOccurred("Could not uniquely detect new root partition."); return; }

            if (QProcess::execute("sudo", {"mkfs.ext4", "-F", rootDev}) != 0) { emit errorOccurred("Failed to format root."); return; }
            QProcess::execute("sudo", {"e2fsck", "-f", rootDev});

            emit logMessage("Mounting root partition...");
            if (QProcess::execute("sudo", {"mount", rootDev, "/mnt"}) != 0) { emit errorOccurred("Failed to mount root at /mnt."); return; }

            recordTargetMountState(rootDev, QString());

            emit installComplete();
            return;
        }

        // No bios_grub present -> carve one from the freed region before creating root
        const long long biosEndMiB = startMiB + 2; // reserve ~2MiB for bios_grub like the wipe path
        if (biosEndMiB >= endMiB) {
            emit errorOccurred("Selected partition is too small to host bios_grub and root partitions.");
            return;
        }

        const QString biosStart = QString::number(startMiB) + "MiB";
        const QString biosEnd   = QString::number(biosEndMiB) + "MiB";
        const QSet<QString> beforeBios = childPartitionsSet(devPath);
        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", biosStart, biosEnd}) != 0) {
            emit errorOccurred("Failed to create bios_grub partition.");
            return;
        }
        QProcess::execute("sudo", {"partprobe", devPath});
        QProcess::execute("sudo", {"udevadm", "settle"});
        QThread::sleep(1);

        const QString biosPart = detectNewPartitionNode(devPath, beforeBios);
        if (biosPart.isEmpty()) {
            emit errorOccurred("Could not detect newly created bios_grub partition.");
            return;
        }
        const QString biosNum = partitionNumberFromPath(biosPart);
        if (biosNum.isEmpty()) {
            emit errorOccurred("Could not determine bios_grub partition number.");
            return;
        }
        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "set", biosNum, "bios_grub", "on"}) != 0) {
            emit errorOccurred("Failed to flag bios_grub partition.");
            return;
        }
        emit logMessage(QString("Created bios_grub partition: %1").arg(biosPart));


            emit logMessage("Mounting root partition...");
            if (QProcess::execute("sudo", {"mount", rootDev, "/mnt"}) != 0) { emit errorOccurred("Failed to mount root at /mnt."); return; }

            emit installComplete();
            return;
        }


            const QSet<QString> before = childPartitionsSet(devPath);

            const QString rootStart = QString::number(startMiB) + "MiB";
            const QString rootEnd   = QString::number(endMiB - 1) + "MiB";
            if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "ext4", rootStart, rootEnd}) != 0) {
                emit errorOccurred("Failed to create root (existing partition).");
                return;
            }
            QProcess::execute("sudo", {"partprobe", devPath});
            QProcess::execute("sudo", {"udevadm", "settle"});
            QThread::sleep(1);

            const QString rootDev = detectNewPartitionNode(devPath, before);
            if (rootDev.isEmpty()) { emit errorOccurred("Could not uniquely detect new root partition."); return; }

            if (QProcess::execute("sudo", {"mkfs.ext4", "-F", rootDev}) != 0) { emit errorOccurred("Failed to format root."); return; }
            QProcess::execute("sudo", {"e2fsck", "-f", rootDev});

            emit logMessage("Mounting root partition...");
            if (QProcess::execute("sudo", {"mount", rootDev, "/mnt"}) != 0) { emit errorOccurred("Failed to mount root at /mnt."); return; }

            emit installComplete();
            return;
        }



            emit installComplete();
            return;
        }

        // No bios_grub present -> carve one from the freed region before creating root
        const long long biosEndMiB = startMiB + 2; // reserve ~2MiB for bios_grub like the wipe path
        if (biosEndMiB >= endMiB) {
            emit errorOccurred("Selected partition is too small to host bios_grub and root partitions.");
            return;
        }

        const QString biosStart = QString::number(startMiB) + "MiB";
        const QString biosEnd   = QString::number(biosEndMiB) + "MiB";
        const QSet<QString> beforeBios = childPartitionsSet(devPath);
        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", biosStart, biosEnd}) != 0) {
            emit errorOccurred("Failed to create bios_grub partition.");
            return;
        }
        QProcess::execute("sudo", {"partprobe", devPath});
        QProcess::execute("sudo", {"udevadm", "settle"});
        QThread::sleep(1);

        const QString biosPart = detectNewPartitionNode(devPath, beforeBios);
        if (biosPart.isEmpty()) {
            emit errorOccurred("Could not detect newly created bios_grub partition.");
            return;
        }
        const QString biosNum = partitionNumberFromPath(biosPart);
        if (biosNum.isEmpty()) {
            emit errorOccurred("Could not determine bios_grub partition number.");
            return;
        }
        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "set", biosNum, "bios_grub", "on"}) != 0) {
            emit errorOccurred("Failed to flag bios_grub partition.");
            return;
        }
        emit logMessage(QString("Created bios_grub partition: %1").arg(biosPart));

        const long long rootStartMiB = biosEndMiB;
        if (endMiB <= rootStartMiB + 1) {
            emit errorOccurred("Remaining space after bios_grub is insufficient for root partition.");
            return;
        }

        const QString rootStart = QString::number(rootStartMiB) + "MiB";
        const QString rootEnd   = QString::number(endMiB - 1) + "MiB";
        const QSet<QString> beforeRoot = childPartitionsSet(devPath);
        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "ext4", rootStart, rootEnd}) != 0) {
            emit errorOccurred("Failed to create root (existing partition).");
            return;
        }
        QProcess::execute("sudo", {"partprobe", devPath});
        QProcess::execute("sudo", {"udevadm", "settle"});
        QThread::sleep(1);

        const QString rootDev = detectNewPartitionNode(devPath, beforeRoot);
        if (rootDev.isEmpty()) {
            emit errorOccurred("Could not uniquely detect new root partition.");
            return;
        }

        if (QProcess::execute("sudo", {"mkfs.ext4", "-F", rootDev}) != 0) {
            emit errorOccurred("Failed to format root.");
            return;
        }
        QProcess::execute("sudo", {"e2fsck", "-f", rootDev});

        emit logMessage("Mounting root partition...");
        if (QProcess::execute("sudo", {"mount", rootDev, "/mnt"}) != 0) {
            emit errorOccurred("Failed to mount root at /mnt.");
            return;
        }

        recordTargetMountState(rootDev, QString());

        emit installComplete();
        return;
    }
}

void InstallerWorker::createFromFreeSpace(QProcess &process, const QString &partedBin, const QString &devPath)
{
    // Helper to format exact "123MiB" tokens (no scientific notation)
    auto miB = [](double v) -> QString {
        const qulonglong iv = static_cast<qulonglong>(v + 0.5); // round
        return QString::number(iv) + "MiB";
    };

    // If UI encoded an explicit extent as "__FREE__:start:end" (MiB)
    double selStartMiB = -1.0, selEndMiB = -1.0;
    if (targetPartition.startsWith("__FREE__:")) {
        const QStringList t = targetPartition.split(':');
        if (t.size() >= 3) {
            selStartMiB = t.at(1).toDouble();
            selEndMiB   = t.at(2).toDouble();
        }
    }

    emit logMessage("Searching for free space…");
    process.start("sudo", {partedBin, devPath, "-m", "unit", "MiB", "print", "free"});
    process.waitForFinished();
    const QString out = process.readAllStandardOutput();
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);

    QString bestStartStr, bestEndStr;

    if (selStartMiB > 0.0 && selEndMiB > selStartMiB) {
        // Use the user-selected extent verbatim
        bestStartStr = miB(selStartMiB);
        bestEndStr   = miB(selEndMiB);
        emit logMessage(QString("Using selected free extent: %1 → %2").arg(bestStartStr, bestEndStr));
    } else {
        // Parse the largest free region from parted -m print free
        double bestSize = 0.0;
        double bestStart = -1.0, bestEnd = -1.0;

        auto toMiBnum = [](QString s) -> double {
            s.remove(QRegularExpression("[^0-9\\.]"));
            return s.toDouble();
        };

        for (const QString &lRaw : lines) {
            const QString l = lRaw.trimmed();
            if (l.isEmpty()) continue;
            if (l.startsWith("BYT")) continue;
            if (l.startsWith("/dev/")) continue;

            // parted -m free lines include "free" as a field
            const QStringList cols = l.split(':');
            if (cols.size() < 4) continue;

            bool isFree = false;
            for (const QString &c : cols) {
                if (c.compare("free", Qt::CaseInsensitive) == 0) { isFree = true; break; }
            }
            if (!isFree) continue;

            const double s = toMiBnum(cols[1]);
            const double e = toMiBnum(cols[2]);
            const double z = toMiBnum(cols[3]); // size
            if (!(z > bestSize && e > s)) continue;

            bestSize  = z;
            bestStart = s;
            bestEnd   = e;
        }

        if (bestStart < 0.0) { emit errorOccurred("No suitable free space found."); return; }
        bestStartStr = miB(bestStart);
        bestEndStr   = miB(bestEnd);
        emit logMessage(QString("Using largest free extent: %1 → %2").arg(bestStartStr, bestEndStr));
    }

    // Numeric for arithmetic; keep 1MiB tail to avoid end-boundary issues
    auto stripMiB = [](QString s) -> double { s.remove("MiB", Qt::CaseInsensitive); return s.toDouble(); };
    const double startMiB = stripMiB(bestStartStr);
    const double endMiB   = stripMiB(bestEndStr);

    if (endMiB <= startMiB + 10.0) { emit errorOccurred("Selected free space is too small."); return; }

    const QSet<QString> baseline = childPartitionsSet(devPath);

    QString espPart;                 // /dev/… for ESP
    QString rootPart;                // /dev/… for root
    bool createdNewEsp = false;

    if (efiInstall) {
        // 1) Reuse an existing ESP on this disk if present
        const QString existingEsp = findExistingEsp(partedBin, devPath);
        if (!existingEsp.isEmpty()) {
            espPart = existingEsp;
            emit logMessage(QString("Found existing ESP: %1").arg(espPart));

            // Sanity: ensure the existing ESP is VFAT/FAT32; don't reformat it.
            if (!isPartitionVfat(espPart)) {
                emit errorOccurred(QString("Existing ESP (%1) is not FAT32. Refusing to modify it.").arg(espPart));
                return;
            }

            // 2) Create root spanning the free extent
            const QSet<QString> before = childPartitionsSet(devPath);
            const QString rootStart = miB(startMiB);
            const QString rootEnd   = miB(endMiB - 1.0);
            if (QProcess::execute("sudo", {partedBin, devPath, "--script",
                                           "mkpart", "primary", "ext4",
                                           rootStart, rootEnd}) != 0) {
                emit errorOccurred("Failed to create root partition (free space).");
                return;
            }
            QProcess::execute("sudo", {"partprobe", devPath});
            QProcess::execute("sudo", {"udevadm", "settle"});
            QThread::sleep(1);
            rootPart = detectNewPartitionNode(devPath, before);
            if (rootPart.isEmpty()) { emit errorOccurred("Could not uniquely detect new root partition."); return; }
        } else {
            // 1) Create a new 512MiB ESP at the start of the free extent
            const QSet<QString> beforeEsp = childPartitionsSet(devPath);
            const QString espStart = miB(startMiB);
            const QString espEnd   = miB(startMiB + 512.0);

            if (QProcess::execute("sudo", {partedBin, devPath, "--script",
                                           "mkpart", "primary", "fat32",
                                           espStart, espEnd}) != 0) {
                emit errorOccurred("Failed to create ESP (free space).");
                return;
            }
            QProcess::execute("sudo", {"partprobe", devPath});
            QProcess::execute("sudo", {"udevadm", "settle"});
            QThread::sleep(1);
            espPart = detectNewPartitionNode(devPath, beforeEsp);
            if (espPart.isEmpty()) { emit errorOccurred("Could not uniquely detect new ESP."); return; }

            const QString baseName = devPath.startsWith("/dev/") ? devPath.mid(5) : devPath;
            const QString espNum   = partitionNumberFromPath(espPart);
            if (espNum.isEmpty()) { emit errorOccurred("Could not determine ESP partition number."); return; }

            // Name + flag
            QProcess::execute("sudo", {partedBin, devPath, "--script", "name", espNum, "ESP"});
            QProcess::execute("sudo", {partedBin, devPath, "--script", "set",  espNum, "esp", "on"});

            createdNewEsp = true;

            // 2) Create root using the remaining extent
            const QSet<QString> beforeRoot = childPartitionsSet(devPath);
            const QString rootStart = espEnd;
            const QString rootEnd   = miB(endMiB - 1.0);
            if (QProcess::execute("sudo", {partedBin, devPath, "--script",
                                           "mkpart", "primary", "ext4",
                                           rootStart, rootEnd}) != 0) {
                emit errorOccurred("Failed to create root partition (free space).");
                return;
            }
            QProcess::execute("sudo", {"partprobe", devPath});
            QProcess::execute("sudo", {"udevadm", "settle"});
            QThread::sleep(1);
            rootPart = detectNewPartitionNode(devPath, beforeRoot);
            if (rootPart.isEmpty()) { emit errorOccurred("Could not uniquely detect new root partition."); return; }
        }

        // Format/mount:
        if (createdNewEsp) {
            emit logMessage("Formatting new ESP as FAT32…");
            if (QProcess::execute("sudo", {"mkfs.fat", "-F32", espPart}) != 0) {
                emit errorOccurred("Failed to format new ESP.");
                return;
            }
        } else {
            emit logMessage("Reusing existing ESP (will not format) …");
        }

        emit logMessage("Formatting root as ext4…");
        if (QProcess::execute("sudo", {"mkfs.ext4", "-F", rootPart}) != 0) {
            emit errorOccurred("Failed to format root.");
            return;
        }
        QProcess::execute("sudo", {"e2fsck", "-f", rootPart});

        emit logMessage("Mounting partitions…");
        QProcess::execute("sudo", {"mount", rootPart, "/mnt"});
        QProcess::execute("sudo", {"mkdir", "-p", "/mnt/boot/efi"});
        QProcess::execute("sudo", {"mount", espPart, "/mnt/boot/efi"});

        emit installComplete();
        return;
    }

    // BIOS path: single ext4 root in the free extent
    {
        const QSet<QString> before = childPartitionsSet(devPath);
        const QString rootStart = miB(startMiB);
        const QString rootEnd   = miB(endMiB - 1.0);
        if (QProcess::execute("sudo", {partedBin, devPath, "--script",
                                       "mkpart", "primary", "ext4",
                                       rootStart, rootEnd}) != 0) {
            emit errorOccurred("Failed to create root partition (free space).");
            return;
        }
        QProcess::execute("sudo", {"partprobe", devPath});
        QProcess::execute("sudo", {"udevadm", "settle"});
        QThread::sleep(1);

        QString rootPartNew = detectNewPartitionNode(devPath, before);
        if (rootPartNew.isEmpty()) { emit errorOccurred("Could not uniquely detect new root partition."); return; }

        emit logMessage("Formatting root as ext4…");
        if (QProcess::execute("sudo", {"mkfs.ext4", "-F", rootPartNew}) != 0) {
            emit errorOccurred("Failed to format root.");
            return;
        }
        QProcess::execute("sudo", {"e2fsck", "-f", rootPartNew});

        emit logMessage("Mounting root partition…");
        if (QProcess::execute("sudo", {"mount", rootPartNew, "/mnt"}) != 0) {
            emit errorOccurred("Failed to mount root at /mnt.");
            return;
        }

        emit installComplete();
        return;
    }
}

void InstallerWorker::run() {
    qputenv("PATH", QByteArray("/usr/sbin:/usr/bin:/sbin:/bin:") + qgetenv("PATH"));
    QProcess process;
    QString partedBin = locatePartedBinary();
    if (partedBin.isEmpty()) {
        emit errorOccurred("parted not found");
        return;
    }

    QFile::remove(targetStateFilePath());

    QString devPath = QString("/dev/%1").arg(selectedDrive);
    emit logMessage(QString("efiInstall = %1").arg(efiInstall ? "true" : "false"));

    // Unmount everything under /mnt and from the drive
    emit logMessage("Preparing mounts...");
    safePreflightUnmounts(devPath);


    if (mode == InstallMode::WipeDrive) {
        emit logMessage(efiInstall ? "Preparing drive for EFI (GPT + ESP + root)" : "Preparing drive for BIOS/GRUB (GPT + bios_grub + root)");

        // Create GPT label
        if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mklabel", "gpt"}) != 0) {
            emit errorOccurred("Failed to create GPT partition table.");
            return;
        }
        QProcess::execute("sudo", {"partprobe", devPath});
        QProcess::execute("sudo", {"udevadm", "settle"});
        QThread::sleep(1);

        if (efiInstall) {
            // ESP (fat32, 1MiB-513MiB)
            if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "fat32", "1MiB", "513MiB"}) != 0 ||
                QProcess::execute("sudo", {partedBin, devPath, "--script", "name", "1", "ESP"}) != 0 ||
                QProcess::execute("sudo", {partedBin, devPath, "--script", "set", "1", "esp", "on"}) != 0) {
                emit errorOccurred("Failed to create/set ESP partition.");
                return;
            }
            // Root partition
            if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "ext4", "513MiB", "100%"}) != 0) {
                emit errorOccurred("Failed to create root partition.");
                return;
            }
        } else {
            // bios_grub (EF02, 1MiB-3MiB)
            if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "1MiB", "3MiB"}) != 0 ||
                QProcess::execute("sudo", {partedBin, devPath, "--script", "set", "1", "bios_grub", "on"}) != 0) {
                emit errorOccurred("Failed to create/set bios_grub partition.");
                return;
            }
            // Root partition
            if (QProcess::execute("sudo", {partedBin, devPath, "--script", "mkpart", "primary", "ext4", "3MiB", "100%"}) != 0) {
                emit errorOccurred("Failed to create root partition.");
                return;
            }
        }
        QProcess::execute("sudo", {"partprobe", devPath});
        QProcess::execute("sudo", {"udevadm", "settle"});
        QThread::sleep(1);

        // Find partitions: lsblk -ln -o NAME
        process.start("lsblk", QStringList() << "-ln" << "-o" << "NAME" << devPath);
        process.waitForFinished();
        QStringList parts = QString(process.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);

        QString espPart, rootPart;
        if (efiInstall && parts.size() >= 2) {
            espPart = "/dev/" + parts[parts.size()-2];
            rootPart = "/dev/" + parts.last();
        } else if (!efiInstall && parts.size() >= 2) {
            rootPart = "/dev/" + parts.last();
        } else {
            emit errorOccurred("Could not detect created partitions after wipe.");
            return;
        }

        if (efiInstall) {
            emit logMessage("Formatting ESP as FAT32...");
            if (QProcess::execute("sudo", {"mkfs.fat", "-F32", espPart}) != 0) {
                emit errorOccurred("Failed to format ESP.");
                return;
            }
        }
        emit logMessage("Formatting root as ext4...");
        if (QProcess::execute("sudo", {"mkfs.ext4", "-F", rootPart}) != 0) {
            emit errorOccurred("Failed to format root partition.");
            return;
        }
        QProcess::execute("sudo", {"e2fsck", "-f", rootPart});

        // Mount root and ESP/bios_grub
        emit logMessage("Mounting new partitions...");
        QProcess::execute("sudo", {"mount", rootPart, "/mnt"});
        if (efiInstall) {
            QProcess::execute("sudo", {"mkdir", "-p", "/mnt/boot/efi"});
            QProcess::execute("sudo", {"mount", espPart, "/mnt/boot/efi"});
        }

        recordTargetMountState(rootPart, efiInstall ? espPart : QString());

        emit installComplete();
        return;
    }

    // ----- Use Free Space -----
    if (mode == InstallMode::UseFreeSpace) {
        createFromFreeSpace(process, partedBin, devPath);
        return;
    }

    // ----- Use Existing Partition -----
    if (mode == InstallMode::UsePartition) {
        recreateFromSelectedPartition(process, partedBin, devPath);
        return;
    }
}
