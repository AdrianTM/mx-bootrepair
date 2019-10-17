/*****************************************************************************
 * mainwindow.cpp
 *****************************************************************************
 * Copyright (C) 2014 MX Authors
 *
 * Authors: Adrian
 *          MX Linux <http://mxlinux.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MX Boot Repair is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MX Boot Repair.  If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/

#include "about.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "version.h"

#include <QDebug>
#include <QFileDialog>
#include <QScrollBar>

MainWindow::MainWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::MainWindow)
{
    qDebug().noquote() << QCoreApplication::applicationName() << "version:" << VERSION;
    ui->setupUi(this);
    timer = new QTimer(this);
    shell = new Cmd(this);

    connect(shell, &Cmd::outputAvailable, [](const QString &out) {qDebug() << out.trimmed();});
    connect(shell, &Cmd::errorAvailable, [](const QString &out) {qWarning() << out.trimmed();});

    setWindowFlags(Qt::Window); // for the close, min and max buttons
    refresh();
    addDevToList();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::refresh() {
    ui->stackedWidget->setCurrentIndex(0);
    ui->reinstallRadioButton->setFocus();
    ui->reinstallRadioButton->setChecked(true);
    ui->progressBar->hide();
    ui->progressBar->setValue(0);
    ui->outputBox->setPlainText("");
    ui->outputLabel->setText("");
    ui->grubInsLabel->show();
    ui->grubRootButton->show();
    ui->grubMbrButton->show();
    ui->grubEspButton->show();
    ui->rootLabel->hide();
    ui->rootCombo->hide();
    ui->buttonApply->setText(tr("Apply"));
    ui->buttonApply->setIcon(QIcon::fromTheme("dialog-ok"));
    ui->buttonApply->setEnabled(true);
    ui->buttonCancel->setEnabled(true);
    ui->rootCombo->setDisabled(false);
    setCursor(QCursor(Qt::ArrowCursor));
}

void MainWindow::installGRUB() {
    QString cmd;
    ui->progressBar->show();
    setCursor(QCursor(Qt::WaitCursor));
    ui->buttonCancel->setEnabled(false);
    ui->buttonApply->setEnabled(false);
    ui->stackedWidget->setCurrentWidget(ui->outputPage);

    QString location = QString(ui->grubBootCombo->currentText()).section(" ", 0, 0);
    QString root = QString(ui->rootCombo->currentText()).section(" ", 0, 0);
    QString text = QString(tr("GRUB is being installed on %1 device.")).arg(location);
    ui->outputLabel->setText(text);

    // create a temp folder and mount dev sys proc
    QString path = shell->getCmdOut("mktemp -d --tmpdir -p /tmp");
    cmd = QString("mount /dev/%1 %2 && mount -o bind /dev %2/dev && mount -o bind /sys %2/sys && mount -o bind /proc %2/proc").arg(root).arg(path);
    if (shell->run(cmd)) {
        cmd = QString("chroot %1 grub-install --target=i386-pc --recheck --force /dev/%2").arg(path).arg(location);
        if (ui->grubEspButton->isChecked()) {
            shell->run("test -d " + path.toUtf8() + "/boot/efi || mkdir " + path.toUtf8()  + "/boot/efi");
            if (!shell->run("mount /dev/" + location.toUtf8()  + " " + path.toUtf8() + "/boot/efi")) {
                QMessageBox::critical(this, tr("Error"),
                                      tr("Could not mount ") + location + tr(" on /boot/efi"));
                setCursor(QCursor(Qt::ArrowCursor));
                ui->buttonApply->setEnabled(true);
                ui->buttonCancel->setEnabled(true);
                ui->progressBar->hide();
                ui->stackedWidget->setCurrentWidget(ui->selectionPage);
                return;
            }
            QString arch = shell->getCmdOut("arch");
            if (arch == "i686") { // rename arch to match grub-install target
                arch = "i386";
            }
            QString release = shell->getCmdOut("grep -oP '(?<=DISTRIB_RELEASE=).*' /etc/lsb-release");
            cmd = QString("chroot %1 grub-install --target=%2-efi --efi-directory=/boot/efi --bootloader-id=MX%3 --recheck\"").arg(path).arg(arch).arg(release);
        }
        displayOutput();
        shell->run(cmd);
        disableOutput();

        // umount and clean temp folder
        shell->run("mountpoint -q " + path.toUtf8() + "/boot/efi && umount " + path.toUtf8() + "/boot/efi");
        cmd = QString("umount %1/proc %1/sys %1/dev; umount %1; rmdir %1").arg(path);
        shell->run(cmd.toUtf8());
    } else {
        QMessageBox::critical(this, tr("Error"),
                              tr("Could not set up chroot environment.\nPlease double-check the selected location."));
        setCursor(QCursor(Qt::ArrowCursor));
        ui->buttonApply->setEnabled(true);
        ui->buttonCancel->setEnabled(true);
        ui->progressBar->hide();
        ui->stackedWidget->setCurrentWidget(ui->selectionPage);
    }
}

void MainWindow::repairGRUB() {
    QString cmd;
    ui->progressBar->show();
    setCursor(QCursor(Qt::WaitCursor));
    ui->buttonCancel->setEnabled(false);
    ui->buttonApply->setEnabled(false);
    ui->stackedWidget->setCurrentWidget(ui->outputPage);
    QString location = QString(ui->grubBootCombo->currentText()).section(" ", 0, 0);
    ui->outputLabel->setText(tr("The GRUB configuration file (grub.cfg) is being rebuilt."));
    // create a temp folder and mount dev sys proc
    QString path = shell->getCmdOut("mktemp -d --tmpdir -p /mnt");
    cmd = QString("mount /dev/%1 %2 && mount -o bind /dev %2/dev && mount -o bind /sys %2/sys && mount -o bind /proc %2/proc").arg(location).arg(path);
    if (shell->run(cmd)) {
        QEventLoop loop;
        cmd = QString("chroot %1 update-grub").arg(path);
        displayOutput();
        shell->run(cmd);
        disableOutput();
    } else {
        QMessageBox::critical(this, tr("Error"),
                              tr("Could not set up chroot environment.\nPlease double-check the selected location."));
        setCursor(QCursor(Qt::ArrowCursor));
        ui->buttonApply->setEnabled(true);
        ui->buttonCancel->setEnabled(true);
        ui->progressBar->hide();

        ui->stackedWidget->setCurrentWidget(ui->selectionPage);
    }
    // umount and clean temp folder
    cmd = QString("umount %1/proc %1/sys %1/dev; umount %1; rmdir %1").arg(path);
    shell->run(cmd);
}


void MainWindow::backupBR(QString filename) {
    ui->progressBar->show();
    setCursor(QCursor(Qt::WaitCursor));
    ui->buttonCancel->setEnabled(false);
    ui->buttonApply->setEnabled(false);
    ui->stackedWidget->setCurrentWidget(ui->outputPage);
    QString location = QString(ui->grubBootCombo->currentText()).section(" ", 0, 0);
    QString text = QString(tr("Backing up MBR or PBR from %1 device.")).arg(location);
    ui->outputLabel->setText(text);
    QString cmd = "dd if=/dev/" + location + " of=" + filename + " bs=446 count=1";
    displayOutput();
    shell->run(cmd);
    disableOutput();
}

// try to guess partition to install GRUB
void MainWindow::guessPartition()
{
    if (ui->grubMbrButton->isChecked()) {
        // find first disk with Linux partitions
        for (int index = 0; index < ui->grubBootCombo->count(); index++) {
            QString drive = ui->grubBootCombo->itemText(index);
            if (shell->run("lsblk -ln -o PARTTYPE /dev/" + drive.section(" ", 0 ,0).toUtf8() + \
                       "| grep -qEi '0x83|0fc63daf-8483-4772-8e79-3d69d8477de4|44479540-F297-41B2-9AF7-D131D5F0458A|4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709'")) {
                ui->grubBootCombo->setCurrentIndex(index);
                break;
            }
        }
    }
    // find first a partition with rootMX* label
    for (int index = 0; index < ui->rootCombo->count(); index++) {
        QString part = ui->rootCombo->itemText(index);
        if (shell->run("lsblk -ln -o LABEL /dev/" + part.section(" ", 0 ,0).toUtf8() + "| grep -q rootMX")) {
            ui->rootCombo->setCurrentIndex(index);
            // select the same location by default for GRUB and /boot
            if (ui->grubRootButton->isChecked()) {
                ui->grubBootCombo->setCurrentIndex(ui->rootCombo->currentIndex());
            }
            return;
        }
    }
    // it it cannot find rootMX*, look for Linux partitions
    for (int index = 0; index < ui->rootCombo->count(); index++) {
        QString part = ui->rootCombo->itemText(index);
        if (shell->run("lsblk -ln -o PARTTYPE /dev/" + part.section(" ", 0 ,0).toUtf8() + \
                   "| grep -qEi '0x83|0fc63daf-8483-4772-8e79-3d69d8477de4|44479540-F297-41B2-9AF7-D131D5F0458A|4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709'")) {
            ui->rootCombo->setCurrentIndex(index);
            break;
        }
    }
    // use by default the same root and /boot partion for installing on root
    if (ui->grubRootButton->isChecked()) {
        ui->grubBootCombo->setCurrentIndex(ui->rootCombo->currentIndex());
    }
}

void MainWindow::restoreBR(QString filename) {
    ui->progressBar->show();
    setCursor(QCursor(Qt::WaitCursor));
    ui->buttonCancel->setEnabled(false);
    ui->buttonApply->setEnabled(false);
    ui->stackedWidget->setCurrentWidget(ui->outputPage);
    QString location = QString(ui->grubBootCombo->currentText()).section(" ", 0, 0);
    if (QMessageBox::warning(this, tr("Warning"),
                             tr("You are going to write the content of ") + filename + tr(" to ") + location + tr("\n\nAre you sure?"),
                             QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes) {
        refresh();
        return;
    }
    QString text = QString(tr("Restoring MBR/PBR from backup to %1 device.")).arg(location);
    ui->outputLabel->setText(text);
    QString cmd = "dd if=" + filename + " of=/dev/" + location + " bs=446 count=1";
    displayOutput();
    shell->run(cmd);
    disableOutput();
}

// select ESP GUI items
void MainWindow::setEspDefaults()
{
    // remove non-ESP partitions
    for (int index = 0; index < ui->grubBootCombo->count(); index++) {
        QString part = ui->grubBootCombo->itemText(index);
        if (!shell->run("lsblk -ln -o PARTTYPE /dev/" + part.section(" ", 0 ,0).toUtf8() + "| grep -qi c12a7328-f81f-11d2-ba4b-00a0c93ec93b")) {
            ui->grubBootCombo->removeItem(index);
            index--;
        }
    }
    if (ui->grubBootCombo->count() == 0) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Could not find EFI system partition (ESP) on any system disks. Please create an ESP and try again."));
        ui->buttonApply->setDisabled(true);
    }
}

void MainWindow::procStart() {
    timer->start(100);
    setCursor(QCursor(Qt::BusyCursor));
}

void MainWindow::progress() {
    if (ui->progressBar->value() == 100) {
        ui->progressBar->reset();
    }
    ui->progressBar->setValue(ui->progressBar->value() + 1);
}

void MainWindow::procDone() {
    timer->stop();
    ui->progressBar->setValue(100);
    setCursor(QCursor(Qt::ArrowCursor));
    ui->buttonCancel->setEnabled(true);
    ui->buttonApply->setEnabled(true);
    if (shell->exitStatus() == QProcess::NormalExit && shell->exitCode() == 0 ) {
        if (QMessageBox::information(this, tr("Success"),
                                     tr("Process finished with success.<p><b>Do you want to exit MX Boot Repair?</b>"),
                                     QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes){
            qApp->exit(EXIT_SUCCESS);
        }
    } else {
        QMessageBox::critical(this, tr("Error"),
                              tr("Process finished. Errors have occurred."));
    }
    ui->buttonApply->setText(tr("Back"));
    ui->buttonApply->setIcon(QIcon::fromTheme("go-previous"));
}

void MainWindow::displayOutput()
{
    connect(timer, &QTimer::timeout, this, &MainWindow::progress);
    connect(shell, &Cmd::started, this, &MainWindow::procStart);
    connect(shell, &Cmd::outputAvailable, this, &MainWindow::outputAvailable);
    connect(shell, &Cmd::errorAvailable, this, &MainWindow::outputAvailable);
    connect(shell, &Cmd::finished, this, &MainWindow::procDone);
}

void MainWindow::disableOutput()
{
    disconnect(timer, &QTimer::timeout, this, &MainWindow::progress);
    disconnect(shell, &Cmd::started, this, &MainWindow::procStart);
    disconnect(shell, &Cmd::outputAvailable, this, &MainWindow::outputAvailable);
    disconnect(shell, &Cmd::errorAvailable, this, &MainWindow::outputAvailable);
    disconnect(shell, &Cmd::finished, this, &MainWindow::procDone);
}


// add list of devices to grubBootCombo
void MainWindow::addDevToList() {
    QString cmd = "/bin/bash -c \"lsblk -ln -o NAME,SIZE,LABEL,MODEL -d -e 2,11 -x NAME | grep -E '^x?[h,s,v].[a-z]|^mmcblk|^nvme'\"";
    ListDisk = shell->getCmdOut(cmd).split("\n", QString::SkipEmptyParts);

    cmd = "/bin/bash -c \"lsblk -ln -o NAME,SIZE,FSTYPE,MOUNTPOINT,LABEL -e 2,11 -x NAME | grep -E '^x?[h,s,v].[a-z][0-9]|^mmcblk[0-9]+p|^nvme[0-9]+n[0-9]+p'\"";
    ListPart = shell->getCmdOut(cmd).split("\n", QString::SkipEmptyParts);
    ui->rootCombo->clear();
    ui->rootCombo->addItems(ListPart);

    ui->grubBootCombo->clear();
    // add only disks
    if (ui->grubMbrButton->isChecked()) {
        ui->grubBootCombo->addItems(ListDisk);
    } else { // add partition
        ui->grubBootCombo->addItems(ListPart);
    }

}

// enabled/disable GUI elements depending on MBR, Root or ESP selection
void MainWindow::targetSelection() {
    ui->grubBootCombo->clear();
    ui->rootCombo->setEnabled(true);
    ui->buttonApply->setEnabled(true);
    // add only disks
    if (ui->grubMbrButton->isChecked()) {
        ui->grubBootCombo->addItems(ListDisk);
        guessPartition();
        // add partitions if select root
    } else if (ui->grubRootButton->isChecked()) {
        ui->grubBootCombo->addItems(ListPart);
        guessPartition();
        // if Esp is checked, add partitions to Location combobox
    } else {
        ui->grubBootCombo->addItems(ListPart);
        guessPartition();
        setEspDefaults();
    }
}

// update output box on Stdout
void MainWindow::outputAvailable(const QString &output)
{
    if (output.contains("\r")) {
        ui->outputBox->moveCursor(QTextCursor::Up, QTextCursor::KeepAnchor);
        ui->outputBox->moveCursor(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
    }
    ui->outputBox->insertPlainText(output);
    ui->outputBox->verticalScrollBar()->setValue(ui->outputBox->verticalScrollBar()->maximum());
}

// Apply button clicked
void MainWindow::on_buttonApply_clicked() {
    // on first page
    if (ui->stackedWidget->currentIndex() == 0) {
        targetSelection();
        // Reinstall button selected
        if (ui->reinstallRadioButton->isChecked()) {
            ui->stackedWidget->setCurrentWidget(ui->selectionPage);
            ui->bootMethodGroup->setTitle(tr("Select Boot Method"));
            ui->grubInsLabel->setText(tr("Install on:"));
            ui->grubRootButton->setText(tr("root"));
            ui->rootLabel->show();
            ui->rootCombo->show();

            // Repair button selected
        } else if (ui->repairRadioButton->isChecked()) {
            ui->stackedWidget->setCurrentWidget(ui->selectionPage);
            ui->bootMethodGroup->setTitle(tr("Select GRUB location"));
            ui->grubInsLabel->hide();
            ui->grubRootButton->hide();
            ui->grubMbrButton->hide();
            ui->grubEspButton->hide();
            ui->grubRootButton->setChecked(true);
            on_grubRootButton_clicked();

            // Backup button selected
        } else if (ui->bakRadioButton->isChecked()) {
            ui->stackedWidget->setCurrentWidget(ui->selectionPage);
            ui->bootMethodGroup->setTitle(tr("Select Item to Back Up"));
            ui->grubInsLabel->setText("");
            ui->grubRootButton->setText("PBR");
            ui->grubEspButton->hide();
            // Restore backup button selected
        } else if (ui->restoreBakRadioButton->isChecked()) {
            ui->stackedWidget->setCurrentWidget(ui->selectionPage);
            ui->bootMethodGroup->setTitle(tr("Select Item to Restore"));
            ui->grubInsLabel->setText("");
            ui->grubRootButton->setText("PBR");
            ui->grubEspButton->hide();
        }

        // on selection page
    } else if (ui->stackedWidget->currentWidget() == ui->selectionPage) {
        if (ui->reinstallRadioButton->isChecked()) {
            installGRUB();
        } else if (ui->bakRadioButton->isChecked()) {
            QString filename = QFileDialog::getSaveFileName(this, tr("Select backup file name"));
            if (filename == "") {
                QMessageBox::critical(this, tr("Error"), tr("No file was selected."));
                return;
            }
            backupBR(filename);
        } else if (ui->restoreBakRadioButton->isChecked()) {
            QString filename = QFileDialog::getOpenFileName(this, tr("Select MBR or PBR backup file"));
            if (filename == "") {
                QMessageBox::critical(this, tr("Error"), tr("No file was selected."));
                return;
            }
            restoreBR(filename);
        } else if (ui->repairRadioButton->isChecked()) {
            repairGRUB();
        }
        // on output page
    } else if (ui->stackedWidget->currentWidget() == ui->outputPage) {
        refresh();
    } else {
        qApp->exit(EXIT_SUCCESS);
    }
}

// About button clicked
void MainWindow::on_buttonAbout_clicked() {
    this->hide();
    displayAboutMsgBox(tr("About %1").arg(this->windowTitle()), "<p align=\"center\"><b><h2>" + this->windowTitle() +"</h2></b></p><p align=\"center\">" +
                       tr("Version: ") + VERSION + "</p><p align=\"center\"><h3>" +
                       tr("Simple boot repair program for MX Linux") +
                       "</h3></p><p align=\"center\"><a href=\"http://mxlinux.org\">http://mxlinux.org</a><br /></p><p align=\"center\">" +
                       tr("Copyright (c) MX Linux") + "<br /><br /></p>",
                       "/usr/share/doc/mx-bootrepair/license.html", tr("%1 License").arg(this->windowTitle()), true);
    this->show();
}

// Help button clicked
void MainWindow::on_buttonHelp_clicked() {
    QLocale locale;
    QString lang = locale.bcp47Name();
    QString user = shell->getCmdOut("logname");

    QString url = "/usr/share/doc/mx-bootrepair/help/mx-bootrepair.html";
    if (lang.startsWith("fr")) {
        url = "https://mxlinux.org/wiki/help-files/help-r%C3%A9paration-d%E2%80%99amor%C3%A7age";
    }
    displayDoc(url, tr("%1 Help").arg(this->windowTitle()), true);
}

void MainWindow::on_grubMbrButton_clicked()
{
    targetSelection();
}

void MainWindow::on_grubRootButton_clicked()
{
    targetSelection();
}

void MainWindow::on_grubEspButton_clicked()
{
    targetSelection();
}
