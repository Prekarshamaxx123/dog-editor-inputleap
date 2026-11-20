#pragma once

#include <QDialog>
#include <QFileSystemModel>
#include <QTableWidgetItem>
#include <QTimer>
#include <QFileIconProvider>
#include "FileTransferService.h"

namespace Ui {
class FileManagerDialog;
}

namespace inputleap {

class FileManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit FileManagerDialog(FileTransferService* service, QWidget* parent = nullptr);
    virtual ~FileManagerDialog();

    void refreshDeviceList();
    void selectDevice(const QString& host);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void onDeviceSelected(int index);
    void onRefreshDevicesClicked();

    // Local site navigation
    void onLocalNavigate();
    void onLocalUpClicked();
    void onLocalHomeClicked();
    void onLocalRefreshClicked();
    void onLocalItemDoubleClicked(int row, int column);

    // Remote site navigation
    void onRemoteNavigate();
    void onRemoteUpClicked();
    void onRemoteHomeClicked();
    void onRemoteRefreshClicked();
    void onRemoteItemDoubleClicked(int row, int column);

    // Transfer actions
    void onUploadClicked();
    void onDownloadClicked();
    void onCancelTransferClicked();

    // Service callbacks
    void onDeviceConnected(const QString& name, const QString& ip);
    void onDeviceDisconnected(const QString& name, const QString& ip);
    void onDirectoryListed(const QString& host, const QString& path, const QString& parent, const QJsonArray& items);
    void onTransferProgress(const QString& filename, qint64 bytesDone, qint64 totalBytes, double speedMBs);
    void onTransferFinished(const QString& filename, bool success, const QString& message);
    void onStatusMessage(const QString& message);

private:
    void setupIcons();
    void populateLocalDirectory(const QString& path);
    void populateRemoteDirectory(const QJsonArray& items);
    QString formatFileSize(qint64 bytes) const;
    QString formatFileType(const QString& name, bool isDir) const;

private:
    Ui::FileManagerDialog* ui;
    FileTransferService* m_service;
    QFileIconProvider m_iconProvider;

    QString m_currentLocalPath;
    QString m_currentRemotePath;
    QString m_remoteParentPath;
    QString m_selectedDeviceIp;
};

} // namespace inputleap
