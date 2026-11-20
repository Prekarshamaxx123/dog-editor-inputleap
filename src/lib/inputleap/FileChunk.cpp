/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2015-2016 Symless Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "inputleap/FileChunk.h"

#include "inputleap/ProtocolUtil.h"
#include "inputleap/protocol_types.h"
#include "io/IStream.h"
#include "base/Stopwatch.h"
#include "base/String.h"
#include "base/Log.h"

#include <fstream>
#include <cstdio>
#include <thread>
#include "io/filesystem.h"

namespace inputleap {

static const std::uint16_t kIntervalThreshold = 1;
static std::string s_lastReceivedFile;

static void run_system_async(const std::string& cmd) {
    if (cmd.empty()) {
        return;
    }
    std::thread([cmd]() {
        int r = system(cmd.c_str());
        (void)r;
    }).detach();
}

std::string FileChunk::getLastReceivedFile()
{
    return s_lastReceivedFile;
}

FileChunk FileChunk::start(std::size_t size)
{
    FileChunk chunk;
    chunk.mark_ = kDataStart;
    chunk.data_ = std::to_string(size);
    return chunk;
}

FileChunk FileChunk::data(std::uint8_t* data, size_t dataSize)
{
    FileChunk chunk;
    chunk.mark_ = kDataChunk;
    chunk.data_ = std::string(reinterpret_cast<char*>(data), dataSize);
    return chunk;
}

FileChunk FileChunk::end()
{
    FileChunk chunk;
    chunk.mark_ = kDataEnd;
    return chunk;
}

int FileChunk::assemble(inputleap::IStream* stream, std::string& dataReceived, size_t& expectedSize)
{
    // parse
    std::uint8_t mark = 0;
    std::string content;
    static size_t receivedDataSize;
    static double elapsedTime;
    static Stopwatch stopwatch;

    if (!ProtocolUtil::readf(stream, kMsgDFileTransfer + 4, &mark, &content)) {
        return kError;
    }

    switch (mark) {
    case kDataStart:
        dataReceived.clear();
        expectedSize = inputleap::string::stringToSizeType(content);
        receivedDataSize = 0;
        elapsedTime = 0;
        stopwatch.reset();

        if (CLOG->getFilter() >= kDEBUG2) {
            LOG_DEBUG2("recv file size=%s", content.c_str());
            stopwatch.start();
        }
        return kStart;

    case kDataChunk:
        dataReceived.append(content);
        if (CLOG->getFilter() >= kDEBUG2) {
                LOG_DEBUG2("recv file chunk size=%zi", content.size());
                double interval = stopwatch.getTime();
                receivedDataSize += content.size();
                LOG_DEBUG2("recv file interval=%f s", interval);
                if (interval >= kIntervalThreshold) {
                    double averageSpeed = receivedDataSize / interval / 1000;
                    LOG_DEBUG2("recv file average speed=%f kb/s", averageSpeed);

                    receivedDataSize = 0;
                    elapsedTime += interval;
                    stopwatch.reset();
                }
            }
        return kNotFinish;

    case kDataEnd:
        if (expectedSize != dataReceived.size()) {
            LOG_ERR("corrupted clipboard data, expected size=%zd actual size=%zd", expectedSize, dataReceived.size());
            return kError;
        }

        if (CLOG->getFilter() >= kDEBUG2) {
            LOG_DEBUG2("file transfer finished");
            elapsedTime += stopwatch.getTime();
            double averageSpeed = expectedSize / elapsedTime / 1000;
            LOG_DEBUG2("file transfer finished: total time consumed=%f s", elapsedTime);
            LOG_DEBUG2("file transfer finished: total data received=%zi kb", expectedSize / 1000);
            LOG_DEBUG2("file transfer finished: total average speed=%f kb/s", averageSpeed);
        }
        return kFinish;
        default:
            break;
    }

    return kError;
}

static std::ofstream s_receivingFile;
static std::string s_currentPartPath;
static std::string s_currentFinalPath;
static size_t s_lastNotifiedPercent = 0;
static Stopwatch s_notificationWatch;

int FileChunk::assemble(inputleap::IStream* stream,
                        const std::string& dropTarget,
                        const std::string& filename,
                        size_t& receivedSize,
                        size_t& expectedSize)
{
    std::uint8_t mark = 0;
    std::string content;

    if (!ProtocolUtil::readf(stream, kMsgDFileTransfer + 4, &mark, &content)) {
        if (s_receivingFile.is_open()) {
            s_receivingFile.close();
            if (!s_currentPartPath.empty()) {
                std::remove(s_currentPartPath.c_str());
            }
        }
        return kError;
    }

    switch (mark) {
    case kDataStart: {
        if (s_receivingFile.is_open()) {
            s_receivingFile.close();
            if (!s_currentPartPath.empty()) {
                std::remove(s_currentPartPath.c_str());
            }
        }

        expectedSize = inputleap::string::stringToSizeType(content);
        receivedSize = 0;
        s_lastNotifiedPercent = 0;

        std::string targetDir = dropTarget;
        if (targetDir.empty()) {
            const char* home = getenv("HOME");
            targetDir = home ? (std::string(home) + "/.cache/dog-editor-inputleap/clipboard_cache") : "/tmp/dog-editor-inputleap_cache";
        }

        std::error_code ec;
        inputleap::fs::create_directories(inputleap::fs::u8path(targetDir), ec);

        std::string actualFilename = filename.empty() ? "received_file" : filename;
        size_t lastSlash = actualFilename.find_last_of("/\\");
        if (lastSlash != std::string::npos) {
            actualFilename = actualFilename.substr(lastSlash + 1);
        }

        // Clean up previous cached files to avoid eating disk space
        for (const auto& entry : inputleap::fs::directory_iterator(inputleap::fs::u8path(targetDir), ec)) {
            std::string entryFilename = entry.path().filename().u8string();
            if (entryFilename != actualFilename && entryFilename != (actualFilename + ".part")) {
                std::error_code ecRem;
                inputleap::fs::remove_all(entry.path(), ecRem);
            }
        }

        s_currentFinalPath = targetDir;
#ifdef SYSAPI_WIN32
        s_currentFinalPath += "\\";
#else
        s_currentFinalPath += "/";
#endif
        s_currentFinalPath += actualFilename;
        s_currentPartPath = s_currentFinalPath + ".part";

        open_utf8_path(s_receivingFile, s_currentPartPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!s_receivingFile.is_open()) {
            LOG_ERR("assemble: cannot create temporary file \"%s\"", s_currentPartPath.c_str());
            return kError;
        }

        LOG_INFO("Receiving incoming file \"%s\" (%zu bytes) into \"%s\"",
                 actualFilename.c_str(), expectedSize, s_currentFinalPath.c_str());

#if defined(WINAPI_XWINDOWS)
        std::string sizeStr;
        if (expectedSize > 1024ULL * 1024ULL * 1024ULL) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.2f GB", (double)expectedSize / (1024.0 * 1024.0 * 1024.0));
            sizeStr = buf;
        } else if (expectedSize > 1024ULL * 1024ULL) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f MB", (double)expectedSize / (1024.0 * 1024.0));
            sizeStr = buf;
        } else {
            sizeStr = std::to_string(expectedSize / 1024) + " KB";
        }

        std::string notifyCmd =
            "notify-send -i document-save 'InputLeap File Copy' 'Copying: " +
            actualFilename + " (" + sizeStr + ")...' >/dev/null 2>&1 &";
        run_system_async(notifyCmd);
        s_notificationWatch.reset();
        s_notificationWatch.start();
#endif
        return kStart;
    }

    case kDataChunk: {
        if (!s_receivingFile.is_open()) {
            LOG_ERR("assemble: received chunk but file is not open");
            return kError;
        }

        s_receivingFile.write(content.data(), content.size());
        receivedSize += content.size();

#if defined(WINAPI_XWINDOWS)
        if (expectedSize > 50ULL * 1024ULL * 1024ULL && s_notificationWatch.getTime() >= 5.0) {
            size_t percent = (receivedSize * 100) / (expectedSize > 0 ? expectedSize : 1);
            if (percent >= s_lastNotifiedPercent + 10) {
                s_lastNotifiedPercent = percent;
                s_notificationWatch.reset();
                char buf[200];
                if (expectedSize > 1024ULL * 1024ULL * 1024ULL) {
                    snprintf(buf, sizeof(buf), "notify-send -i document-save 'InputLeap Copying' 'Progress: %zu%% (%.2f / %.2f GB)' >/dev/null 2>&1 &",
                             percent, (double)receivedSize / (1024.0 * 1024.0 * 1024.0), (double)expectedSize / (1024.0 * 1024.0 * 1024.0));
                } else {
                    snprintf(buf, sizeof(buf), "notify-send -i document-save 'InputLeap Copying' 'Progress: %zu%% (%zu / %zu MB)' >/dev/null 2>&1 &",
                             percent, receivedSize / (1024 * 1024), expectedSize / (1024 * 1024));
                }
                run_system_async(buf);
            }
        }
#endif
        return kNotFinish;
    }

    case kDataEnd: {
        if (s_receivingFile.is_open()) {
            s_receivingFile.flush();
            s_receivingFile.close();
        }

        if (expectedSize != receivedSize) {
            LOG_ERR("corrupted file transfer: expected %zu bytes, received %zu bytes", expectedSize, receivedSize);
            std::remove(s_currentPartPath.c_str());
            return kError;
        }

        std::error_code ec;
        inputleap::fs::rename(inputleap::fs::u8path(s_currentPartPath), inputleap::fs::u8path(s_currentFinalPath), ec);
        if (ec) {
            std::remove(s_currentFinalPath.c_str());
            if (std::rename(s_currentPartPath.c_str(), s_currentFinalPath.c_str()) != 0) {
                LOG_ERR("assemble: rename failed from %s to %s", s_currentPartPath.c_str(), s_currentFinalPath.c_str());
            }
        }

        bool isFolder = (s_currentFinalPath.find("__inputleap_folder__.tar") != std::string::npos);
        if (isFolder) {
            std::string extractTarget = dropTarget;
            if (extractTarget.empty()) {
                const char* home = getenv("HOME");
                extractTarget = home ? (std::string(home) + "/.cache/dog-editor-inputleap/clipboard_cache") : "/tmp/dog-editor-inputleap_cache";
            }
            std::string extractCmd = "tar -xf '" + s_currentFinalPath + "' -C '" + extractTarget + "' >/dev/null 2>&1";
            int rExtract = system(extractCmd.c_str());
            (void)rExtract;
            std::remove(s_currentFinalPath.c_str());

            std::string base = inputleap::fs::u8path(s_currentFinalPath).filename().u8string();
            size_t pos = base.find("__inputleap_folder__.tar");
            if (pos != std::string::npos) {
                base = base.substr(0, pos);
            }
            s_currentFinalPath = extractTarget + "/" + base;
            LOG_INFO("Successfully extracted received folder into \"%s\"", s_currentFinalPath.c_str());
        } else {
            LOG_INFO("Successfully saved received file \"%s\" (%zu bytes)", s_currentFinalPath.c_str(), receivedSize);
        }

        s_lastReceivedFile = s_currentFinalPath;

#if defined(WINAPI_XWINDOWS)
        std::string setClipCmd =
            "((which dog-editor-setclip >/dev/null 2>&1 && dog-editor-setclip '" + s_currentFinalPath + "') || "
            "(test -x \"$HOME/.local/bin/dog-editor-setclip\" && \"$HOME/.local/bin/dog-editor-setclip\" '" + s_currentFinalPath + "') || "
            "(which python3 >/dev/null 2>&1 && python3 -c \"import sys, os; from PyQt5 import QtWidgets, QtCore; app = QtWidgets.QApplication(sys.argv[:1]); cb = app.clipboard(); md = QtCore.QMimeData(); path = os.path.abspath(sys.argv[1]); uri = QtCore.QUrl.fromLocalFile(path); md.setUrls([uri]); md.setText(f'file://{path}'); md.setData('x-special/gnome-copied-files', f'copy\\nfile://{path}\\n'.encode('utf-8')); cb.setMimeData(md); cb.dataChanged.connect(app.quit); app.exec_()\" '" + s_currentFinalPath + "') || "
            "(which xclip >/dev/null 2>&1 && printf '%s\\r\\n' 'file://" + s_currentFinalPath + "' | xclip -selection clipboard -t text/uri-list)) >/dev/null 2>&1";
        run_system_async(setClipCmd);

        std::string notifyTitle = isFolder ? "InputLeap Folder Copied" : "InputLeap File Copied";
        std::string itemType = isFolder ? "folder" : "file";
        std::string doneNotify =
            "notify-send -i dialog-ok '" + notifyTitle + "' '" +
            inputleap::fs::u8path(s_currentFinalPath).filename().u8string() +
            "\\nReady to paste into any folder!' >/dev/null 2>&1 &";
        run_system_async(doneNotify);
#endif

        return kFinish;
    }

    case kDataRequest: {
        return kRequest;
    }

    default:
        break;
    }

    if (s_receivingFile.is_open()) {
        s_receivingFile.close();
        if (!s_currentPartPath.empty()) {
            std::remove(s_currentPartPath.c_str());
        }
    }
    return kError;
}

} // namespace inputleap
