#include "FileTransferService.h"

#include <QDataStream>
#include <QHostAddress>
#include <QHostInfo>
#include <QDateTime>
#include <QTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QDebug>

namespace inputleap {

static const quint32 kMaxChunkSize = 128 * 1024; // 128 KB chunks

FileTransferService::FileTransferService(QObject* parent)
    : QObject(parent)
{
    m_localName = QHostInfo::localHostName();
}

FileTransferService::~FileTransferService()
{
    stopServer();
}

bool FileTransferService::startServer(quint16 port)
{
    if (m_server && m_server->isListening()) {
        if (m_port == port) {
            broadcastDiscovery();
            return true;
        }
        stopServer();
    }

    m_port = port;
    m_server = new QTcpServer(this);
    m_server->setProxy(QNetworkProxy::NoProxy);
    connect(m_server, &QTcpServer::newConnection, this, &FileTransferService::onNewConnection);

    if (!m_server->listen(QHostAddress::Any, m_port)) {
        qWarning() << "FileTransferService: Failed to listen on port" << m_port << ":" << m_server->errorString();
        return false;
    }

    if (!m_udpSocket) {
        m_udpSocket = new QUdpSocket(this);
        m_udpSocket->setProxy(QNetworkProxy::NoProxy);
        m_udpSocket->bind(QHostAddress::AnyIPv4, m_udpPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        connect(m_udpSocket, &QUdpSocket::readyRead, this, &FileTransferService::onUdpReadyRead);
    }
    broadcastDiscovery();

    qDebug() << "FileTransferService: Listening on port" << m_port;
    return true;
}

void FileTransferService::cleanupSocket(QTcpSocket* socket)
{
    if (!socket) return;

    socket->disconnect();

    QString devName;
    QString devIp;
    bool hadDevice = false;

    if (m_devices.contains(socket)) {
        DeviceInfo dev = m_devices.take(socket);
        devName = dev.name;
        devIp = dev.ip;
        hadDevice = true;
    }
    m_incomingBuffers.remove(socket);
    m_expectedSizes.remove(socket);

    if (m_activeTransferSocket == socket) {
        cancelTransfer();
    }

    if (socket->state() != QAbstractSocket::UnconnectedState) {
        socket->abort();
    }
    socket->deleteLater();

    if (hadDevice && !devName.isEmpty()) {
        emit deviceDisconnected(devName, devIp);
    }
}

void FileTransferService::stopServer()
{
    cancelTransfer();

    QList<QPointer<QTcpSocket>> sockets = m_devices.keys();
    m_devices.clear();
    m_incomingBuffers.clear();
    m_expectedSizes.clear();

    for (QPointer<QTcpSocket> s : sockets) {
        if (s) {
            s->disconnect();
            s->abort();
            s->deleteLater();
        }
    }

    if (m_udpSocket) {
        m_udpSocket->disconnect();
        m_udpSocket->close();
        m_udpSocket->deleteLater();
        m_udpSocket = nullptr;
    }

    if (m_server) {
        m_server->disconnect();
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

void FileTransferService::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        if (!socket) continue;

        socket->setProxy(QNetworkProxy::NoProxy);
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

        QString ip = socket->peerAddress().toString();
        if (ip.startsWith("::ffff:")) {
            ip = ip.mid(7);
        }

        // Check if we already have an established connection to this IP
        bool alreadyConnected = false;
        QTcpSocket* pendingSocket = nullptr;
        for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
            if (it.value().ip == ip && it.value().socket) {
                auto state = it.value().socket->state();
                if (state == QAbstractSocket::ConnectedState) {
                    alreadyConnected = true;
                    break;
                } else if (state == QAbstractSocket::ConnectingState || state == QAbstractSocket::HostLookupState) {
                    pendingSocket = it.value().socket.data();
                }
            }
        }

        if (alreadyConnected) {
            qDebug() << "FileTransferService: Rejecting duplicate incoming connection from" << ip;
            socket->abort();
            socket->deleteLater();
            continue;
        }

        if (pendingSocket) {
            qDebug() << "FileTransferService: Incoming connection from" << ip << "supersedes pending outgoing socket";
            cleanupSocket(pendingSocket);
        }

        DeviceInfo dev;
        dev.ip = ip;
        dev.port = socket->peerPort();
        dev.socket = socket;
        dev.name = ip;

        m_devices[socket] = dev;
        m_incomingBuffers[socket] = QByteArray();

        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            onSocketReadyRead(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            onSocketDisconnected(socket);
        });
        connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError err) {
            onSocketError(err, socket);
        });
        connect(socket, &QTcpSocket::bytesWritten, this, [this](qint64) {
            if (m_isTransferring && m_isSending) {
                sendNextChunk();
            }
        });

        // Send register message to announce our name
        QJsonObject reg;
        reg["cmd"] = "register";
        reg["name"] = m_localName;
        sendPacket(socket, reg);

        qDebug() << "FileTransferService: Incoming connection from" << ip;
    }
}

void FileTransferService::connectToDevice(const QString& host, quint16 port, const QString& expectedName)
{
    if (host.isEmpty()) {
        return;
    }

    QString cleanHost = host;
    if (cleanHost.startsWith("::ffff:")) {
        cleanHost = cleanHost.mid(7);
    }

    // Check if already connected or connecting to this host
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (it.value().ip == cleanHost && it.value().socket) {
            auto state = it.value().socket->state();
            if (state == QAbstractSocket::ConnectedState ||
                state == QAbstractSocket::ConnectingState ||
                state == QAbstractSocket::HostLookupState) {
                return;
            }
        }
    }

    QTcpSocket* socket = new QTcpSocket(this);
    socket->setProxy(QNetworkProxy::NoProxy);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

    DeviceInfo dev;
    dev.ip = cleanHost;
    dev.port = port;
    dev.name = expectedName.isEmpty() ? cleanHost : expectedName;
    dev.socket = socket;

    m_devices[socket] = dev;
    m_incomingBuffers[socket] = QByteArray();

    connect(socket, &QTcpSocket::connected, this, [this, socket, cleanHost]() {
        if (!socket) return;
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        QJsonObject reg;
        reg["cmd"] = "register";
        reg["name"] = m_localName;
        sendPacket(socket, reg);
        qDebug() << "FileTransferService: Connected to" << cleanHost;
    });

    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        onSocketReadyRead(socket);
    });

    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        onSocketDisconnected(socket);
    });

    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError err) {
        onSocketError(err, socket);
    });

    connect(socket, &QTcpSocket::bytesWritten, this, [this](qint64) {
        if (m_isTransferring && m_isSending) {
            sendNextChunk();
        }
    });

    socket->connectToHost(cleanHost, port);
}

void FileTransferService::disconnectDevice(const QString& host)
{
    QTcpSocket* s = getSocketForHost(host);
    if (s) {
        cleanupSocket(s);
    }
}

QList<DeviceInfo> FileTransferService::getConnectedDevices() const
{
    QList<DeviceInfo> list;
    QSet<QString> seenIps;
    for (const auto& dev : m_devices) {
        if (dev.socket && dev.socket->state() == QAbstractSocket::ConnectedState) {
            if (!seenIps.contains(dev.ip)) {
                seenIps.insert(dev.ip);
                list.append(dev);
            }
        }
    }
    return list;
}

QTcpSocket* FileTransferService::getSocketForHost(const QString& host) const
{
    QString clean = host;
    if (clean.startsWith("::ffff:")) {
        clean = clean.mid(7);
    }

    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if ((it.value().ip == clean || it.value().name == host) &&
            it.value().socket && it.value().socket->state() == QAbstractSocket::ConnectedState) {
            return it.value().socket.data();
        }
    }

    // If host is empty and we have connected devices, return the first connected one
    if (host.isEmpty()) {
        for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
            if (it.value().socket && it.value().socket->state() == QAbstractSocket::ConnectedState) {
                return it.value().socket.data();
            }
        }
    }

    return nullptr;
}

void FileTransferService::onSocketDisconnected(QTcpSocket* socket)
{
    cleanupSocket(socket);
}

void FileTransferService::onSocketError(QAbstractSocket::SocketError error, QTcpSocket* socket)
{
    if (!socket) return;
    qWarning() << "FileTransferService socket error:" << error << socket->errorString();
    if (socket->state() != QAbstractSocket::ConnectedState) {
        cleanupSocket(socket);
    }
}

void FileTransferService::onSocketReadyRead(QTcpSocket* socket)
{
    if (!socket || !m_devices.contains(socket)) {
        return;
    }

    QByteArray& buffer = m_incomingBuffers[socket];
    buffer.append(socket->readAll());

    while (buffer.size() >= static_cast<int>(sizeof(quint32))) {
        QDataStream in(&buffer, QIODevice::ReadOnly);
        in.setVersion(QDataStream::Qt_6_0);

        quint32 packetSize = 0;
        in >> packetSize;

        if (packetSize > 32 * 1024 * 1024) {
            qWarning() << "FileTransferService: Oversized packet received:" << packetSize;
            cleanupSocket(socket);
            return;
        }

        quint32 totalPacketSize = sizeof(quint32) + packetSize;
        if (static_cast<quint32>(buffer.size()) < totalPacketSize) {
            // Wait for more data to complete the packet
            break;
        }

        // Read packet payload
        QByteArray jsonBytes;
        QByteArray extraBinary;
        in >> jsonBytes >> extraBinary;

        // Fast deallocation if exact packet size, otherwise remove
        if (buffer.size() == static_cast<int>(totalPacketSize)) {
            buffer.clear();
        } else {
            buffer.remove(0, totalPacketSize);
        }

        QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject obj = doc.object();
            QString cmd = obj.value("cmd").toString();

            if (cmd == "register") {
                handleRegister(socket, obj);
            } else if (cmd == "register_ack") {
                QString name = obj.value("name").toString();
                if (m_devices.contains(socket)) {
                    m_devices[socket].name = name;
                    QString devIp = m_devices[socket].ip;
                    for (auto it = m_devices.begin(); it != m_devices.end(); ) {
                        if (it.key() != socket && it.value().ip == devIp) {
                            QTcpSocket* dup = it.key().data();
                            it = m_devices.erase(it);
                            if (dup) {
                                dup->abort();
                                dup->deleteLater();
                            }
                        } else {
                            ++it;
                        }
                    }
                    emit deviceConnected(name, devIp);
                }
            } else if (cmd == "list_dir") {
                handleListDir(socket, obj);
            } else if (cmd == "list_dir_resp") {
                QString path = obj.value("path").toString();
                QString parent = obj.value("parent").toString();
                QJsonArray items = obj.value("items").toArray();
                QString host = m_devices.contains(socket) ? m_devices[socket].ip : "";
                emit directoryListed(host, path, parent, items);
            } else if (cmd == "put_file_req") {
                handlePutFileReq(socket, obj);
            } else if (cmd == "get_file_req") {
                handleGetFileReq(socket, obj);
            } else if (cmd == "file_chunk") {
                handleFileChunk(socket, obj, extraBinary);
            } else if (cmd == "put_file_ready") {
                // Remote is ready to receive chunks! Start sending
                if (m_isTransferring && m_isSending) {
                    sendNextChunk();
                }
            } else if (cmd == "transfer_done") {
                bool success = obj.value("success").toBool();
                QString msg = obj.value("msg").toString();
                emit transferFinished(m_activeFilename, success, msg);
                cancelTransfer();
            } else if (cmd == "transfer_cancel") {
                cancelTransfer();
                emit transferFinished(m_activeFilename, false, "Transfer was cancelled by remote peer");
            } else if (cmd == "mkdir") {
                handleMkdir(socket, obj);
            } else if (cmd == "mkdir_resp") {
                bool ok = obj.value("success").toBool();
                emit statusMessage(ok ? "Folder created successfully" : "Failed to create folder: " + obj.value("error").toString());
            } else if (cmd == "delete") {
                handleDelete(socket, obj);
            } else if (cmd == "delete_resp") {
                bool ok = obj.value("success").toBool();
                emit statusMessage(ok ? "Item deleted" : "Failed to delete item: " + obj.value("error").toString());
            }
        }
    }
}

void FileTransferService::sendPacket(QTcpSocket* socket, const QJsonObject& json, const QByteArray& extraBinary)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    QByteArray jsonBytes = QJsonDocument(json).toJson(QJsonDocument::Compact);

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);

    out << static_cast<quint32>(0); // Placeholder
    out << jsonBytes;
    out << extraBinary;

    out.device()->seek(0);
    out << static_cast<quint32>(block.size() - sizeof(quint32));

    socket->write(block);
}

void FileTransferService::handleRegister(QTcpSocket* socket, const QJsonObject& req)
{
    QString name = req.value("name").toString();
    if (m_devices.contains(socket)) {
        m_devices[socket].name = name;
        QString devIp = m_devices[socket].ip;
        for (auto it = m_devices.begin(); it != m_devices.end(); ) {
            if (it.key() != socket && it.value().ip == devIp) {
                QTcpSocket* dup = it.key().data();
                it = m_devices.erase(it);
                if (dup) {
                    dup->abort();
                    dup->deleteLater();
                }
            } else {
                ++it;
            }
        }
        emit deviceConnected(name, devIp);
    }

    QJsonObject resp;
    resp["cmd"] = "register_ack";
    resp["name"] = m_localName;
    sendPacket(socket, resp);
}

void FileTransferService::handleListDir(QTcpSocket* socket, const QJsonObject& req)
{
    QString reqPath = req.value("path").toString();
    if (reqPath.isEmpty() || reqPath == "~") {
        reqPath = QDir::homePath();
    }

    QDir dir(reqPath);
    QJsonObject resp;
    resp["cmd"] = "list_dir_resp";
    resp["path"] = dir.canonicalPath().isEmpty() ? dir.absolutePath() : dir.canonicalPath();
    QDir parentDir = dir;
    parentDir.cdUp();
    resp["parent"] = parentDir.absolutePath();

    QJsonArray items;
    if (dir.exists()) {
        QFileInfoList list = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::DirsFirst | QDir::Name);
        for (const QFileInfo& fi : list) {
            QJsonObject item;
            item["name"] = fi.fileName();
            item["is_dir"] = fi.isDir();
            item["size"] = fi.isDir() ? 0 : fi.size();
            item["mtime"] = fi.lastModified().toSecsSinceEpoch();
            items.append(item);
        }
        resp["success"] = true;
    } else {
        resp["success"] = false;
        resp["error"] = "Directory does not exist";
    }

    resp["items"] = items;
    sendPacket(socket, resp);
}

void FileTransferService::listRemoteDirectory(const QString& host, const QString& path)
{
    QTcpSocket* socket = getSocketForHost(host);
    if (!socket) {
        emit statusMessage("Cannot list directory: Device not connected");
        return;
    }

    QJsonObject req;
    req["cmd"] = "list_dir";
    req["path"] = path;
    sendPacket(socket, req);
}

void FileTransferService::uploadItem(const QString& host, const QString& localPath, const QString& remoteDestDir)
{
    QTcpSocket* socket = getSocketForHost(host);
    if (!socket) {
        emit statusMessage("Cannot upload: Device not connected");
        return;
    }

    if (m_isTransferring) {
        emit statusMessage("Another transfer is currently in progress");
        return;
    }

    QFileInfo fi(localPath);
    if (!fi.exists()) {
        emit statusMessage("Local file does not exist");
        return;
    }

    m_activeIsFolder = fi.isDir();
    m_activeFilename = fi.fileName();
    m_activeTransferSocket = socket;
    m_isSending = true;
    m_isTransferring = true;
    m_bytesTransferred = 0;

    if (m_activeIsFolder) {
        emit statusMessage("Archiving folder for transfer...");
        QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/transfers";
        QDir().mkpath(cacheDir);
        m_activeTempArchive = cacheDir + "/" + m_activeFilename + "_" + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".tar";
        QFile::remove(m_activeTempArchive);

        QProcess* proc = new QProcess(this);
        m_activeProcess = proc;
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, socket, remoteDestDir](int exitCode, QProcess::ExitStatus exitStatus) {
            if (m_activeProcess) {
                m_activeProcess->deleteLater();
                m_activeProcess = nullptr;
            }
            if (exitCode != 0 || exitStatus != QProcess::NormalExit || !QFile::exists(m_activeTempArchive)) {
                emit statusMessage("Failed to package folder for transfer");
                cancelTransfer();
                return;
            }
            startSendingFile(socket, m_activeTempArchive, remoteDestDir);
        });

        proc->start("tar", QStringList() << "-cf" << m_activeTempArchive << "-C" << fi.absolutePath() << fi.fileName());
        return;
    }

    startSendingFile(socket, localPath, remoteDestDir);
}

void FileTransferService::startSendingFile(QTcpSocket* socket, const QString& filePath, const QString& remoteDestDir)
{
    if (!m_isTransferring || !socket || socket->state() != QAbstractSocket::ConnectedState) {
        cancelTransfer();
        return;
    }

    m_activeFile.setFileName(filePath);
    if (!m_activeFile.open(QIODevice::ReadOnly)) {
        emit statusMessage("Failed to open local file for reading");
        cancelTransfer();
        return;
    }

    m_totalBytes = m_activeFile.size();
    m_speedTimer.start();
    m_lastBytesForSpeed = 0;
    m_lastSpeedTime = 0;

    QJsonObject req;
    req["cmd"] = "put_file_req";
    req["dest_dir"] = remoteDestDir;
    req["filename"] = m_activeFilename;
    req["size"] = m_totalBytes;
    req["is_folder"] = m_activeIsFolder;
    sendPacket(socket, req);
}

void FileTransferService::handlePutFileReq(QTcpSocket* socket, const QJsonObject& req)
{
    if (m_isTransferring) {
        QJsonObject rej;
        rej["cmd"] = "transfer_cancel";
        rej["error"] = "Receiver is busy with another transfer";
        sendPacket(socket, rej);
        return;
    }

    QString destDir = req.value("dest_dir").toString();
    if (destDir.isEmpty()) {
        destDir = QDir::homePath();
    }
    m_activeFilename = req.value("filename").toString();
    m_totalBytes = req.value("size").toInteger();
    m_activeIsFolder = req.value("is_folder").toBool();
    m_bytesTransferred = 0;
    m_isSending = false;
    m_isTransferring = true;
    m_activeTransferSocket = socket;
    m_activeDestFolder = destDir;

    QDir().mkpath(destDir);

    if (m_activeIsFolder) {
        QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/transfers";
        QDir().mkpath(cacheDir);
        m_activeTempArchive = cacheDir + "/" + m_activeFilename + "_" + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".tar";
        QFile::remove(m_activeTempArchive);
        m_activeFile.setFileName(m_activeTempArchive);
    } else {
        QString finalPath = destDir + "/" + m_activeFilename;
        m_activeFile.setFileName(finalPath);
    }

    if (!m_activeFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        cancelTransfer();
        QJsonObject rej;
        rej["cmd"] = "transfer_cancel";
        rej["error"] = "Cannot open destination file for writing";
        sendPacket(socket, rej);
        return;
    }

    m_speedTimer.start();
    m_lastBytesForSpeed = 0;
    m_lastSpeedTime = 0;

    QJsonObject ready;
    ready["cmd"] = "put_file_ready";
    ready["dest_path"] = m_activeFile.fileName();
    sendPacket(socket, ready);
}

void FileTransferService::downloadItem(const QString& host, const QString& remotePath, const QString& localDestDir)
{
    QTcpSocket* socket = getSocketForHost(host);
    if (!socket) {
        emit statusMessage("Cannot download: Device not connected");
        return;
    }

    if (m_isTransferring) {
        emit statusMessage("Another transfer is currently in progress");
        return;
    }

    QJsonObject req;
    req["cmd"] = "get_file_req";
    req["src_path"] = remotePath;
    req["dest_local_dir"] = localDestDir;
    sendPacket(socket, req);
}

void FileTransferService::handleGetFileReq(QTcpSocket* socket, const QJsonObject& req)
{
    QString srcPath = req.value("src_path").toString();
    QString destLocalDir = req.value("dest_local_dir").toString();

    QFileInfo fi(srcPath);
    if (!fi.exists()) {
        QJsonObject rej;
        rej["cmd"] = "transfer_cancel";
        rej["error"] = "Remote source file does not exist";
        sendPacket(socket, rej);
        return;
    }

    // Upload from here to requesting peer
    uploadItem(m_devices[socket].ip, srcPath, destLocalDir);
}

void FileTransferService::sendNextChunk()
{
    if (!m_isTransferring || !m_isSending || !m_activeFile.isOpen() || !m_activeTransferSocket) {
        return;
    }

    if (m_activeTransferSocket->state() != QAbstractSocket::ConnectedState) {
        cancelTransfer();
        return;
    }

    // Flow control: keep socket write buffer below 256 KB, burst up to 4 chunks then yield to event loop
    int chunksSent = 0;
    while (m_activeTransferSocket && m_activeTransferSocket->state() == QAbstractSocket::ConnectedState &&
           m_activeTransferSocket->bytesToWrite() < 256 * 1024 && !m_activeFile.atEnd()) {
        QByteArray chunk = m_activeFile.read(kMaxChunkSize);
        if (chunk.isEmpty()) {
            break;
        }

        m_bytesTransferred += chunk.size();
        bool isLast = m_activeFile.atEnd();

        QJsonObject chunkObj;
        chunkObj["cmd"] = "file_chunk";
        chunkObj["is_last"] = isLast;
        sendPacket(m_activeTransferSocket, chunkObj, chunk);

        // Speed calculation
        qint64 elapsedMs = m_speedTimer.elapsed();
        if (elapsedMs - m_lastSpeedTime >= 200) {
            double seconds = (elapsedMs - m_lastSpeedTime) / 1000.0;
            double bytes = m_bytesTransferred - m_lastBytesForSpeed;
            m_currentSpeed = (bytes / (1024.0 * 1024.0)) / seconds;
            m_lastSpeedTime = elapsedMs;
            m_lastBytesForSpeed = m_bytesTransferred;
            emit transferProgress(m_activeFilename, m_bytesTransferred, m_totalBytes, m_currentSpeed);
        }

        if (isLast) {
            m_activeFile.close();
            if (!m_activeTempArchive.isEmpty()) {
                QFile::remove(m_activeTempArchive);
                m_activeTempArchive.clear();
            }
            emit transferProgress(m_activeFilename, m_totalBytes, m_totalBytes, m_currentSpeed);
            break;
        }

        chunksSent++;
        if (chunksSent >= 4) {
            // Yield to Qt event loop to ensure UI stays responsive
            QTimer::singleShot(0, this, &FileTransferService::sendNextChunk);
            break;
        }
    }
}

void FileTransferService::handleFileChunk(QTcpSocket* socket, const QJsonObject& req, const QByteArray& binData)
{
    if (!m_isTransferring || m_isSending || !m_activeFile.isOpen()) {
        return;
    }

    m_activeFile.write(binData);
    m_bytesTransferred += binData.size();
    bool isLast = req.value("is_last").toBool();

    qint64 elapsedMs = m_speedTimer.elapsed();
    if (elapsedMs - m_lastSpeedTime >= 200 || isLast) {
        double seconds = qMax(1.0, static_cast<double>(elapsedMs)) / 1000.0;
        m_currentSpeed = (m_bytesTransferred / (1024.0 * 1024.0)) / seconds;
        m_lastSpeedTime = elapsedMs;
        emit transferProgress(m_activeFilename, m_bytesTransferred, m_totalBytes, m_currentSpeed);
    }

    if (isLast) {
        m_activeFile.close();

        if (m_activeIsFolder && QFile::exists(m_activeTempArchive)) {
            emit statusMessage("Extracting folder archive...");
            QProcess* proc = new QProcess(this);
            m_activeProcess = proc;
            connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, [this, socket](int exitCode, QProcess::ExitStatus exitStatus) {
                if (m_activeProcess) {
                    m_activeProcess->deleteLater();
                    m_activeProcess = nullptr;
                }
                if (QFile::exists(m_activeTempArchive)) {
                    QFile::remove(m_activeTempArchive);
                    m_activeTempArchive.clear();
                }

                if (exitCode != 0 || exitStatus != QProcess::NormalExit) {
                    emit statusMessage("Failed to extract folder archive");
                    emit transferFinished(m_activeFilename, false, "Extraction failed");
                    cancelTransfer();
                    return;
                }

                QJsonObject done;
                done["cmd"] = "transfer_done";
                done["success"] = true;
                done["msg"] = "Transfer completed successfully";
                sendPacket(socket, done);

                emit transferFinished(m_activeFilename, true, "Transfer completed successfully");
                cancelTransfer();
            });

            proc->start("tar", QStringList() << "-xf" << m_activeTempArchive << "-C" << m_activeDestFolder);
            return;
        }

        QJsonObject done;
        done["cmd"] = "transfer_done";
        done["success"] = true;
        done["msg"] = "Transfer completed successfully";
        sendPacket(socket, done);

        emit transferFinished(m_activeFilename, true, "Transfer completed successfully");
        cancelTransfer();
    }
}

void FileTransferService::cancelTransfer()
{
    if (m_activeProcess) {
        m_activeProcess->kill();
        m_activeProcess->deleteLater();
        m_activeProcess = nullptr;
    }

    if (m_activeTransferSocket && m_isTransferring) {
        QJsonObject cancelObj;
        cancelObj["cmd"] = "transfer_cancel";
        sendPacket(m_activeTransferSocket, cancelObj);
    }

    if (m_activeFile.isOpen()) {
        m_activeFile.close();
    }
    if (!m_activeTempArchive.isEmpty()) {
        QFile::remove(m_activeTempArchive);
        m_activeTempArchive.clear();
    }

    m_isTransferring = false;
    m_isSending = false;
    m_activeTransferSocket = nullptr;
    m_activeFilename.clear();
    m_totalBytes = 0;
    m_bytesTransferred = 0;
}

void FileTransferService::createRemoteDirectory(const QString& host, const QString& newPath)
{
    QTcpSocket* socket = getSocketForHost(host);
    if (!socket) return;
    QJsonObject req;
    req["cmd"] = "mkdir";
    req["path"] = newPath;
    sendPacket(socket, req);
}

void FileTransferService::handleMkdir(QTcpSocket* socket, const QJsonObject& req)
{
    QString path = req.value("path").toString();
    bool ok = QDir().mkpath(path);
    QJsonObject resp;
    resp["cmd"] = "mkdir_resp";
    resp["path"] = path;
    resp["success"] = ok;
    if (!ok) resp["error"] = "Permission denied or invalid path";
    sendPacket(socket, resp);
}

void FileTransferService::deleteRemoteItem(const QString& host, const QString& path)
{
    QTcpSocket* socket = getSocketForHost(host);
    if (!socket) return;
    QJsonObject req;
    req["cmd"] = "delete";
    req["path"] = path;
    sendPacket(socket, req);
}

void FileTransferService::handleDelete(QTcpSocket* socket, const QJsonObject& req)
{
    QString path = req.value("path").toString();
    QFileInfo fi(path);
    bool ok = false;
    if (fi.isDir()) {
        ok = QDir(path).removeRecursively();
    } else if (fi.exists()) {
        ok = QFile::remove(path);
    }
    QJsonObject resp;
    resp["cmd"] = "delete_resp";
    resp["path"] = path;
    resp["success"] = ok;
    if (!ok) resp["error"] = "Cannot delete item";
    sendPacket(socket, resp);
}

void FileTransferService::broadcastDiscovery()
{
    if (!m_udpSocket) {
        return;
    }

    QJsonObject req;
    req["cmd"] = "discover";
    req["name"] = m_localName;
    req["tcpPort"] = m_port;
    QByteArray data = QJsonDocument(req).toJson(QJsonDocument::Compact);

    // Broadcast on 255.255.255.255
    m_udpSocket->writeDatagram(data, QHostAddress::Broadcast, m_udpPort);

    // Also broadcast on subnet broadcast addresses
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto& iface : interfaces) {
        if (!iface.flags().testFlag(QNetworkInterface::IsUp) ||
            iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const auto& entry : iface.addressEntries()) {
            QHostAddress bcast = entry.broadcast();
            if (!bcast.isNull() && bcast != QHostAddress::Broadcast) {
                m_udpSocket->writeDatagram(data, bcast, m_udpPort);
            }
        }
    }
}

void FileTransferService::onUdpReadyRead()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        qint64 dSize = m_udpSocket->pendingDatagramSize();
        if (dSize <= 0) {
            break;
        }
        QByteArray datagram;
        datagram.resize(dSize);
        QHostAddress sender;
        quint16 senderPort = 0;
        m_udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        QJsonDocument doc = QJsonDocument::fromJson(datagram);
        if (!doc.isObject()) {
            continue;
        }

        QJsonObject obj = doc.object();
        QString cmd = obj.value("cmd").toString();
        QString peerName = obj.value("name").toString();
        quint16 peerTcpPort = static_cast<quint16>(obj.value("tcpPort").toInt(24802));

        QString senderIp = sender.toString();
        if (senderIp.startsWith("::ffff:")) {
            senderIp = senderIp.mid(7);
        }

        // Avoid connecting to ourselves
        if (peerName == m_localName) {
            bool isLocal = false;
            const auto addresses = QNetworkInterface::allAddresses();
            for (const auto& addr : addresses) {
                QString a = addr.toString();
                if (a.startsWith("::ffff:")) a = a.mid(7);
                if (a == senderIp) {
                    isLocal = true;
                    break;
                }
            }
            if (isLocal) {
                continue;
            }
        }

        if (cmd == "discover") {
            connectToDevice(senderIp, peerTcpPort, peerName);

            // Send unicast response back to sender
            QJsonObject ack;
            ack["cmd"] = "discover_ack";
            ack["name"] = m_localName;
            ack["tcpPort"] = m_port;
            QByteArray ackBytes = QJsonDocument(ack).toJson(QJsonDocument::Compact);
            m_udpSocket->writeDatagram(ackBytes, sender, senderPort);
        } else if (cmd == "discover_ack") {
            connectToDevice(senderIp, peerTcpPort, peerName);
        }
    }
}

} // namespace inputleap
