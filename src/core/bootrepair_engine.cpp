#include "core/bootrepair_engine.h"

#include <QCoreApplication>
#include <QSignalBlocker>
#include <QDir>
#include <QFile>
#include <QSysInfo>

namespace {
QString normalizeArch(QString arch)
{
    arch = arch.trimmed();
    if (arch == "i686") {
        arch = "i386";
    }
    return arch;
}

QString detectArch(Cmd* shell)
{
    const QString cmd = QStringLiteral("uname -m");
    QString arch = shell->getCmdOut(cmd, QuietMode::Yes).trimmed();
    if (arch.contains("not found", Qt::CaseInsensitive)) {
        arch.clear();
    }
    if (arch.isEmpty()) {
        arch = QSysInfo::currentCpuArchitecture();
    }
    arch = normalizeArch(arch);
    if (arch.isEmpty()) {
        arch = QStringLiteral("x86_64");
    }
    return arch;
}

bool isArchBuild(const QString& rootPath, Cmd* shell = nullptr)
{
    const QStringList mkinitcpio = {"/usr/bin/mkinitcpio", "/etc/mkinitcpio.conf"};
    for (const auto& path : mkinitcpio) {
        const QString full = rootPath.isEmpty() ? path : QDir(rootPath).filePath(path.mid(1));
        if (shell && !rootPath.isEmpty()) {
            if (shell->runAsRoot(QStringLiteral("test -e %1").arg(full), nullptr, nullptr, QuietMode::Yes)) {
                return true;
            }
        } else if (QFile::exists(full)) {
            return true;
        }
    }
    return false;
}

bool dirContainsEfi(const QString& dirPath, Cmd* shell)
{
    if (shell) {
        // Use shell ls to handle root-owned mounts
        QString output;
        if (shell->runAsRoot(QStringLiteral("ls -1 %1").arg(dirPath), &output, nullptr, QuietMode::Yes)) {
            const QStringList files = output.split('\n', Qt::SkipEmptyParts);
            for (const auto& file : files) {
                if (file.trimmed().endsWith(".efi", Qt::CaseInsensitive)) {
                    return true;
                }
            }
        }
        return false;
    }
    const QDir dir(dirPath);
    const QStringList files = dir.entryList(QDir::Files);
    for (const auto& file : files) {
        if (file.endsWith(".efi", Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QString detectBootloaderId(const QString& efiMountPath, const QString& rootPath, Cmd* shell = nullptr)
{
    const QString fallback = isArchBuild(rootPath, shell) ? QStringLiteral("MXarch") : QStringLiteral("MX");

    // Look for existing MX*/antiX* directories on the ESP that contain a GRUB binary.
    const QString efiPath = efiMountPath + "/EFI";
    QStringList entries;
    if (shell && !rootPath.isEmpty()) {
        // Use shell to list directories on root-owned mounts
        if (!shell->runAsRoot(QStringLiteral("test -d %1").arg(efiPath), nullptr, nullptr, QuietMode::Yes)) {
            return fallback;
        }
        QString output;
        if (shell->runAsRoot(QStringLiteral("ls -1 %1").arg(efiPath), &output, nullptr, QuietMode::Yes)) {
            entries = output.split('\n', Qt::SkipEmptyParts);
            // Filter to directories only
            QStringList dirs;
            for (const auto& e : entries) {
                const QString trimmed = e.trimmed();
                if (!trimmed.isEmpty() && trimmed != "." && trimmed != "..") {
                    if (shell->runAsRoot(QStringLiteral("test -d %1/%2").arg(efiPath, trimmed), nullptr, nullptr, QuietMode::Yes)) {
                        dirs << trimmed;
                    }
                }
            }
            entries = dirs;
        }
    } else {
        const QDir efiDir(efiPath);
        if (!efiDir.exists()) {
            return fallback;
        }
        entries = efiDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    }

    // Collect all MX*/antiX* candidates that have .efi files.
    QStringList candidates;
    for (const auto& entry : entries) {
        if ((entry.startsWith("MX", Qt::CaseInsensitive) || entry.startsWith("antiX", Qt::CaseInsensitive))
            && dirContainsEfi(efiPath + "/" + entry, shell)) {
            candidates << entry;
        }
    }
    if (candidates.isEmpty()) {
        return fallback;
    }
    // Prefer the expected fallback name if present; otherwise take the first match.
    if (candidates.contains(fallback, Qt::CaseInsensitive)) {
        for (const auto& c : candidates) {
            if (c.compare(fallback, Qt::CaseInsensitive) == 0) {
                return c;
            }
        }
    }
    return candidates.first();
}

bool grubSupportsForceExtraRemovable(Cmd* shell, const QString& helpCmd, Elevation elevate)
{
    const bool wasSuppressed = shell->outputSuppressed();
    shell->setOutputSuppressed(true);
    const QString helpText = shell->getCmdOut(helpCmd, QuietMode::Yes, elevate);
    shell->setOutputSuppressed(wasSuppressed);
    return helpText.contains("--force-extra-removable");
}

QString grubInstallCmd(const QString& baseCmd, const QString& arch, const QString& bootloaderId, bool forceExtraRemovable)
{
    const QString extra = forceExtraRemovable ? QStringLiteral(" --force-extra-removable") : QString();
    return QStringLiteral("%1 --target=%2-efi --efi-directory=/boot/efi --bootloader-id=%3%4 --recheck")
        .arg(baseCmd, arch, bootloaderId, extra);
}

QString detectGrubInstallCmd(const QString& rootPath, Cmd* shell = nullptr)
{
    const QStringList grubInstall = {"/usr/sbin/grub-install", "/usr/bin/grub-install", "/sbin/grub-install", "/bin/grub-install"};
    const QStringList grubMkstandalone = {"/usr/bin/grub-mkstandalone", "/bin/grub-mkstandalone", "/usr/sbin/grub-mkstandalone", "/sbin/grub-mkstandalone"};
    auto existsInRoot = [&rootPath, shell](const QString& path) {
        if (rootPath.isEmpty()) {
            return QFile::exists(path);
        }
        // Use shell test -x for chroot paths to handle root-owned mounts
        // that QFile::exists() cannot access from the unprivileged process.
        if (shell) {
            const QString full = QDir(rootPath).filePath(path.mid(1));
            return shell->runAsRoot(QStringLiteral("test -x %1").arg(full), nullptr, nullptr, QuietMode::Yes);
        }
        return QFile::exists(QDir(rootPath).filePath(path.mid(1)));
    };
    for (const auto& path : grubInstall) {
        if (existsInRoot(path)) {
            return QStringLiteral("grub-install");
        }
    }
    for (const auto& path : grubMkstandalone) {
        if (existsInRoot(path)) {
            return QStringLiteral("grub-mkstandalone");
        }
    }
    return {};
}

QString efiArchSuffix(const QString& arch)
{
    if (arch == "x86_64") return QStringLiteral("x64");
    if (arch == "i386") return QStringLiteral("ia32");
    if (arch == "aarch64" || arch == "arm64") return QStringLiteral("aa64");
    return QStringLiteral("x64");
}

QString grubMkstandaloneCmd(const QString& chrootPath, const QString& arch, const QString& bootloaderId, bool useHostBinary)
{
    const QString suffix = efiArchSuffix(arch);
    const QString upperSuffix = suffix.toUpper();
    // Host-absolute path prefix for all file operations
    const QString hp = chrootPath.isEmpty() ? QString() : chrootPath;
    const QString efiTarget = QStringLiteral("%1-efi").arg(arch);

    // When useHostBinary is true, run the host's grub-mkstandalone with --directory
    // pointing at the target's GRUB modules.  This avoids chroot which can fail when
    // the target's dynamic linker or libraries are incompatible with the host.
    // When false, run inside the chroot (fallback if host lacks the tool).
    QString mkstandalone;
    if (chrootPath.isEmpty() || !useHostBinary) {
        const QString chroot = chrootPath.isEmpty() ? QString() : QStringLiteral("chroot %1 ").arg(chrootPath);
        mkstandalone = QStringLiteral("%1grub-mkstandalone -O %2").arg(chroot, efiTarget);
    } else {
        mkstandalone = QStringLiteral("grub-mkstandalone --directory=%1/usr/lib/grub/%2 -O %2").arg(hp, efiTarget);
    }

    // 1. Write minimal early config that chainloads the real grub.cfg
    // 2. Create EFI directory
    // 3. Run grub-mkstandalone
    // 4. Copy to fallback path EFI/BOOT/BOOT<SUFFIX>.EFI
    // 5. Clean up temp config
    //
    // When using the host binary, all paths (including the embedded config source)
    // are host-absolute.  When using chroot, -o and the config source use
    // chroot-relative paths while file ops (printf, mkdir, cp) use host-absolute.
    const QString outPath = (chrootPath.isEmpty() || useHostBinary)
        ? QStringLiteral("%1/boot/efi/EFI/%2/grub%3.efi").arg(hp, bootloaderId, suffix)
        : QStringLiteral("/boot/efi/EFI/%1/grub%2.efi").arg(bootloaderId, suffix);
    const QString cfgSource = (chrootPath.isEmpty() || useHostBinary)
        ? QStringLiteral("%1/run/grub-early.cfg").arg(hp)
        : QStringLiteral("/run/grub-early.cfg");

    return QStringLiteral(
        "mkdir -p %1/run"
        " && printf 'search --no-floppy --file /boot/grub/grub.cfg --set=root\\n"
        "set prefix=($root)/boot/grub\\n"
        "configfile $prefix/grub.cfg\\n' > %1/run/grub-early.cfg"
        " && mkdir -p %1/boot/efi/EFI/%2"
        " && %3 -o %4 \"boot/grub/grub.cfg=%5\""
        " && mkdir -p %1/boot/efi/EFI/BOOT"
        " && cp %1/boot/efi/EFI/%2/grub%6.efi %1/boot/efi/EFI/BOOT/BOOT%7.EFI"
        " && rm -f %1/run/grub-early.cfg")
        .arg(hp, bootloaderId, mkstandalone, outPath, cfgSource, suffix, upperSuffix);
}

QString detectInitramfsCmd(const QString& rootPath, Cmd* shell = nullptr)
{
    const QStringList updateInitramfs = {"/usr/sbin/update-initramfs", "/usr/bin/update-initramfs"};
    const QStringList mkinitcpio = {"/usr/bin/mkinitcpio"};
    const QStringList dracut = {"/usr/bin/dracut", "/usr/sbin/dracut"};
    auto existsInRoot = [&rootPath, shell](const QString& path) {
        if (rootPath.isEmpty()) {
            return QFile::exists(path);
        }
        if (shell) {
            const QString full = QDir(rootPath).filePath(path.mid(1));
            return shell->runAsRoot(QStringLiteral("test -x %1").arg(full), nullptr, nullptr, QuietMode::Yes);
        }
        return QFile::exists(QDir(rootPath).filePath(path.mid(1)));
    };
    for (const auto& path : updateInitramfs) {
        if (existsInRoot(path)) {
            return QStringLiteral("update-initramfs");
        }
    }
    for (const auto& path : mkinitcpio) {
        if (existsInRoot(path)) {
            return QStringLiteral("mkinitcpio");
        }
    }
    for (const auto& path : dracut) {
        if (existsInRoot(path)) {
            return QStringLiteral("dracut");
        }
    }
    return {};
}

QString detectUpdateGrubCmd(const QString& rootPath, Cmd* shell = nullptr)
{
    const QStringList updateGrub = {"/usr/sbin/update-grub", "/usr/bin/update-grub"};
    const QStringList grubMkconfig = {"/usr/bin/grub-mkconfig", "/usr/sbin/grub-mkconfig"};
    auto existsInRoot = [&rootPath, shell](const QString& path) {
        if (rootPath.isEmpty()) {
            return QFile::exists(path);
        }
        if (shell) {
            const QString full = QDir(rootPath).filePath(path.mid(1));
            return shell->runAsRoot(QStringLiteral("test -x %1").arg(full), nullptr, nullptr, QuietMode::Yes);
        }
        return QFile::exists(QDir(rootPath).filePath(path.mid(1)));
    };
    for (const auto& path : updateGrub) {
        if (existsInRoot(path)) {
            return QStringLiteral("update-grub");
        }
    }
    for (const auto& path : grubMkconfig) {
        if (existsInRoot(path)) {
            return QStringLiteral("grub-mkconfig");
        }
    }
    return {};
}
} // namespace

BootRepairEngine::BootRepairEngine(QObject* parent)
    : QObject(parent), shell(new Cmd(this))
{
    connect(shell, &Cmd::outputAvailable, this, [this](const QString& s) { emit log(s); });
    connect(shell, &Cmd::errorAvailable, this, [this](const QString& s) { emit log(s); });
}

bool BootRepairEngine::execRunAsRoot(const QString& cmd, QString* output, const QByteArray* input, bool quiet)
{
    if (currentDryRun_) {
        emit log(QStringLiteral("[dry-run] %1").arg(cmd));
        if (output) *output = {};
        return true;
    }
    if (quiet) {
        emit log(QStringLiteral("# %1").arg(cmd));
    }
    return shell->runAsRoot(cmd, output, input, quiet ? QuietMode::Yes : QuietMode::No);
}

bool BootRepairEngine::execProcAsRoot(const QString& cmd, const QStringList& args, QString* output, const QByteArray* input, bool quiet)
{
    if (currentDryRun_) {
        emit log(QStringLiteral("[dry-run] %1 %2").arg(cmd, args.join(' ')));
        if (output) *output = {};
        return true;
    }
    if (quiet) {
        emit log(QStringLiteral("# %1 %2").arg(cmd, args.join(' ')));
    }
    return shell->procAsRoot(cmd, args, output, input, quiet ? QuietMode::Yes : QuietMode::No);
}

QStringList BootRepairEngine::listDisks() const
{
    const QString cmd = QStringLiteral(
        "lsblk -ln -o NAME,SIZE,LABEL,MODEL -d -e 2,11 -x NAME | grep -E '^x?[h,s,v].[a-z]|^mmcblk|^nvme'");
    emit const_cast<BootRepairEngine*>(this)->log(QStringLiteral("$ %1").arg(cmd));
    const QSignalBlocker blocker(const_cast<BootRepairEngine*>(this)); // suppress engine::log emissions
    return shell->getCmdOut(cmd, QuietMode::Yes).split('\n', Qt::SkipEmptyParts);
}

QStringList BootRepairEngine::listPartitions() const
{
    const QString cmd = QStringLiteral(
        "lsblk -ln -o NAME,SIZE,FSTYPE,MOUNTPOINT,LABEL -e 2,11 -x NAME | grep -E "
        "'^x?[h,s,v].[a-z][0-9]|^mmcblk[0-9]+p|^nvme[0-9]+n[0-9]+p'");
    emit const_cast<BootRepairEngine*>(this)->log(QStringLiteral("$ %1").arg(cmd));
    const QSignalBlocker blocker(const_cast<BootRepairEngine*>(this)); // suppress engine::log emissions
    return shell->getCmdOut(cmd, QuietMode::Yes).split('\n', Qt::SkipEmptyParts);
}

bool BootRepairEngine::isUefi()
{
    QDir dir("/sys/firmware/efi/efivars");
    return dir.exists() && !dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty();
}

bool BootRepairEngine::isMounted(const QString& volume, const QString& mount) const
{
    return isMountedTo(volume, mount);
}

bool BootRepairEngine::isMountedTo(const QString& volume, const QString& mount) const
{
    const bool wasSuppressed = shell->outputSuppressed();
    shell->setOutputSuppressed(true);
    QString points;
    if (!shell->proc("lsblk", {"-nro", "MOUNTPOINTS", volume}, &points, nullptr, QuietMode::Yes)) {
        shell->proc("lsblk", {"-nro", "MOUNTPOINT", volume}, &points, nullptr, QuietMode::Yes);
    }
    shell->setOutputSuppressed(wasSuppressed);
    return points.split('\n', Qt::SkipEmptyParts).contains(mount);
}

QString BootRepairEngine::luksMapper(const QString& part) const
{
    QString mapper;
    if (!shell->procAsRoot("cryptsetup", {"isLuks", part}, nullptr, nullptr, QuietMode::Yes)) {
        return {};
    }
    if (!shell->procAsRoot("cryptsetup", {"luksUUID", part}, &mapper, nullptr, QuietMode::Yes)) {
        return {};
    }
    return QStringLiteral("luks-") + mapper.trimmed();
}

bool BootRepairEngine::isLuks(const QString& device) const
{
    const QString dev = device.startsWith("/dev/") ? device : ("/dev/" + device);
    return shell->procAsRoot("cryptsetup", {"isLuks", dev}, nullptr, nullptr, QuietMode::Yes);
}

static inline QString normalizeDev(const QString& device)
{
    return device.startsWith("/dev/") ? device : ("/dev/" + device);
}

QString BootRepairEngine::mountSource(const QString& mountpoint) const
{
    QString output;
    shell->proc("findmnt", {"-n", "-o", "SOURCE", "--target", mountpoint}, &output, nullptr, QuietMode::Yes);
    return output.trimmed();
}

bool BootRepairEngine::isEspPartition(const QString& device) const
{
    const QString dev = normalizeDev(device);
    const QString cmd = QStringLiteral("lsblk -ln -o PARTTYPE %1 | grep -qiE 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b|0xef'")
                            .arg(dev);
    emit const_cast<BootRepairEngine*>(this)->log(QStringLiteral("$ %1").arg(cmd));
    return shell->run(cmd, nullptr, nullptr, QuietMode::Yes);
}

bool BootRepairEngine::isLinuxPartitionType(const QString& device) const
{
    const QString dev = normalizeDev(device);
    const QString cmd = QStringLiteral("lsblk -ln -o PARTTYPE %1 | grep -qEi '0x83|0fc63daf-8483-4772-8e79-3d69d8477de4|44479540-F297-41B2-9AF7-D131D5F0458A|4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709'")
                            .arg(dev);
    emit const_cast<BootRepairEngine*>(this)->log(QStringLiteral("$ %1").arg(cmd));
    return shell->run(cmd, nullptr, nullptr, QuietMode::Yes);
}

bool BootRepairEngine::labelContains(const QString& device, const QString& needle) const
{
    const QString dev = normalizeDev(device);
    const QString cmd = QStringLiteral("lsblk -ln -o LABEL %1 | grep -q %2").arg(dev, needle);
    emit const_cast<BootRepairEngine*>(this)->log(QStringLiteral("$ %1").arg(cmd));
    return shell->run(cmd, nullptr, nullptr, QuietMode::Yes);
}

bool BootRepairEngine::openLuks(const QString& part, const QString& mapper, const QByteArray& pass)
{
    if (pass.isEmpty()) {
        emit log(QStringLiteral("LUKS device requires a password for %1").arg(part));
        return false;
    }
    return execProcAsRoot("cryptsetup", {"luksOpen", part, mapper, "-"}, nullptr, &const_cast<QByteArray&>(pass), true);
}

bool BootRepairEngine::mountChrootEnv(const QString& path)
{
    if (!tmpdir.isValid()) {
        emit log(QStringLiteral("Could not create a temporary folder"));
        return false;
    }
    if (!QFile::exists(tmpdir.path())) {
        execRunAsRoot(QStringLiteral("mkdir -p %1").arg(tmpdir.path()), nullptr, nullptr, true);
    }
    const QString cmd = QStringLiteral(
        "/bin/mount %1 %2 && /bin/mount --rbind --make-rslave /dev %2/dev && "
        "(! test -d %2/sys || /bin/mount --rbind --make-rslave /sys %2/sys) && "
        "(! test -d %2/proc || /bin/mount --rbind /proc %2/proc) && "
        "(! test -d %2/run || (/bin/mount -t tmpfs -o size=100m,nodev,mode=755 tmpfs %2/run && "
        "(! test -d /run/udev || /bin/mkdir %2/run/udev && /bin/mount --rbind /run/udev %2/run/udev)))").arg(path, tmpdir.path());
    return execRunAsRoot(cmd, nullptr, nullptr, true);
}

void BootRepairEngine::cleanupMounts(const QString& path, const QString& luks)
{
    if (path != "/") {
        execRunAsRoot("mountpoint -q " + path + "/boot/efi && umount " + path + "/boot/efi", nullptr, nullptr, true);
        execRunAsRoot("mountpoint -q " + path + "/boot && umount -R " + path + "/boot", nullptr, nullptr, true);
        const QString cmd = QStringLiteral(
                                 "mountpoint -q %1 && (! mountpoint -q %1/run || /bin/umount -R %1/run) && "
                                 "(! mountpoint -q %1/proc || /bin/umount -R %1/proc) && "
                                 "(! mountpoint -q %1/sys || /bin/umount -R %1/sys) && "
                                 "/bin/umount -R %1/dev && umount %1 && rmdir %1")
                                 .arg(path);
        execRunAsRoot(cmd, nullptr, nullptr, true);
    }
    if (!luks.isEmpty()) {
        execProcAsRoot("cryptsetup", {"luksClose", luks}, nullptr, nullptr, true);
    }
}

bool BootRepairEngine::ensureMountFor(const QString& path, const QString& mountpoint, const QString& device)
{
    // If directory is not populated, mount provided device to path/mountpoint
    if (!execRunAsRoot("test -n \"$(ls -A " + path + mountpoint + ")\"", nullptr, nullptr, true)) {
        if (device.isEmpty()) {
            emit log(QStringLiteral("No device provided for %1; please mount manually").arg(mountpoint));
            return false;
        }
        if (!execRunAsRoot("mount " + device + ' ' + path + mountpoint, nullptr, nullptr, true)) {
            emit log(QStringLiteral("Failed to mount %1 on %2%3").arg(device, path, mountpoint));
            return false;
        }
    }
    return true;
}

bool BootRepairEngine::installGrub(const BootRepairOptions& opt)
{
    currentDryRun_ = opt.dryRun;
    // Handle LUKS if needed
    QString root = opt.root;
    const QString mapper = luksMapper(root);
    if (!mapper.isEmpty() && !root.startsWith("/dev/mapper/")) {
        if (!openLuks(root, mapper, opt.luksPassword)) {
            emit finished(false);
            currentDryRun_ = false;
            return false;
        }
        root = "/dev/mapper/" + mapper;
    }

    // If installing on current root
    if (isMountedTo(root, "/")) {
        QString cmd;
        QString prevEsp; // track original ESP mount so we can restore it
        if (opt.target == GrubTarget::Esp) {
            execRunAsRoot("test -d /boot/efi || mkdir /boot/efi", nullptr, nullptr, true);
            if (!opt.espDevice.isEmpty() && !isMountedTo(opt.espDevice, "/boot/efi")) {
                shell->getCmdOut("findmnt -n -o SOURCE /boot/efi", QuietMode::Yes).trimmed().swap(prevEsp);
                if (!execRunAsRoot("mountpoint -q /boot/efi && umount /boot/efi", nullptr, nullptr, true)) {
                    emit log(QStringLiteral("Failed to unmount /boot/efi (device may be busy)"));
                    emit finished(false);
                    currentDryRun_ = false;
                    return false;
                }
                if (!execRunAsRoot("mount " + opt.espDevice + " /boot/efi", nullptr, nullptr, true)) {
                    emit log(QStringLiteral("Failed to mount %1 on /boot/efi").arg(opt.espDevice));
                    if (!prevEsp.isEmpty()
                        && !execRunAsRoot("mount " + prevEsp + " /boot/efi", nullptr, nullptr, true)) {
                        emit log(QStringLiteral("Warning: could not restore %1 on /boot/efi; please remount manually").arg(prevEsp));
                    }
                    emit finished(false);
                    currentDryRun_ = false;
                    return false;
                }
            }
            emit log(QStringLiteral("$ uname -m"));
            const QString arch = detectArch(shell);
            const QString bootloaderId = detectBootloaderId(QStringLiteral("/boot/efi"), {});
            const QString grubTool = detectGrubInstallCmd({});
            if (grubTool == "grub-install") {
                const QString helpCmd = QStringLiteral("grub-install --help");
                const bool forceExtraRemovable = grubSupportsForceExtraRemovable(shell, helpCmd, Elevation::No);
                cmd = grubInstallCmd(QStringLiteral("grub-install"), arch, bootloaderId, forceExtraRemovable);
            } else if (grubTool == "grub-mkstandalone") {
                cmd = grubMkstandaloneCmd({}, arch, bootloaderId, false);
            } else {
                emit log(QStringLiteral("No GRUB installation tool found (grub-install/grub-mkstandalone)."));
                if (!prevEsp.isEmpty()) {
                    if (!execRunAsRoot("umount /boot/efi", nullptr, nullptr, true)
                        || !execRunAsRoot("mount " + prevEsp + " /boot/efi", nullptr, nullptr, true)) {
                        emit log(QStringLiteral("Warning: could not restore %1 on /boot/efi; please remount manually").arg(prevEsp));
                    }
                }
                emit finished(false);
                currentDryRun_ = false;
                return false;
            }
        } else {
            const QString grubTool = detectGrubInstallCmd({});
            if (grubTool == "grub-install") {
                cmd = QStringLiteral("grub-install --target=i386-pc --recheck --force /dev/%1").arg(opt.location);
            } else {
                emit log(QStringLiteral("grub-install is required for MBR/Root target but was not found."));
                emit finished(false);
                currentDryRun_ = false;
                return false;
            }
        }
        const bool ok = execRunAsRoot(cmd, nullptr, nullptr, true);
        if (!prevEsp.isEmpty()) {
            if (!execRunAsRoot("umount /boot/efi", nullptr, nullptr, true)
                || !execRunAsRoot("mount " + prevEsp + " /boot/efi", nullptr, nullptr, true)) {
                emit log(QStringLiteral("Warning: could not restore %1 on /boot/efi; please remount manually").arg(prevEsp));
            }
        }
        emit finished(ok);
        currentDryRun_ = false;
        return ok;
    }

    // Otherwise, chroot into target root
    if (opt.target == GrubTarget::Esp) {
        // Mount efivarfs if needed for NVRAM
        execRunAsRoot("grep -sq ^efivarfs /proc/self/mounts || { test -d /sys/firmware/efi/efivars && mount -t efivarfs efivarfs /sys/firmware/efi/efivars; }",
                        nullptr, nullptr, true);
        execRunAsRoot("ls -1 /sys/firmware/efi/efivars | grep -sq ^dump && rm /sys/firmware/efi/efivars/dump*",
                        nullptr, nullptr, true);
    }

    if (!mountChrootEnv(root)) {
        emit log(QStringLiteral("Failed to set up chroot"));
        emit finished(false);
        currentDryRun_ = false;
        return false;
    }
    if (!ensureMountFor(tmpdir.path(), "/boot", opt.bootDevice)) {
        cleanupMounts(tmpdir.path(), mapper);
        emit finished(false);
        currentDryRun_ = false;
        return false;
    }

    QString cmd;
    if (opt.target == GrubTarget::Esp) {
        execRunAsRoot("test -d " + tmpdir.path() + "/boot/efi || mkdir " + tmpdir.path() + "/boot/efi",
                        nullptr, nullptr, true);
        if (!ensureMountFor(tmpdir.path(), "/boot/efi", opt.espDevice)) {
            cleanupMounts(tmpdir.path(), mapper);
            emit finished(false);
            currentDryRun_ = false;
            return false;
        }
        emit log(QStringLiteral("$ uname -m"));
        const QString arch = detectArch(shell);
        const QString bootloaderId = detectBootloaderId(tmpdir.path() + "/boot/efi", tmpdir.path(), shell);
        const QString grubTool = detectGrubInstallCmd(tmpdir.path(), shell);
        if (grubTool == "grub-install") {
            const QString helpCmd = QStringLiteral("chroot %1 grub-install --help").arg(tmpdir.path());
            const bool forceExtraRemovable = grubSupportsForceExtraRemovable(shell, helpCmd, Elevation::Yes);
            cmd = grubInstallCmd(QStringLiteral("chroot %1 grub-install").arg(tmpdir.path()), arch, bootloaderId, forceExtraRemovable);
        } else if (grubTool == "grub-mkstandalone") {
            // Prefer the host's grub-mkstandalone with --directory pointing at
            // the target's modules — avoids chroot dynamic linker issues.
            const bool hostHasMkstandalone = QFile::exists("/usr/bin/grub-mkstandalone")
                || QFile::exists("/usr/sbin/grub-mkstandalone");
            cmd = grubMkstandaloneCmd(tmpdir.path(), arch, bootloaderId, hostHasMkstandalone);
        } else {
            emit log(QStringLiteral("No GRUB installation tool found in target root (grub-install/grub-mkstandalone)."));
            cleanupMounts(tmpdir.path(), mapper);
            emit finished(false);
            currentDryRun_ = false;
            return false;
        }
    } else {
        const QString grubTool = detectGrubInstallCmd(tmpdir.path(), shell);
        if (grubTool == "grub-install") {
            cmd = QStringLiteral("chroot %1 grub-install --target=i386-pc --recheck --force /dev/%2").arg(tmpdir.path(), opt.location);
        } else {
            emit log(QStringLiteral("grub-install is required for MBR/Root target but was not found in target root."));
            cleanupMounts(tmpdir.path(), mapper);
            emit finished(false);
            currentDryRun_ = false;
            return false;
        }
    }

    const bool ok = execRunAsRoot(cmd, nullptr, nullptr, true);
    cleanupMounts(tmpdir.path(), mapper);
    emit finished(ok);
    currentDryRun_ = false;
    return ok;
}

bool BootRepairEngine::repairGrub(const BootRepairOptions& opt)
{
    currentDryRun_ = opt.dryRun;
    QString root = opt.root;
    const QString mapper = luksMapper(root);
    if (!mapper.isEmpty() && !root.startsWith("/dev/mapper/")) {
        if (!openLuks(root, mapper, opt.luksPassword)) {
            emit finished(false);
            currentDryRun_ = false;
            return false;
        }
        root = "/dev/mapper/" + mapper;
    }

    if (isMountedTo(root, "/")) {
        const QString tool = detectUpdateGrubCmd({});
        bool ok = false;
        if (tool == "update-grub") {
            ok = execRunAsRoot("update-grub", nullptr, nullptr, true);
        } else if (tool == "grub-mkconfig") {
            ok = execRunAsRoot("grub-mkconfig -o /boot/grub/grub.cfg", nullptr, nullptr, true);
        } else {
            emit log(QStringLiteral("No GRUB config tool found (update-grub/grub-mkconfig)."));
            emit finished(false);
            currentDryRun_ = false;
            return false;
        }
        emit finished(ok);
        currentDryRun_ = false;
        return ok;
    }

    if (!mountChrootEnv(root)) {
        emit finished(false);
        currentDryRun_ = false;
        return false;
    }
    // Ensure /boot and possibly /boot/efi
    if (!ensureMountFor(tmpdir.path(), "/boot", opt.bootDevice)) {
        cleanupMounts(tmpdir.path(), mapper);
        emit finished(false);
        return false;
    }
    if (shell->runAsRoot(QStringLiteral("test -d %1/boot/efi").arg(tmpdir.path()), nullptr, nullptr, QuietMode::Yes)) {
        ensureMountFor(tmpdir.path(), "/boot/efi", opt.espDevice);
    }
    const QString tool = detectUpdateGrubCmd(tmpdir.path(), shell);
    bool ok = false;
    if (tool == "update-grub") {
        ok = execRunAsRoot(QStringLiteral("chroot %1 update-grub").arg(tmpdir.path()), nullptr, nullptr, true);
    } else if (tool == "grub-mkconfig") {
        ok = execRunAsRoot(QStringLiteral("chroot %1 grub-mkconfig -o /boot/grub/grub.cfg").arg(tmpdir.path()),
                           nullptr, nullptr, true);
    } else {
        emit log(QStringLiteral("No GRUB config tool found in target root (update-grub/grub-mkconfig)."));
        cleanupMounts(tmpdir.path(), mapper);
        emit finished(false);
        currentDryRun_ = false;
        return false;
    }
    cleanupMounts(tmpdir.path(), mapper);
    emit finished(ok);
    currentDryRun_ = false;
    return ok;
}

bool BootRepairEngine::regenerateInitramfs(const BootRepairOptions& opt)
{
    currentDryRun_ = opt.dryRun;
    QString root = opt.root;
    const QString mapper = luksMapper(root);
    if (!mapper.isEmpty() && !root.startsWith("/dev/mapper/")) {
        if (!openLuks(root, mapper, opt.luksPassword)) {
            emit finished(false);
            currentDryRun_ = false;
            return false;
        }
        root = "/dev/mapper/" + mapper;
    }

    if (isMountedTo(root, "/")) {
        const QString tool = detectInitramfsCmd({});
        bool ok = false;
        if (tool == "update-initramfs") {
            ok = execProcAsRoot("update-initramfs", {"-c", "-v", "-k", "all"}, nullptr, nullptr, true);
        } else if (tool == "mkinitcpio") {
            ok = execProcAsRoot("mkinitcpio", {"-P"}, nullptr, nullptr, true);
        } else if (tool == "dracut") {
            ok = execProcAsRoot("dracut", {"--regenerate-all", "--force"}, nullptr, nullptr, true);
        } else {
            emit log(QStringLiteral("No initramfs generator found (update-initramfs/mkinitcpio/dracut)."));
            emit finished(false);
            currentDryRun_ = false;
            return false;
        }
        emit finished(ok);
        currentDryRun_ = false;
        return ok;
    }

    if (!mountChrootEnv(root)) {
        emit finished(false);
        currentDryRun_ = false;
        return false;
    }
    if (!ensureMountFor(tmpdir.path(), "/boot", opt.bootDevice)) {
        cleanupMounts(tmpdir.path(), mapper);
        emit finished(false);
        return false;
    }
    const QString tool = detectInitramfsCmd(tmpdir.path(), shell);
    bool ok = false;
    if (tool == "update-initramfs") {
        ok = execProcAsRoot("chroot", {tmpdir.path(), "update-initramfs", "-c", "-v", "-k", "all"}, nullptr, nullptr, true);
    } else if (tool == "mkinitcpio") {
        ok = execProcAsRoot("chroot", {tmpdir.path(), "mkinitcpio", "-P"}, nullptr, nullptr, true);
    } else if (tool == "dracut") {
        ok = execProcAsRoot("chroot", {tmpdir.path(), "dracut", "--regenerate-all", "--force"}, nullptr, nullptr, true);
    } else {
        emit log(QStringLiteral("No initramfs generator found in target root (update-initramfs/mkinitcpio/dracut)."));
        cleanupMounts(tmpdir.path(), mapper);
        emit finished(false);
        currentDryRun_ = false;
        return false;
    }
    cleanupMounts(tmpdir.path(), mapper);
    emit finished(ok);
    currentDryRun_ = false;
    return ok;
}

QString BootRepairEngine::resolveFstabDevice(const QString& root, const QString& mountpoint, const QByteArray& luksPass)
{
    QString part = root;
    const QString mapper = luksMapper(part);
    bool opened = false;
    if (!mapper.isEmpty() && !part.startsWith("/dev/mapper/")) {
        if (!openLuks(part, mapper, luksPass)) {
            return {};
        }
        part = "/dev/mapper/" + mapper;
        opened = true;
    }
    if (!mountChrootEnv(part)) {
        if (opened) shell->procAsRoot("cryptsetup", {"luksClose", mapper}, nullptr, nullptr, QuietMode::Yes);
        return {};
    }
    QString device;
    QString fstabContent;
    if (shell->runAsRoot(QStringLiteral("cat %1/etc/fstab").arg(tmpdir.path()), &fstabContent, nullptr, QuietMode::Yes)) {
        const QStringList lines = fstabContent.split('\n', Qt::SkipEmptyParts);
        for (const auto& rawLine : lines) {
            const QString line = rawLine.simplified();
            if (line.isEmpty() || line.startsWith('#')) continue;
            const QStringList fields = line.split(' ');
            if (fields.size() >= 2 && fields.at(1) == mountpoint) {
                device = fields.at(0);
                break;
            }
        }
    }
    QString resolved;
    if (!device.isEmpty()) {
        const QString cmd = "readlink -e \"$(echo " + device
                           + " | sed -r 's:((PART)?(UUID|LABEL))=:\\L/dev/disk/by-\\1/:g; s:[\\\"]::g;')\"";
        if (shell->runAsRoot(cmd, &resolved, nullptr, QuietMode::Yes)) {
            resolved = resolved.trimmed();
        }
    }
    cleanupMounts(tmpdir.path(), opened ? mapper : QString());
    return resolved;
}

bool BootRepairEngine::backup(const BootRepairOptions& opt)
{
    currentDryRun_ = opt.dryRun;
    if (opt.location.isEmpty() || opt.backupPath.isEmpty()) return false;
    const QString cmd = QStringLiteral("dd if=/dev/%1 of=%2 bs=446 count=1").arg(opt.location, opt.backupPath);
    const bool ok = execRunAsRoot(cmd, nullptr, nullptr, true);
    emit finished(ok);
    currentDryRun_ = false;
    return ok;
}

bool BootRepairEngine::restore(const BootRepairOptions& opt)
{
    currentDryRun_ = opt.dryRun;
    if (opt.location.isEmpty() || opt.backupPath.isEmpty()) return false;
    const QString cmd = QStringLiteral("dd if=%1 of=/dev/%2 bs=446 count=1").arg(opt.backupPath, opt.location);
    const bool ok = execRunAsRoot(cmd, nullptr, nullptr, true);
    emit finished(ok);
    currentDryRun_ = false;
    return ok;
}
