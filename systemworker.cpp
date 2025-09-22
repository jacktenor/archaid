#include "systemworker.h"
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QMap>
#include <QStringList>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <functional>
#include <utility>

SystemWorker::SystemWorker(QObject *parent) : QObject(parent) {}

// Trim decorations and ensure a canonical /dev/... partition path
static QString normalizePartitionPath(QString any)
{
    // Drop everything after first space or '[' (pretty labels, etc.)
    int cut = any.indexOf(' ');
    if (cut > 0) any = any.left(cut).trimmed();
    int br = any.indexOf('[');
    if (br > 0) any = any.left(br).trimmed();

    // Prepend /dev/ if user gave "sda3", "nvme0n1p5", etc.
    if (!any.startsWith("/dev/") && any.contains(QRegularExpression("^[A-Za-z]")))
        any.prepend("/dev/");

    return any;
}

// Normalizes a partition path to an absolute /dev/* form.
// Accepts "sda3", "/dev/sda3", "nvme0n1p2", etc.
QString SystemWorker::normalizePartitionPath(const QString &in) const
{
    QString s = in.trimmed();
    if (s.isEmpty()) return s;
    if (!s.startsWith("/dev/"))
        s.prepend("/dev/");
    // Nothing else to do here; lsblk/parted already give canonical names.
    return s;
}

bool SystemWorker::isMountPoint(const QString &path)
{
    QProcess proc;
    proc.start("findmnt", {"-rn", path});
    if (!proc.waitForFinished(-1))
        return false;
    return proc.exitCode() == 0;
}

bool SystemWorker::ensureTargetMounts()
{
    const QString disk = drive.startsWith("/dev/") ? drive : QStringLiteral("/dev/%1").arg(drive);
    if (disk.size() <= 5) {
        emit errorOccurred("Invalid target drive specified for installation.");
        return false;
    }

    QDir().mkpath("/mnt");
    if (isMountPoint("/mnt")) {
        if (useEfi) {
            QDir().mkpath("/mnt/boot/efi");
            if (!isMountPoint("/mnt/boot/efi")) {
                emit logMessage("/mnt already mounted; ensuring ESP is mounted as well…");
            } else {
                return true;
            }
        } else {
            return true;
        }
    } else {
        emit logMessage("Target root is not mounted. Attempting to locate and mount it automatically…");
    }

    QString out;
    if (!runCommandCapture(
            QStringLiteral("lsblk -J -b -o NAME,TYPE,FSTYPE,SIZE,PARTFLAGS,MOUNTPOINT %1").arg(disk),
            &out))
        return false;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        emit errorOccurred(QStringLiteral("Failed to parse lsblk output for %1: %2")
                               .arg(disk, parseError.errorString()));
        return false;
    }

    const QJsonArray devices = doc.object().value(QStringLiteral("blockdevices")).toArray();
    if (devices.isEmpty()) {
        emit errorOccurred(QStringLiteral("No block devices found in lsblk output for %1.").arg(disk));
        return false;
    }

    struct PartitionInfo {
        QString name;
        QString fstype;
        QString flags;
        QString mountpoint;
        qulonglong size = 0;
    };

    QList<PartitionInfo> partitions;
    std::function<void(const QJsonObject &)> walk = [&](const QJsonObject &obj) {
        const QString type = obj.value(QStringLiteral("type")).toString();
        const QString name = obj.value(QStringLiteral("name")).toString();
        const QString fstype = obj.value(QStringLiteral("fstype")).toString();
        const QString flags = obj.value(QStringLiteral("partflags")).toString();
        const QString mountpoint = obj.value(QStringLiteral("mountpoint")).toString();
        const qulonglong size = obj.value(QStringLiteral("size")).toVariant().toULongLong();

        if (type == QLatin1String("part") && !name.isEmpty()) {
            PartitionInfo p;
            p.name = name;
            p.fstype = fstype;
            p.flags = flags;
            p.mountpoint = mountpoint;
            p.size = size;
            partitions.append(p);
        }

        const QJsonArray children = obj.value(QStringLiteral("children")).toArray();
        for (const QJsonValue &childVal : children) {
            if (childVal.isObject())
                walk(childVal.toObject());
        }
    };

    for (const QJsonValue &val : devices) {
        if (val.isObject())
            walk(val.toObject());
    }

    if (partitions.isEmpty()) {
        emit errorOccurred(QStringLiteral("No partitions detected on %1.").arg(disk));
        return false;
    }

    auto hasFlag = [](const QString &flags, const QString &needle) {
        return flags.contains(needle, Qt::CaseInsensitive);
    };

    const QSet<QString> rootFsTypes = {"ext4", "btrfs", "xfs", "f2fs", "jfs", "reiserfs"};
    PartitionInfo rootCandidate;
    for (const PartitionInfo &p : std::as_const(partitions)) {
        const QString fs = p.fstype.toLower();
        if (!rootFsTypes.contains(fs))
            continue;
        if (hasFlag(p.flags, QStringLiteral("esp")) || hasFlag(p.flags, QStringLiteral("bios_grub")))
            continue;
        if (rootCandidate.name.isEmpty() || p.size > rootCandidate.size)
            rootCandidate = p;
    }

    if (rootCandidate.name.isEmpty()) {
        emit errorOccurred(QStringLiteral("Unable to determine target root partition on %1.").arg(disk));
        return false;
    }

    const QString rootDev = rootCandidate.name.startsWith("/dev/")
                                ? rootCandidate.name
                                : QStringLiteral("/dev/%1").arg(rootCandidate.name);

    if (!isMountPoint("/mnt")) {
        emit logMessage(QStringLiteral("Mounting %1 at /mnt…").arg(rootDev));
        if (QProcess::execute("sudo", {"mount", rootDev, "/mnt"}) != 0) {
            emit errorOccurred(QStringLiteral("Failed to mount %1 at /mnt.").arg(rootDev));
            return false;
        }
    }

    if (!isMountPoint("/mnt")) {
        emit errorOccurred("/mnt is not a valid mountpoint even after attempting to mount the root partition.");
        return false;
    }

    if (!useEfi) {
        return true;
    }

    QDir().mkpath("/mnt/boot/efi");
    if (isMountPoint("/mnt/boot/efi"))
        return true;

    PartitionInfo espCandidate;
    for (const PartitionInfo &p : std::as_const(partitions)) {
        const QString flags = p.flags;
        const QString fs = p.fstype.toLower();
        const bool looksEsp = hasFlag(flags, QStringLiteral("esp")) || fs == "vfat" || fs == "fat32" || fs == "fat";
        if (!looksEsp)
            continue;
        if (espCandidate.name.isEmpty() || hasFlag(flags, QStringLiteral("esp")) || p.size < espCandidate.size) {
            espCandidate = p;
            if (hasFlag(flags, QStringLiteral("esp")))
                break; // prefer explicitly flagged ESPs
        }
    }

    if (espCandidate.name.isEmpty()) {
        emit errorOccurred(QStringLiteral("EFI installation requested, but no EFI System Partition was found on %1.").arg(disk));
        return false;
    }

    const QString espDev = espCandidate.name.startsWith("/dev/")
                               ? espCandidate.name
                               : QStringLiteral("/dev/%1").arg(espCandidate.name);

    emit logMessage(QStringLiteral("Mounting EFI System Partition %1 at /mnt/boot/efi…").arg(espDev));
    if (QProcess::execute("sudo", {"mount", espDev, "/mnt/boot/efi"}) != 0) {
        emit errorOccurred(QStringLiteral("Failed to mount EFI System Partition %1 at /mnt/boot/efi.").arg(espDev));
        return false;
    }

    if (!isMountPoint("/mnt/boot/efi")) {
        emit errorOccurred("/mnt/boot/efi is not mounted after attempting to attach the EFI System Partition.");
        return false;
    }

    return true;
}

void SystemWorker::setParameters(const QString &drv,
                                 const QString &user,
                                 const QString &pass,
                                 const QString &rootPass,
                                 const QString &de,
                                 bool efi) {
    drive = drv;
    username = user;
    password = pass;
    rootPassword = rootPass;
    desktopEnv = de;
    useEfi = efi;
}

bool SystemWorker::installDesktopAndDM()
{
    const QString choice = desktopEnv.trimmed();

    // Handle "no desktop"
    if (choice.isEmpty() ||
        choice.compare("None", Qt::CaseInsensitive) == 0 ||
        choice.compare("No Desktop", Qt::CaseInsensitive) == 0)
    {
        emit logMessage("No desktop selected. Boot target set to multi-user.");
        return runCommand("sudo arch-chroot /mnt systemctl set-default multi-user.target");
    }

    // Essentials per-DE: Xorg, DM, terminal, Firefox, helpers
    QMap<QString, QStringList> desktopPkgs = {
        {"GNOME",      {"xorg", "gnome", "gdm", "gnome-terminal", "firefox",
                   "gvfs", "xdg-utils", "xdg-user-dirs"}},
        {"KDE Plasma", {"xorg", "plasma", "sddm", "konsole", "firefox",
                        "gvfs", "xdg-utils", "xdg-user-dirs"}},
        {"XFCE",       {"xorg", "xfce4", "xfce4-goodies", "lightdm", "lightdm-gtk-greeter",
                  "xfce4-terminal", "firefox", "gvfs", "xdg-utils", "xdg-user-dirs"}},
        // LXQt includes icon theme + file manager + terminal + helpers
        {"LXQt",       {"xorg", "lxqt", "lxqt-qtplugin", "pcmanfm-qt", "qterminal",
                  "papirus-icon-theme", "hicolor-icon-theme",
                  "sddm", "firefox", "gvfs", "xdg-utils", "xdg-user-dirs"}},
        {"Cinnamon",   {"xorg", "cinnamon", "lightdm", "lightdm-gtk-greeter",
                      "gnome-terminal", "nemo", "firefox",
                      "gvfs", "xdg-utils", "xdg-user-dirs"}},
        {"MATE",       {"xorg", "mate", "mate-extra", "lightdm", "lightdm-gtk-greeter",
                  "mate-terminal", "firefox", "gvfs", "xdg-utils", "xdg-user-dirs"}}
        //{"i3",         {"xorg", "i3", "lightdm", "lightdm-gtk-greeter",
          //      "alacritty", "firefox", "gvfs", "xdg-utils", "xdg-user-dirs"}}
    };

    if (!desktopPkgs.contains(choice)) {
        emit errorOccurred(QString("Unknown desktop environment: %1").arg(choice));
        return false;
    }

    // Install packages in chroot
    const QStringList pkgs = desktopPkgs.value(choice);
    const QString pkgCmd =
        QString("sudo arch-chroot /mnt pacman -S --noconfirm --needed %1")
            .arg(pkgs.join(' '));
    if (!runCommand(pkgCmd))
        return false;

    // Enable the display manager
    QString dmService;
    if (choice == "GNOME") dmService = "gdm.service";
    else if (choice == "KDE Plasma" || choice == "LXQt") dmService = "sddm.service";
    else dmService = "lightdm.service";

    if (!runCommand(QString("sudo arch-chroot /mnt systemctl enable %1").arg(dmService)))
        return false;

    // Boot to graphical when a DE is installed
    if (!runCommand("sudo arch-chroot /mnt systemctl set-default graphical.target"))
        return false;

    // Greeter defaults (safe if already present)
    if (dmService == "lightdm.service") {
        runCommand(
            "sudo arch-chroot /mnt bash -c \"mkdir -p /etc/lightdm && "
            "printf '[greeter]\\n"
            "theme-name=Adwaita\\n"
            "icon-theme-name=Adwaita\\n"
            "background=#101010\\n' > /etc/lightdm/lightdm-gtk-greeter.conf\""
            );
    } else if (dmService == "sddm.service") {
        runCommand(
            "sudo arch-chroot /mnt bash -c \"mkdir -p /etc/sddm.conf.d && "
            "printf '[Theme]\\nCurrent=breeze\\n' > /etc/sddm.conf.d/10-theme.conf\""
            );
    }

    // Create user’s standard folders (harmless if they already exist)
    runCommand(QString(
                   "sudo arch-chroot /mnt bash -lc 'su - %1 -c xdg-user-dirs-update || true'"
                   ).arg(username));

    // Auto-apply icon theme for LXQt (robust, no fragile quoting)
    if (choice == "LXQt") {
        if (!applyLxqtIconTheme(username))
            return false;
        emit logMessage("LXQt: icon theme applied for the user.");
    }

    // Cinnamon safety: ensure a terminal exists even if upstream changes
    if (choice == "Cinnamon") {
        runCommand(
            "sudo arch-chroot /mnt bash -lc '"
            "command -v gnome-terminal >/dev/null || pacman -S --noconfirm --needed xterm'"
            );
    }

    if (!neutralizeLoginNoise()) return false;

    emit logMessage(QString("Desktop environment '%1' installed and configured.").arg(choice));
    return true;
}

// Neutralize live-ISO banners so PAM/DM won't show "installation guide" text at login.
bool SystemWorker::neutralizeLoginNoise()
{
    // Minimal issue (TTY prompt text); harmless if not used by the greeter
    if (!runCommand("sudo arch-chroot /mnt bash -lc 'printf \"Arch Linux \\\\r (\\\\l)\\\\n\" > /etc/issue'"))
        return false;

    // Empty MOTD (PAM will find nothing to display)
    if (!runCommand("sudo arch-chroot /mnt bash -lc \"> /etc/motd\""))
        return false;

    // If issue.net exists (rare), clear it as well
    runCommand("sudo arch-chroot /mnt bash -lc '[ -f /etc/issue.net ] && : > /etc/issue.net || true'");

    emit logMessage("Login banner/MOTD neutralized in target.");
    return true;
}

bool SystemWorker::applyLxqtIconTheme(const QString &user)
{
    // 0) Verify the user exists in the chroot and get their home
    QString uhome;
    {
        QString out;
        if (!runCommandCapture(
                QString("sudo arch-chroot /mnt bash -lc 'id -u %1 >/dev/null 2>&1 && getent passwd %1 | cut -d: -f6'")
                    .arg(user),
                &out)) {
            emit errorOccurred(QString("LXQt: user '%1' not found inside the target.").arg(user));
            return false;
        }
        uhome = out.trimmed();
        if (uhome.isEmpty()) {
            emit errorOccurred(QString("LXQt: could not resolve home directory for '%1'.").arg(user));
            return false;
        }
    }

    // 1) Detect an installed icon theme we can apply
    QString picked;
    if (!runCommandCapture(
            "sudo arch-chroot /mnt bash -lc '"
            "for t in Papirus Papirus-Dark Papirus-Light ePapirus ePapirus-Dark Breeze oxygen Adwaita hicolor; do "
            "  [ -d \"/usr/share/icons/$t\" ] && { echo \"$t\"; exit 0; }; "
            "done; exit 1'",
            &picked)) {
        emit errorOccurred("LXQt: no suitable icon theme found under /usr/share/icons.");
        return false;
    }
    picked = picked.trimmed();
    emit logMessage(QString("LXQt: will apply icon theme '%1'.").arg(picked));

    // 2) Write files as root, then chown to the user (no group name required)
    const QString script =
        QString(
            "sudo arch-chroot /mnt bash -lc '"
            "set -e; "
            "UHOME=%1; THEME=%2; "
            "mkdir -p \"$UHOME/.config/lxqt\" \"$UHOME/.config/gtk-3.0\" \"$UHOME/.config/gtk-4.0\"; "
            // lxqt.conf (both sections some versions read)
            "cat > \"$UHOME/.config/lxqt/lxqt.conf\" <<EOF\n"
            "[Appearance]\n"
            "icon_theme=$THEME\n"
            "\n"
            "[General]\n"
            "icon_theme=$THEME\n"
            "EOF\n"
            // GTK 3
            "cat > \"$UHOME/.config/gtk-3.0/settings.ini\" <<EOF\n"
            "[Settings]\n"
            "gtk-icon-theme-name=$THEME\n"
            "gtk-theme-name=Adwaita\n"
            "EOF\n"
            // GTK 4
            "cat > \"$UHOME/.config/gtk-4.0/settings.ini\" <<EOF\n"
            "[Settings]\n"
            "gtk-icon-theme-name=$THEME\n"
            "gtk-theme-name=Adwaita\n"
            "EOF\n"
            // Ownership: use primary group via 'user:' (no explicit group name)
            "chown -R %3: \"$UHOME/.config/lxqt\" \"$UHOME/.config/gtk-3.0\" \"$UHOME/.config/gtk-4.0\"; "
            // Session hint (harmless if already there)
            "echo \"export XDG_CURRENT_DESKTOP=LXQt\" > /etc/profile.d/10-lxqt.sh; "
            "chmod 0644 /etc/profile.d/10-lxqt.sh; "
            // Best effort icon cache refresh
            "gtk-update-icon-cache -f /usr/share/icons/hicolor 2>/dev/null || true; "
            "'"
            ).arg(uhome, picked, user);

    if (!runCommand(script)) {
        emit errorOccurred("LXQt: failed to write user appearance settings (applyCmd).");
        return false;
    }

    return true;
}

// Runs a shell command, captures its stdout into 'output', returns true on success (exit code 0).
bool SystemWorker::runCommandCapture(const QString &command, QString *output)
{
    QProcess process;
    process.start("bash", {"-lc", command});
    if (!process.waitForFinished(-1)) {
        emit errorOccurred(QString("Timeout while running command: %1").arg(command));
        return false;
    }
    const int code = process.exitCode();
    const QString out = QString::fromUtf8(process.readAllStandardOutput());
    const QString err = QString::fromUtf8(process.readAllStandardError());
    if (output) *output = out;
    if (code != 0) {
        emit errorOccurred(QString("Command failed: %1\nExit code: %2\nError: %3")
                               .arg(command).arg(code).arg(err.trimmed()));
        return false;
    }
    return true;
}

bool SystemWorker::runCommand(const QString &cmd) {
    // Stream stdout/stderr in real-time to avoid "big dump at the end".
    QProcess proc;

    // Keep stderr interleaved with stdout, so logs are chronological.
    proc.setProcessChannelMode(QProcess::MergedChannels);

    // Use stdbuf to force line buffering (-oL = stdout, -eL = stderr).
    // We go through /bin/sh -lc to handle quotes and let PATH resolve stdbuf.
    const QString wrapped = QString("stdbuf -oL -eL %1").arg(cmd);
    emit logMessage(QString("→ %1").arg(cmd));
    proc.start("/bin/sh", {"-lc", wrapped});

    if (!proc.waitForStarted()) {
        emit errorOccurred(QString("Failed to start: %1").arg(cmd));
        return false;
    }

    QByteArray acc;  // line accumulator
    auto flushLines = [&]() {
        int nl;
        while ((nl = acc.indexOf('\n')) >= 0) {
            const QByteArray line = acc.left(nl);
            acc.remove(0, nl + 1);
            if (!line.trimmed().isEmpty())
                emit logMessage(QString::fromUtf8(line));
        }
    };

    // Read as data arrives
    while (proc.state() != QProcess::NotRunning) {
        if (proc.waitForReadyRead(200)) {
            acc.append(proc.readAll());
            flushLines();
        } else {
            // keep UI responsive
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
    }

    // Flush any trailing partial line
    acc.append(proc.readAll());
    if (!acc.isEmpty()) {
        const QString tail = QString::fromUtf8(acc).trimmed();
        if (!tail.isEmpty()) emit logMessage(tail);
    }

    const int code = proc.exitCode();
    if (code != 0) {
        const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        emit errorOccurred(err.isEmpty()
                               ? QString("Command failed (exit %1): %2").arg(code).arg(cmd)
                               : QString("Command failed (exit %1): %2\n%3").arg(code).arg(cmd, err));
        return false;
    }
    return true;
}

// Ensure whatever the UI passes (pretty text or real path) becomes a canonical /dev/... path
void SystemWorker::setTargetPartition(const QString &sel)
{
    m_targetPartition = normalizePartitionPath(sel);
    emit logMessage(QString("Target partition set to %1").arg(m_targetPartition));
}

QString SystemWorker::targetPartitionPath()
{
    return m_targetPartition;
}
bool SystemWorker::hasTargetPartition()
{
    return !m_targetPartition.isEmpty();
}

bool SystemWorker::installGrubRobust(const QString &targetDisk, bool efiInstall)
{
    // targetDisk example: "/dev/sda" or "/dev/nvme0n1"
    QString disk = targetDisk;
    if (!disk.startsWith("/dev/")) disk = "/dev/" + disk;

    if (efiInstall) {
        emit logMessage("Installing GRUB for UEFI…");

        // Make sure ESP is mounted at /mnt/boot/efi
        if (!runCommand("sudo arch-chroot /mnt bash -lc 'mkdir -p /boot/efi'"))
            return false;

        // If /mnt/boot/efi is empty, try to auto-mount an ESP by PARTUUID/label/flag
        // (Safe no-ops if already mounted)
        runCommand(
            "bash -lc '"
            "if ! mountpoint -q /mnt/boot/efi; then "
            "  ESP=$(lsblk -rpno NAME,PARTTYPE,PARTLABEL,PARTFLAGS " + disk +
            R"( | awk '/c12a7328-f81f-11d2-ba4b-00a0c93ec93b|esp|boot/ {print $1; exit}'); )"
            "  if [ -n \"$ESP\" ]; then sudo mount \"$ESP\" /mnt/boot/efi; fi; "
            "fi'");

        // Install GRUB to the ESP with a named boot entry
        if (!runCommand(
                "sudo arch-chroot /mnt bash -lc "
                "'grub-install --target=x86_64-efi "
                "--efi-directory=/boot/efi "
                "--bootloader-id=Arch "
                "--recheck'"))
        {
            emit logMessage("grub-install (NVRAM entry) failed — falling back to removable path…");

            // Fallback: write the removable bootloader (no NVRAM needed)
            if (!runCommand(
                    "sudo arch-chroot /mnt bash -lc "
                    "'grub-install --target=x86_64-efi "
                    "--efi-directory=/boot/efi "
                    "--removable --recheck'"))
                return false;
        }

        // Optional: make sure the EFI/BOOT fallback exists even when the first call succeeded
        runCommand(
            "sudo arch-chroot /mnt bash -lc "
            "'if [ ! -e /boot/efi/EFI/BOOT/BOOTX64.EFI ]; then "
            "   mkdir -p /boot/efi/EFI/BOOT && "
            "   cp -f /boot/efi/EFI/Arch/grubx64.efi /boot/efi/EFI/BOOT/BOOTX64.EFI || true; "
            "fi'");

        // Generate GRUB config now; os-prober step can still add more later
        if (!runCommand("sudo arch-chroot /mnt bash -lc 'grub-mkconfig -o /boot/grub/grub.cfg'"))
            return false;

        // Try to ensure an NVRAM entry exists; not fatal if efibootmgr can’t write
        runCommand("sudo arch-chroot /mnt bash -lc 'efibootmgr -v || true'");

        emit logMessage("UEFI GRUB installation completed.");
        return true;
    } else {
        emit logMessage("Installing GRUB for legacy BIOS (MBR)…");

        // IMPORTANT: install to the DISK, not a partition
        if (!runCommand("sudo arch-chroot /mnt bash -lc 'grub-install --target=i386-pc --recheck " + disk + "'"))
            return false;

        if (!runCommand("sudo arch-chroot /mnt bash -lc 'grub-mkconfig -o /boot/grub/grub.cfg'"))
            return false;

        emit logMessage("BIOS GRUB installation completed.");
        return true;
    }
}

bool SystemWorker::generateGrubWithOsProber()
{
    emit logMessage("Enabling os-prober for GRUB…");

    // Make sure os-prober is installed (idempotent)
    if (!runCommand("sudo arch-chroot /mnt pacman -Sy --noconfirm os-prober dialog networkmanager ntfs-3g --needed"))
        return false;

    // Ensure GRUB uses os-prober: set or replace the line in /etc/default/grub
    if (!runCommand(
            "sudo arch-chroot /mnt bash -c "
            "\"if grep -q '^GRUB_DISABLE_OS_PROBER=' /etc/default/grub; then "
            "sed -i 's/^GRUB_DISABLE_OS_PROBER=.*/GRUB_DISABLE_OS_PROBER=false/' /etc/default/grub; "
            "else "
            "echo 'GRUB_DISABLE_OS_PROBER=false' >> /etc/default/grub; "
            "fi\""
            )) return false;

    // Make sure the prober script is executable if it exists
    if (!runCommand("sudo arch-chroot /mnt bash -c 'if [ -f /etc/grub.d/30_os-prober ]; then chmod +x /etc/grub.d/30_os-prober; fi'"))
        return false;

    // Run os-prober (don't fail if it returns 1 when nothing is found)
    if (!runCommand("sudo arch-chroot /mnt bash -c 'os-prober || true'"))
        return false;

    // Generate GRUB config (use update-grub if available, else grub-mkconfig)
    if (!runCommand(
            "sudo arch-chroot /mnt bash -c "
            "'if command -v update-grub >/dev/null 2>&1; then "
            "update-grub; "
            "else "
            "grub-mkconfig -o /boot/grub/grub.cfg; "
            "fi'"
            )) return false;

    emit logMessage("GRUB menu generated with os-prober results.");
    return true;
}

void SystemWorker::run() {
    emit logMessage("\xF0\x9F\x9A\x80 Starting system installation...");

    if (!ensureTargetMounts())
        return;

    QString isoPath = "/mnt/archlinux.iso";
    if (!QFile::exists(isoPath)) {
        QString tmpIso = QDir::tempPath() + "/archlinux.iso";
        if (QFile::exists(tmpIso)) {
            if (!runCommand(QString("sudo cp %1 %2").arg(tmpIso, isoPath)))
                return;
        } else {
            emit errorOccurred("Arch Linux ISO not found");
            return;
        }
    }

    QDir().mkdir("/mnt/archiso");
    QDir().mkdir("/mnt/rootfs");

    if (!runCommand(QString("sudo mount -o loop %1 /mnt/archiso").arg(isoPath)))
        return;

    QString squashfsPath = "/mnt/archiso/arch/x86_64/airootfs.sfs";
    if (!runCommand(QString("sudo unsquashfs -f -d /mnt %1").arg(squashfsPath)))
        return;

    emit logMessage("ISO mounted and rootfs extracted");
    runCommand("sudo umount -Rfl /mnt/archiso");

    runCommand("sudo rm -f /mnt/etc/resolv.conf");
    runCommand("sudo cp /etc/resolv.conf /mnt/etc/resolv.conf");

    runCommand(
        "sudo arch-chroot /mnt bash -lc \"set -e; "
        "for d in /var/cache/pacman /var/cache/pacman/pkg /var/lib/pacman /var/lib/pacman/sync; do "
        "  if [ -L \\\"$d\\\" ] || { [ -e \\\"$d\\\" ] && [ ! -d \\\"$d\\\" ]; }; then rm -rf \\\"$d\\\"; fi; "
        "done; "
        "mkdir -p /var/cache/pacman/pkg /var/lib/pacman/sync; "
        "chown root:root /var/cache/pacman /var/cache/pacman/pkg /var/lib/pacman /var/lib/pacman/sync; "
        "chmod 0755 /var/cache/pacman /var/cache/pacman/pkg /var/lib/pacman /var/lib/pacman/sync;\"");

    if (!QFile::exists("/mnt/usr/bin/pacman")) {
    QString mirrorUrl = customMirrorUrl;
        QString bootstrapUrl;

        if (!mirrorUrl.isEmpty()) {
            if (!mirrorUrl.endsWith("/"))
                mirrorUrl += "/";
            bootstrapUrl = mirrorUrl + "iso/latest/archlinux-bootstrap-x86_64.tar.gz";
        } else {
            bootstrapUrl = "https://mirrors.edge.kernel.org/archlinux/iso/latest/archlinux-bootstrap-x86_64.tar.gz";
        }

        qDebug() << "Using Arch bootstrap URL:" << bootstrapUrl;

        if (!runCommand(QString("sudo wget -O /tmp/arch-bootstrap.tar.gz %1").arg(bootstrapUrl)))
            return;
        if (!runCommand("sudo tar -xzf /tmp/arch-bootstrap.tar.gz -C /mnt --strip-components=1"))
            return;
    }

    runCommand("sudo arch-chroot /mnt pacman-key --init");
    runCommand("sudo arch-chroot /mnt pacman-key --populate archlinux");
    runCommand("sudo arch-chroot /mnt pacman -Sy --noconfirm archlinux-keyring");

    // Remove leftover firmware files from the live ISO to avoid conflicts
    runCommand("sudo rm -rf /mnt/usr/lib/firmware/nvidia");

    emit logMessage("Installing base, linux, linux-firmware…");
    // Reinstall the kernel even if the ISO's rootfs already contains the
    // package so /boot/vmlinuz-linux is ensured to exist
    if (!runCommand("sudo arch-chroot /mnt pacman -Sy --noconfirm --needed base linux linux-firmware"))
        return;

    // Ensure mkinitcpio presets do not reference the live ISO configuration
    QString presetContent =
        "# mkinitcpio preset file for the 'linux' package\n"
        "ALL_config=\"/etc/mkinitcpio.conf\"\n"
        "ALL_kver=\"/boot/vmlinuz-linux\"\n"
        "\n"
        "PRESETS=(\n"
        "  default\n"
        "  fallback\n"
        ")\n"
        "\n"
        "default_image=\"/boot/initramfs-linux.img\"\n"
        "fallback_image=\"/boot/initramfs-linux-fallback.img\"\n"
        "fallback_options=\"-S autodetect\"\n";
    QFile presetFile("/tmp/linux.preset");
    if (presetFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        presetFile.write(presetContent.toUtf8());
        presetFile.close();
    }
    runCommand("sudo cp /tmp/linux.preset /mnt/etc/mkinitcpio.d/linux.preset");

    runCommand("sudo arch-chroot /mnt systemctl enable systemd-timesyncd.service");
    runCommand("sudo arch-chroot /mnt rm -f /etc/mkinitcpio.conf.d/archiso.conf");
    runCommand("sudo arch-chroot /mnt sed -i 's/archiso[^ ]* *//g' /etc/mkinitcpio.conf");
    runCommand("sudo arch-chroot /mnt rm -f /boot/initramfs-linux*");
    runCommand("sudo arch-chroot /mnt mkinitcpio -P");

    runCommand("sudo arch-chroot /mnt bash -c 'echo archlinux > /etc/hostname'");
    runCommand("sudo arch-chroot /mnt sed -i 's/^#en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen");
    runCommand("sudo arch-chroot /mnt locale-gen");
    runCommand("sudo arch-chroot /mnt bash -c 'echo LANG=en_US.UTF-8 > /etc/locale.conf'");
    runCommand("sudo arch-chroot /mnt ln -sf /usr/share/zoneinfo/UTC /etc/localtime");
    runCommand("sudo arch-chroot /mnt hwclock --systohc");
    runCommand("sudo arch-chroot /mnt mkdir -p /boot/grub");

    emit logMessage("Installing GRUB…");
    if (!runCommand("sudo arch-chroot /mnt pacman -Sy --noconfirm grub os-prober networkmanager dialog --needed"))
        return;

    emit logMessage("Enabling NetworkManager to start at boot…");
    if (!runCommand("sudo arch-chroot /mnt systemctl enable NetworkManager.service"))
    return;

    runCommand("sudo arch-chroot /mnt sed -i '/2025-05-01-10-09-37-00/d' /etc/default/grub");
    runCommand("sudo arch-chroot /mnt bash -c \"echo 'GRUB_DISABLE_LINUX_UUID=false' >> /etc/default/grub\"");

    QString grubCmd;
    if (useEfi) {
        grubCmd = "sudo arch-chroot /mnt grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=GRUB";
    } else {
        grubCmd = QString("sudo arch-chroot /mnt grub-install --target=i386-pc /dev/%1").arg(drive);
    }

    emit logMessage(grubCmd);

        if (!runCommand(grubCmd))
        return;

    if (!generateGrubWithOsProber())
        return;
    if (!runCommand("sudo arch-chroot /mnt pacman -Syu --noconfirm"))
        return;
    emit logMessage("System packages updated");

    emit logMessage("Adding user and configuring system.");
    emit logMessage("This will take a few…");
    runCommand(QString("sudo arch-chroot /mnt useradd -m -G wheel %1").arg(username));
    runCommand(QString("sudo arch-chroot /mnt bash -c \"echo '%1:%2' | chpasswd\"" ).arg(username, password));
    runCommand(QString("sudo arch-chroot /mnt bash -c \"echo 'root:%1' | chpasswd\"" ).arg(rootPassword));
    runCommand("sudo arch-chroot /mnt sed -i 's/^# %wheel ALL=(ALL:ALL) ALL/%wheel ALL=(ALL:ALL) ALL/' /etc/sudoers");

    if (!installDesktopAndDM()) return;

    runCommand("sudo arch-chroot /mnt bash -c 'rm -f /etc/fstab'");
    runCommand("sudo bash -c 'genfstab -U /mnt > /mnt/etc/fstab'");
    runCommand("sudo bash -c \"awk '!/^#|^$/{print; exit} 1' /mnt/etc/fstab > /mnt/etc/fstab.clean && mv /mnt/etc/fstab.clean /mnt/etc/fstab\"");

    emit logMessage("\xE2\x9C\x85 All tasks completed");
    emit finished();
}

