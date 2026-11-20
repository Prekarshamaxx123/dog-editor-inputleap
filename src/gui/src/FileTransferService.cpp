#include "FileTransferService.h"

#include <QDataStream>
#include <QHostAddress>
#include <QHostInfo>
#include <QDateTime>
#include <QTimer>
#include <QProcess>
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
            return true;
        }
        stopServer();
    }

    m_port = port;
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &FileTransferService::onNewConnection);

    if (!m_server->listen(QHostAddress::Any, m_port)) {
        qWarning() << "FileTransferService: Failed to listen on port" << m_port << ":" << m_server->errorString();
        return false;
    }

    qDebug() << "FileTransferService: Listening on port" << m_port;
    return true;
}

void FileTransferService::stopServer()
{
    cancelTransfer();

    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (it.value().socket) {
            it.value().socket->disconnect(this);
            it.value().socket->close();
            it.value().socket->deleteLater();
        }
    }
    m_devices.clear();
    m_incomingBuffers.clear();

    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

void FileTransferService::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        QString ip = socket->peerAddress().toString();
        if (ip.startsWith("::ffff:")) {
            ip = ip.mid(7);
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

    // Check if already connected to this host
    for (const auto& dev : m_devices) {
        if (dev.ip == cleanHost && dev.socket && dev.socket->state() == QAbstractSocket::ConnectedState) {
            return;
        }
    }

    QTcpSocket* socket = new QTcpSocket(this);

    DeviceInfo dev;
    dev.ip = cleanHost;
    dev.port = port;
    dev.name = expectedName.isEmpty() ? cleanHost : expectedName;
    dev.socket = socket;

    m_devices[socket] = dev;
    m_incomingBuffers[socket] = QByteArray();

    connect(socket, &QTcpSocket::connected, this, [this, socket, cleanHost]() {
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
        s->disconnectFromHost();
    }
}

QList<DeviceInfo> FileTransferService::getConnectedDevices() const
{
    QList<DeviceInfo> list;
    for (const auto& dev : m_devices) {
        if (dev.socket && dev.socket->state() == QAbstractSocket::ConnectedState) {
            list.append(dev);
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
            return it.value().socket;
        }
    }

    // If host is empty and we have exactly 1 connected device, return it
    if (host.isEmpty() && m_devices.size() == 1) {
        auto it = m_devices.begin();
        if (it.value().socket && it.value().socket->state() == QAbstractSocket::ConnectedState) {
            return it.value().socket;
        }
    }

    return nullptr;
}

void FileTransferService::onSocketDisconnected(QTcpSocket* socket)
{
    if (m_devices.contains(socket)) {
        DeviceInfo dev = m_devices.value(socket);
        emit deviceDisconnected(dev.name, dev.ip);
        m_devices.remove(socket);
    }
    m_incomingBuffers.remove(socket);

    if (m_activeTransferSocket == socket) {
        cancelTransfer();
    }

    socket->deleteLater();
}

void FileTransferService::onSocketError(QAbstractSocket::SocketError, QTcpSocket* socket)
{
    qWarning() << "FileTransferService socket error:" << socket->errorString();
}

void FileTransferService::onSocketReadyRead(QTcpSocket* socket)
{
    QByteArray& buffer = m_incomingBuffers[socket];
    buffer.append(socket->readAll());

    while (buffer.size() >= static_cast<int>(sizeof(quint32))) {
        QDataStream in(&buffer, QIODevice::ReadOnly);
        in.setVersion(QDataStream::Qt_6_0);

        quint32 packetSize = 0;
        in >> packetSize;

        if (static_cast<quint32>(buffer.size() - sizeof(quint32)) < packetSize) {
            // Wait for more data to complete the packet
            break;
        }

        // Read packet payload
        QByteArray jsonBytes;
        QByteArray extraBinary;
        in >> jsonBytes >> extraBinary;

        // Remove processed packet bytes from buffer
        buffer.remove(0, sizeof(quint32) + packetSize);

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
                    emit deviceConnected(name, m_devices[socket].ip);
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
        emit deviceConnected(name, m_devices[socket].ip);
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

    QString fileToSend = localPath;

    if (m_activeIsFolder) {
        // Create temporary tar archive for fast streaming
        QString tempDir = QDir::tempPath() + "/dog_editor_transfer";
        QDir().mkpath(tempDir);
        m_activeTempArchive = tempDir + "/" + m_activeFilename + ".tar";
        QFile::remove(m_activeTempArchive);

        QString tarCmd = QString("tar -cf \"%1\" -C \"%2\" \"%3\"")
            .arg(m_activeTempArchive)
            .arg(fi.absolutePath())
            .arg(fi.fileName());
        int r = system(tarCmd.toUtf8().constData());
        if (r != 0 || !QFile::exists(m_activeTempArchive)) {
            emit statusMessage("Failed to package folder for transfer");
            cancelTransfer();
            return;
        }
        fileToSend = m_activeTempArchive;
    }

    m_activeFile.setFileName(fileToSend);
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
        QString tempDir = QDir::tempPath() + "/dog_editor_transfer";
        QDir().mkpath(tempDir);
        m_activeTempArchive = tempDir + "/" + m_activeFilename + ".tar";
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

    // Flow control: keep socket write buffer below 256 KB
    while (m_activeTransferSocket->bytesToWrite() < 256 * 1024 && !m_activeFile.atEnd()) {
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
            // Extract the tar archive directly into destination folder
            QString untarCmd = QString("tar -xf \"%1\" -C \"%2\"")
                .arg(m_activeTempArchive)
                .arg(m_activeDestFolder);
            system(untarCmd.toUtf8().constData());
            QFile::remove(m_activeTempArchive);
            m_activeTempArchive.clear();
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

} // namespace inputleap
