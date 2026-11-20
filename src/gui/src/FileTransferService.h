#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QNetworkInterface>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QMap>

namespace inputleap {

struct DeviceInfo {
    QString name;
    QString ip;
    quint16 port = 24802;
    QTcpSocket* socket = nullptr;
};

class FileTransferService : public QObject {
    Q_OBJECT

public:
    explicit FileTransferService(QObject* parent = nullptr);
    virtual ~FileTransferService();

    bool startServer(quint16 port = 24802);
    void stopServer();

    void connectToDevice(const QString& host, quint16 port = 24802, const QString& expectedName = QString());
    void disconnectDevice(const QString& host);
    void broadcastDiscovery();

    QList<DeviceInfo> getConnectedDevices() const;

    void listRemoteDirectory(const QString& host, const QString& path);
    void uploadItem(const QString& host, const QString& localPath, const QString& remoteDestDir);
    void downloadItem(const QString& host, const QString& remotePath, const QString& localDestDir);
    void createRemoteDirectory(const QString& host, const QString& newPath);
    void deleteRemoteItem(const QString& host, const QString& path);
    void cancelTransfer();

    void setLocalDeviceName(const QString& name) { m_localName = name; }
    QString localDeviceName() const { return m_localName; }

signals:
    void deviceConnected(const QString& name, const QString& ip);
    void deviceDisconnected(const QString& name, const QString& ip);
    void directoryListed(const QString& host, const QString& path, const QString& parent, const QJsonArray& items);
    void transferProgress(const QString& filename, qint64 bytesDone, qint64 totalBytes, double speedMBs);
    void transferFinished(const QString& filename, bool success, const QString& message);
    void statusMessage(const QString& message);

private slots:
    void onNewConnection();
    void onSocketReadyRead(QTcpSocket* socket);
    void onSocketDisconnected(QTcpSocket* socket);
    void onSocketError(QAbstractSocket::SocketError error, QTcpSocket* socket);
    void onUdpReadyRead();
    void sendNextChunk();

private:
    void processPacket(QTcpSocket* socket, const QByteArray& packet);
    void sendPacket(QTcpSocket* socket, const QJsonObject& json, const QByteArray& extraBinary = QByteArray());

    // Request handlers
    void handleRegister(QTcpSocket* socket, const QJsonObject& req);
    void handleListDir(QTcpSocket* socket, const QJsonObject& req);
    void handlePutFileReq(QTcpSocket* socket, const QJsonObject& req);
    void handleGetFileReq(QTcpSocket* socket, const QJsonObject& req);
    void handleFileChunk(QTcpSocket* socket, const QJsonObject& req, const QByteArray& binData);
    void handleMkdir(QTcpSocket* socket, const QJsonObject& req);
    void handleDelete(QTcpSocket* socket, const QJsonObject& req);

    QTcpSocket* getSocketForHost(const QString& host) const;

private:
    QTcpServer* m_server = nullptr;
    QUdpSocket* m_udpSocket = nullptr;
    quint16 m_port = 24802;
    quint16 m_udpPort = 24803;
    QString m_localName;

    // Connected peers: key is IP or socket pointer
    QMap<QTcpSocket*, DeviceInfo> m_devices;
    QMap<QTcpSocket*, QByteArray> m_incomingBuffers;
    QMap<QTcpSocket*, quint32> m_expectedSizes;

    // Ongoing transfer state
    bool m_isTransferring = false;
    bool m_isSending = false; // true = upload, false = download receive
    QTcpSocket* m_activeTransferSocket = nullptr;
    QString m_activeFilename;
    qint64 m_totalBytes = 0;
    qint64 m_bytesTransferred = 0;
    QFile m_activeFile;
    QString m_activeTempArchive;
    QString m_activeDestFolder;
    bool m_activeIsFolder = false;
    QElapsedTimer m_speedTimer;
    qint64 m_lastBytesForSpeed = 0;
    qint64 m_lastSpeedTime = 0;
    double m_currentSpeed = 0.0;
};

} // namespace inputleap
