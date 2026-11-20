#include "FileManagerDialog.h"
#include "ui_FileManagerDialog.h"

#include <QStyle>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QHeaderView>
#include <QMessageBox>
#include <QInputDialog>

namespace inputleap {

FileManagerDialog::FileManagerDialog(FileTransferService* service, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::FileManagerDialog)
    , m_service(service)
{
    ui->setupUi(this);

    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(true);
    setStyleSheet("QDialog { background-color: rgb(35, 37, 46); }");

    // Ensure layout gives all vertical space to the dual-pane splitter
    ui->verticalLayout->setStretch(0, 0);
    ui->verticalLayout->setStretch(1, 1);
    ui->verticalLayout->setStretch(2, 0);

    // Balance the local and remote file panes
    ui->splitter->setStretchFactor(0, 5);
    ui->splitter->setStretchFactor(1, 0);
    ui->splitter->setStretchFactor(2, 5);

    // Apply standard OS icons (zero emojis)
    setupIcons();

    // Configure table header formatting
    ui->tableLocal->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->tableLocal->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->tableLocal->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->tableLocal->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    ui->tableRemote->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->tableRemote->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->tableRemote->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->tableRemote->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    // Initial local directory
    m_currentLocalPath = QDir::homePath();
    ui->editLocalPath->setText(m_currentLocalPath);
    populateLocalDirectory(m_currentLocalPath);

    // Connect UI signals
    connect(ui->comboDevices, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &FileManagerDialog::onDeviceSelected);
    connect(ui->btnRefreshDevices, &QPushButton::clicked, this, &FileManagerDialog::onRefreshDevicesClicked);
    connect(ui->btnAddDevice, &QPushButton::clicked, this, &FileManagerDialog::onAddDeviceClicked);

    // Local buttons
    connect(ui->btnLocalUp, &QPushButton::clicked, this, &FileManagerDialog::onLocalUpClicked);
    connect(ui->btnLocalHome, &QPushButton::clicked, this, &FileManagerDialog::onLocalHomeClicked);
    connect(ui->btnLocalRefresh, &QPushButton::clicked, this, &FileManagerDialog::onLocalRefreshClicked);
    connect(ui->editLocalPath, &QLineEdit::returnPressed, this, &FileManagerDialog::onLocalNavigate);
    connect(ui->tableLocal, &QTableWidget::cellDoubleClicked, this, &FileManagerDialog::onLocalItemDoubleClicked);

    // Remote buttons
    connect(ui->btnRemoteUp, &QPushButton::clicked, this, &FileManagerDialog::onRemoteUpClicked);
    connect(ui->btnRemoteHome, &QPushButton::clicked, this, &FileManagerDialog::onRemoteHomeClicked);
    connect(ui->btnRemoteRefresh, &QPushButton::clicked, this, &FileManagerDialog::onRemoteRefreshClicked);
    connect(ui->editRemotePath, &QLineEdit::returnPressed, this, &FileManagerDialog::onRemoteNavigate);
    connect(ui->tableRemote, &QTableWidget::cellDoubleClicked, this, &FileManagerDialog::onRemoteItemDoubleClicked);

    // Transfer actions
    connect(ui->btnUpload, &QPushButton::clicked, this, &FileManagerDialog::onUploadClicked);
    connect(ui->btnDownload, &QPushButton::clicked, this, &FileManagerDialog::onDownloadClicked);
    connect(ui->btnCancelTransfer, &QPushButton::clicked, this, &FileManagerDialog::onCancelTransferClicked);
    connect(ui->btnClose, &QPushButton::clicked, this, &QDialog::accept);

    // Service signals
    if (m_service) {
        connect(m_service, &FileTransferService::deviceConnected, this, &FileManagerDialog::onDeviceConnected);
        connect(m_service, &FileTransferService::deviceDisconnected, this, &FileManagerDialog::onDeviceDisconnected);
        connect(m_service, &FileTransferService::directoryListed, this, &FileManagerDialog::onDirectoryListed);
        connect(m_service, &FileTransferService::transferProgress, this, &FileManagerDialog::onTransferProgress);
        connect(m_service, &FileTransferService::transferFinished, this, &FileManagerDialog::onTransferFinished);
        connect(m_service, &FileTransferService::statusMessage, this, &FileManagerDialog::onStatusMessage);
    }
}

FileManagerDialog::~FileManagerDialog()
{
    delete ui;
}

void FileManagerDialog::setupIcons()
{
    // Apply proper Qt standard system icons without emojis
    ui->btnLocalUp->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    ui->btnLocalHome->setIcon(style()->standardIcon(QStyle::SP_DirHomeIcon));
    ui->btnLocalRefresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));

    ui->btnRemoteUp->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    ui->btnRemoteHome->setIcon(style()->standardIcon(QStyle::SP_DirHomeIcon));
    ui->btnRemoteRefresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));

    ui->btnUpload->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
    ui->btnDownload->setIcon(style()->standardIcon(QStyle::SP_ArrowLeft));

    ui->btnRefreshDevices->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    ui->btnAddDevice->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    ui->btnCancelTransfer->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
    ui->btnClose->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
}

void FileManagerDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (m_service) {
        m_service->broadcastDiscovery();
    }
    refreshDeviceList();
    populateLocalDirectory(m_currentLocalPath);
}

void FileManagerDialog::refreshDeviceList()
{
    ui->comboDevices->blockSignals(true);
    ui->comboDevices->clear();

    if (!m_service) {
        ui->comboDevices->blockSignals(false);
        return;
    }

    QList<DeviceInfo> devs = m_service->getConnectedDevices();
    for (const auto& dev : devs) {
        QString label = QString("%1 (%2)").arg(dev.name).arg(dev.ip);
        ui->comboDevices->addItem(label, dev.ip);
    }

    ui->comboDevices->blockSignals(false);

    if (ui->comboDevices->count() > 0) {
        ui->lblConnectionStatus->setText(QString("Connected (%1 device%2)")
            .arg(ui->comboDevices->count())
            .arg(ui->comboDevices->count() > 1 ? "s" : ""));
        ui->btnUpload->setEnabled(true);
        ui->btnDownload->setEnabled(true);
        onDeviceSelected(0);
    } else {
        ui->lblConnectionStatus->setText("No remote devices connected");
        ui->btnUpload->setEnabled(false);
        ui->btnDownload->setEnabled(false);
        ui->tableRemote->setRowCount(0);
        ui->lblRemoteSummary->setText("0 files and 0 directories");
    }
}

void FileManagerDialog::selectDevice(const QString& host)
{
    for (int i = 0; i < ui->comboDevices->count(); ++i) {
        if (ui->comboDevices->itemData(i).toString() == host) {
            ui->comboDevices->setCurrentIndex(i);
            onDeviceSelected(i);
            break;
        }
    }
}

void FileManagerDialog::onDeviceSelected(int index)
{
    if (index < 0 || index >= ui->comboDevices->count()) {
        m_selectedDeviceIp.clear();
        return;
    }

    m_selectedDeviceIp = ui->comboDevices->itemData(index).toString();
    if (m_service && !m_selectedDeviceIp.isEmpty()) {
        ui->lblTransferStatus->setText("Fetching remote directory...");
        m_service->listRemoteDirectory(m_selectedDeviceIp, "~");
    }
}

void FileManagerDialog::onRefreshDevicesClicked()
{
    ui->lblTransferStatus->setText("Discovering remote devices...");
    if (m_service) {
        m_service->broadcastDiscovery();
    }
    refreshDeviceList();
    QTimer::singleShot(350, this, [this]() {
        refreshDeviceList();
        ui->lblTransferStatus->setText("Ready");
    });
}

void FileManagerDialog::onAddDeviceClicked()
{
    bool ok = false;
    QString ip = QInputDialog::getText(this, tr("Direct Connect"),
                                       tr("Enter Remote IP Address (e.g. 192.168.8.194):"),
                                       QLineEdit::Normal, QString(), &ok);
    if (ok && !ip.trimmed().isEmpty()) {
        QString clean = ip.trimmed();
        if (clean.contains(':')) {
            clean = clean.section(':', 0, 0);
        }
        if (m_service) {
            ui->lblTransferStatus->setText(QString("Connecting to %1...").arg(clean));
            m_service->connectToDevice(clean, 24802);
            QTimer::singleShot(500, this, [this]() {
                refreshDeviceList();
                ui->lblTransferStatus->setText("Ready");
            });
        }
    }
}

void FileManagerDialog::onDeviceConnected(const QString&, const QString&)
{
    refreshDeviceList();
}

void FileManagerDialog::onDeviceDisconnected(const QString&, const QString&)
{
    refreshDeviceList();
}

// ======================== LOCAL SITE ========================

void FileManagerDialog::populateLocalDirectory(const QString& path)
{
    QDir dir(path);
    if (!dir.exists()) {
        return;
    }

    m_currentLocalPath = dir.canonicalPath().isEmpty() ? dir.absolutePath() : dir.canonicalPath();
    ui->editLocalPath->setText(m_currentLocalPath);

    QFileInfoList list = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::DirsFirst | QDir::Name);

    ui->tableLocal->setSortingEnabled(false);
    ui->tableLocal->setRowCount(0);

    int fileCount = 0;
    int dirCount = 0;
    qint64 totalBytes = 0;

    for (const QFileInfo& fi : list) {
        int row = ui->tableLocal->rowCount();
        ui->tableLocal->insertRow(row);

        // Column 0: Filename + Icon
        QTableWidgetItem* nameItem = new QTableWidgetItem(fi.fileName());
        nameItem->setIcon(m_iconProvider.icon(fi));
        nameItem->setData(Qt::UserRole, fi.absoluteFilePath());
        nameItem->setData(Qt::UserRole + 1, fi.isDir());
        ui->tableLocal->setItem(row, 0, nameItem);

        // Column 1: Filesize
        QString sizeStr = fi.isDir() ? "" : formatFileSize(fi.size());
        QTableWidgetItem* sizeItem = new QTableWidgetItem(sizeStr);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ui->tableLocal->setItem(row, 1, sizeItem);

        // Column 2: Filetype
        QTableWidgetItem* typeItem = new QTableWidgetItem(formatFileType(fi.fileName(), fi.isDir()));
        ui->tableLocal->setItem(row, 2, typeItem);

        // Column 3: Last modified
        QTableWidgetItem* dateItem = new QTableWidgetItem(fi.lastModified().toString("yyyy-MM-dd hh:mm"));
        ui->tableLocal->setItem(row, 3, dateItem);

        if (fi.isDir()) {
            dirCount++;
        } else {
            fileCount++;
            totalBytes += fi.size();
        }
    }

    ui->tableLocal->setSortingEnabled(true);
    ui->lblLocalSummary->setText(QString("%1 files and %2 directories. Total size: %3")
        .arg(fileCount)
        .arg(dirCount)
        .arg(formatFileSize(totalBytes)));
}

void FileManagerDialog::onLocalNavigate()
{
    QString entered = ui->editLocalPath->text().trimmed();
    if (QDir(entered).exists()) {
        populateLocalDirectory(entered);
    } else {
        ui->editLocalPath->setText(m_currentLocalPath);
    }
}

void FileManagerDialog::onLocalUpClicked()
{
    QDir dir(m_currentLocalPath);
    if (dir.cdUp()) {
        populateLocalDirectory(dir.absolutePath());
    }
}

void FileManagerDialog::onLocalHomeClicked()
{
    populateLocalDirectory(QDir::homePath());
}

void FileManagerDialog::onLocalRefreshClicked()
{
    populateLocalDirectory(m_currentLocalPath);
}

void FileManagerDialog::onLocalItemDoubleClicked(int row, int)
{
    QTableWidgetItem* item = ui->tableLocal->item(row, 0);
    if (!item) return;

    bool isDir = item->data(Qt::UserRole + 1).toBool();
    QString path = item->data(Qt::UserRole).toString();

    if (isDir) {
        populateLocalDirectory(path);
    } else {
        // Double clicking a file triggers upload
        onUploadClicked();
    }
}

// ======================== REMOTE SITE ========================

void FileManagerDialog::populateRemoteDirectory(const QJsonArray& items)
{
    ui->tableRemote->setSortingEnabled(false);
    ui->tableRemote->setRowCount(0);

    int fileCount = 0;
    int dirCount = 0;
    qint64 totalBytes = 0;

    for (const QJsonValue& val : items) {
        QJsonObject obj = val.toObject();
        QString name = obj.value("name").toString();
        bool isDir = obj.value("is_dir").toBool();
        qint64 size = obj.value("size").toInteger();
        qint64 mtime = obj.value("mtime").toInteger();

        int row = ui->tableRemote->rowCount();
        ui->tableRemote->insertRow(row);

        // Column 0: Filename + Icon
        QTableWidgetItem* nameItem = new QTableWidgetItem(name);
        nameItem->setIcon(style()->standardIcon(isDir ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
        nameItem->setData(Qt::UserRole, name);
        nameItem->setData(Qt::UserRole + 1, isDir);
        ui->tableRemote->setItem(row, 0, nameItem);

        // Column 1: Filesize
        QString sizeStr = isDir ? "" : formatFileSize(size);
        QTableWidgetItem* sizeItem = new QTableWidgetItem(sizeStr);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ui->tableRemote->setItem(row, 1, sizeItem);

        // Column 2: Filetype
        QTableWidgetItem* typeItem = new QTableWidgetItem(formatFileType(name, isDir));
        ui->tableRemote->setItem(row, 2, typeItem);

        // Column 3: Last modified
        QDateTime dt = QDateTime::fromSecsSinceEpoch(mtime);
        QTableWidgetItem* dateItem = new QTableWidgetItem(dt.toString("yyyy-MM-dd hh:mm"));
        ui->tableRemote->setItem(row, 3, dateItem);

        if (isDir) {
            dirCount++;
        } else {
            fileCount++;
            totalBytes += size;
        }
    }

    ui->tableRemote->setSortingEnabled(true);
    ui->lblRemoteSummary->setText(QString("%1 files and %2 directories. Total size: %3")
        .arg(fileCount)
        .arg(dirCount)
        .arg(formatFileSize(totalBytes)));
    ui->lblTransferStatus->setText("Ready");
    ui->tableRemote->viewport()->update();
    QCoreApplication::processEvents();
}

void FileManagerDialog::onDirectoryListed(const QString& host, const QString& path, const QString& parent, const QJsonArray& items)
{
    QString cleanHost = host;
    if (cleanHost.startsWith("::ffff:")) cleanHost = cleanHost.mid(7);
    QString cleanSelected = m_selectedDeviceIp;
    if (cleanSelected.startsWith("::ffff:")) cleanSelected = cleanSelected.mid(7);

    if (!cleanSelected.isEmpty() && !cleanHost.isEmpty() && cleanHost != cleanSelected) {
        bool match = false;
        if (m_service) {
            for (const auto& dev : m_service->getConnectedDevices()) {
                if ((dev.ip == cleanHost || dev.name == host) && (dev.ip == cleanSelected || dev.name == cleanSelected)) {
                    match = true;
                    break;
                }
            }
        }
        if (!match) return;
    }

    m_currentRemotePath = path;
    m_remoteParentPath = parent;
    ui->editRemotePath->setText(m_currentRemotePath);
    populateRemoteDirectory(items);
}

void FileManagerDialog::onRemoteNavigate()
{
    QString entered = ui->editRemotePath->text().trimmed();
    if (m_service && !m_selectedDeviceIp.isEmpty() && !entered.isEmpty()) {
        ui->lblTransferStatus->setText("Loading remote folder...");
        m_service->listRemoteDirectory(m_selectedDeviceIp, entered);
    }
}

void FileManagerDialog::onRemoteUpClicked()
{
    if (m_service && !m_selectedDeviceIp.isEmpty() && !m_remoteParentPath.isEmpty()) {
        ui->lblTransferStatus->setText("Loading remote folder...");
        m_service->listRemoteDirectory(m_selectedDeviceIp, m_remoteParentPath);
    }
}

void FileManagerDialog::onRemoteHomeClicked()
{
    if (m_service && !m_selectedDeviceIp.isEmpty()) {
        ui->lblTransferStatus->setText("Loading remote folder...");
        m_service->listRemoteDirectory(m_selectedDeviceIp, "~");
    }
}

void FileManagerDialog::onRemoteRefreshClicked()
{
    if (m_service && !m_selectedDeviceIp.isEmpty()) {
        m_service->listRemoteDirectory(m_selectedDeviceIp, m_currentRemotePath);
    }
}

void FileManagerDialog::onRemoteItemDoubleClicked(int row, int)
{
    QTableWidgetItem* item = ui->tableRemote->item(row, 0);
    if (!item) return;

    bool isDir = item->data(Qt::UserRole + 1).toBool();
    QString name = item->data(Qt::UserRole).toString();

    if (isDir) {
        QString nextPath = m_currentRemotePath.endsWith("/") ? (m_currentRemotePath + name) : (m_currentRemotePath + "/" + name);
        if (m_service && !m_selectedDeviceIp.isEmpty()) {
            m_service->listRemoteDirectory(m_selectedDeviceIp, nextPath);
        }
    } else {
        // Double clicking a remote file triggers download
        onDownloadClicked();
    }
}

// ======================== TRANSFERS ========================

void FileManagerDialog::onUploadClicked()
{
    int row = ui->tableLocal->currentRow();
    if (row < 0) {
        QMessageBox::information(this, "Upload", "Please select a file or folder from the Local site to upload.");
        return;
    }

    QTableWidgetItem* item = ui->tableLocal->item(row, 0);
    if (!item) return;

    QString localPath = item->data(Qt::UserRole).toString();

    if (m_selectedDeviceIp.isEmpty()) {
        QMessageBox::warning(this, "Upload", "No remote device selected.");
        return;
    }

    ui->btnUpload->setEnabled(false);
    ui->btnDownload->setEnabled(false);
    ui->btnCancelTransfer->setEnabled(true);
    ui->progressBar->setValue(0);
    ui->lblTransferStatus->setText(QString("Starting upload: %1...").arg(item->text()));

    m_service->uploadItem(m_selectedDeviceIp, localPath, m_currentRemotePath);
}

void FileManagerDialog::onDownloadClicked()
{
    int row = ui->tableRemote->currentRow();
    if (row < 0) {
        QMessageBox::information(this, "Download", "Please select a file or folder from the Remote site to download.");
        return;
    }

    QTableWidgetItem* item = ui->tableRemote->item(row, 0);
    if (!item) return;

    QString remoteName = item->data(Qt::UserRole).toString();
    QString remoteFullPath = m_currentRemotePath.endsWith("/") ? (m_currentRemotePath + remoteName) : (m_currentRemotePath + "/" + remoteName);

    if (m_selectedDeviceIp.isEmpty()) {
        QMessageBox::warning(this, "Download", "No remote device selected.");
        return;
    }

    ui->btnUpload->setEnabled(false);
    ui->btnDownload->setEnabled(false);
    ui->btnCancelTransfer->setEnabled(true);
    ui->progressBar->setValue(0);
    ui->lblTransferStatus->setText(QString("Starting download: %1...").arg(remoteName));

    m_service->downloadItem(m_selectedDeviceIp, remoteFullPath, m_currentLocalPath);
}

void FileManagerDialog::onCancelTransferClicked()
{
    if (m_service) {
        m_service->cancelTransfer();
    }
    ui->btnUpload->setEnabled(true);
    ui->btnDownload->setEnabled(true);
    ui->btnCancelTransfer->setEnabled(false);
    ui->progressBar->setValue(0);
    ui->lblTransferStatus->setText("Transfer cancelled.");
}

void FileManagerDialog::onTransferProgress(const QString& filename, qint64 bytesDone, qint64 totalBytes, double speedMBs)
{
    int percent = totalBytes > 0 ? static_cast<int>((bytesDone * 100) / totalBytes) : 0;
    ui->progressBar->setValue(percent);

    ui->lblTransferStatus->setText(QString("Transferring %1: %2 / %3 (%4 MB/s)")
        .arg(filename)
        .arg(formatFileSize(bytesDone))
        .arg(formatFileSize(totalBytes))
        .arg(QString::number(speedMBs, 'f', 1)));
}

void FileManagerDialog::onTransferFinished(const QString& filename, bool success, const QString& message)
{
    ui->btnUpload->setEnabled(true);
    ui->btnDownload->setEnabled(true);
    ui->btnCancelTransfer->setEnabled(false);
    ui->progressBar->setValue(success ? 100 : 0);

    if (success) {
        ui->lblTransferStatus->setText(QString("Successfully transferred %1").arg(filename));
        // Refresh both views so the new file immediately appears
        populateLocalDirectory(m_currentLocalPath);
        onRemoteRefreshClicked();
    } else {
        ui->lblTransferStatus->setText(QString("Transfer failed: %1").arg(message));
    }
}

void FileManagerDialog::onStatusMessage(const QString& message)
{
    ui->lblTransferStatus->setText(message);
}

QString FileManagerDialog::formatFileSize(qint64 bytes) const
{
    if (bytes < 1024) {
        return QString("%1 B").arg(bytes);
    } else if (bytes < 1024 * 1024) {
        return QString("%1 KB").arg(QString::number(bytes / 1024.0, 'f', 1));
    } else if (bytes < 1024 * 1024 * 1024) {
        return QString("%1 MB").arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 1));
    } else {
        return QString("%1 GB").arg(QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2));
    }
}

QString FileManagerDialog::formatFileType(const QString& name, bool isDir) const
{
    if (isDir) {
        return "File folder";
    }
    int dotIndex = name.lastIndexOf('.');
    if (dotIndex > 0) {
        return QString("%1 file").arg(name.mid(dotIndex + 1).toUpper());
    }
    return "File";
}

} // namespace inputleap
