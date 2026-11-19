/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2013-2016 Symless Ltd.
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

#include "inputleap/StreamChunker.h"

#include "inputleap/FileChunk.h"
#include "inputleap/ClipboardChunk.h"
#include "inputleap/protocol_types.h"
#include "base/EventTypes.h"
#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/EventTypes.h"
#include "base/Log.h"
#include "base/String.h"

#include <fstream>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include "base/Time.h"
#include "base/Stopwatch.h"
#include "io/filesystem.h"
#include "io/IStream.h"

namespace inputleap {

static const size_t g_chunkSize = 64 * 1024; // 64kb
static const int kMaxQueuedChunks = 16;
static std::atomic<int> s_queuedChunks{0};
static std::mutex s_chunkMutex;
static std::condition_variable s_chunkCv;

bool StreamChunker::s_isChunkingFile = false;
bool StreamChunker::s_interruptFile = false;

void StreamChunker::onChunkDispatched()
{
    int current = s_queuedChunks.fetch_sub(1);
    if (current <= kMaxQueuedChunks) {
        s_chunkCv.notify_one();
    }
}

void StreamChunker::sendFile(const char* filename, IEventQueue* events,
                             const EventTarget* event_target,
                             inputleap::IStream* stream)
{
    s_isChunkingFile = true;
    s_interruptFile = false;
    s_queuedChunks.store(0);

    std::fstream file;
    open_utf8_path(file, filename, std::ios::in | std::ios::binary);

    if (!file.is_open()) {
        s_isChunkingFile = false;
        throw std::runtime_error("failed to open file");
    }

    // check file size
    file.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    LOG_INFO("starting stream of file \"%s\" (%zu bytes)", filename, size);

    // send first message (file size)
    auto size_message = FileChunk::start(size);
    events->add_event(EventType::FILE_CHUNK_SENDING, event_target,
                      create_event_data<FileChunk>(size_message));

    // send chunk messages with a fixed chunk size
    size_t sentLength = 0;
    size_t chunkSize = g_chunkSize;
    Stopwatch keepAliveWatch;
    keepAliveWatch.start();

    while (sentLength < size) {
        if (s_interruptFile) {
            s_interruptFile = false;
            LOG_INFO("file transmission interrupted");
            break;
        }

        // Network flow control: do not let TCP socket output buffer exceed 256 KB.
        // This guarantees heartbeats and mouse events travel with < 25ms latency, preventing timeouts!
        while (stream != nullptr && stream->getOutputSize() > 256 * 1024) {
            if (s_interruptFile) {
                break;
            }
            inputleap::this_thread_sleep(0.01f); // 10ms
        }

        // Flow control: throttle chunk creation if too many chunks are queued in EventQueue
        {
            std::unique_lock<std::mutex> lock(s_chunkMutex);
            s_chunkCv.wait(lock, [&]() {
                return s_interruptFile || s_queuedChunks.load() < kMaxQueuedChunks;
            });
        }

        if (s_interruptFile) {
            s_interruptFile = false;
            LOG_INFO("file transmission interrupted");
            break;
        }

        // Send keepalive at most once every 2 seconds
        if (keepAliveWatch.getTime() >= 2.0) {
            events->add_event(EventType::FILE_KEEPALIVE, event_target);
            keepAliveWatch.reset();
        }

        if (sentLength + chunkSize > size) {
            chunkSize = size - sentLength;
        }

        std::vector<char> chunkData(chunkSize);
        file.read(chunkData.data(), chunkSize);
        std::streamsize bytesRead = file.gcount();
        if (bytesRead <= 0) {
            break;
        }

        std::uint8_t* data = reinterpret_cast<std::uint8_t*>(chunkData.data());
        FileChunk fileChunk = FileChunk::data(data, static_cast<size_t>(bytesRead));

        s_queuedChunks.fetch_add(1);
        events->add_event(EventType::FILE_CHUNK_SENDING, event_target,
                          create_event_data<FileChunk>(fileChunk));

        sentLength += static_cast<size_t>(bytesRead);

        // Cooperative yield so mouse/keyboard threads have priority
        inputleap::this_thread_sleep(0.0001f);
    }

    // Wait for TCP socket output buffer to drain before sending end message
    while (stream != nullptr && stream->getOutputSize() > 0) {
        if (s_interruptFile) {
            break;
        }
        inputleap::this_thread_sleep(0.01f);
    }

    // Wait for remaining in-flight chunks to drain before sending end message
    {
        std::unique_lock<std::mutex> lock(s_chunkMutex);
        s_chunkCv.wait_for(lock, std::chrono::seconds(5), [&]() {
            return s_interruptFile || s_queuedChunks.load() <= 0;
        });
    }

    // send last message
    FileChunk end = FileChunk::end();
    events->add_event(EventType::FILE_CHUNK_SENDING, event_target,
                      create_event_data<FileChunk>(end));

    file.close();
    s_isChunkingFile = false;
    LOG_INFO("completed stream of file \"%s\" (%zu bytes sent)", filename, sentLength);
}

void StreamChunker::sendClipboard(std::string& data, std::size_t size, ClipboardID id,
                                  std::uint32_t sequence, IEventQueue* events,
                                  const EventTarget* event_target)
{
    // send first message (data size)
    ClipboardChunk size_message = ClipboardChunk::start(id, sequence, size);

    events->add_event(EventType::CLIPBOARD_SENDING, event_target,
                      create_event_data<ClipboardChunk>(size_message));

    // send clipboard chunk with a fixed size
    size_t sentLength = 0;
    size_t chunkSize = g_chunkSize;

    while (true) {
        events->add_event(EventType::FILE_KEEPALIVE, event_target);

        // make sure we don't read too much from the mock data.
        if (sentLength + chunkSize > size) {
            chunkSize = size - sentLength;
        }

        std::string chunk(data.substr(sentLength, chunkSize).c_str(), chunkSize);
        ClipboardChunk data_chunk = ClipboardChunk::data(id, sequence, chunk);

        events->add_event(EventType::CLIPBOARD_SENDING, event_target,
                          create_event_data<ClipboardChunk>(data_chunk));

        sentLength += chunkSize;
        if (sentLength == size) {
            break;
        }
    }

    // send last message
    ClipboardChunk end = ClipboardChunk::end(id, sequence);

    events->add_event(EventType::CLIPBOARD_SENDING, event_target,
                      create_event_data<ClipboardChunk>(end));

    LOG_DEBUG("sent clipboard size=%zd", sentLength);
}

void
StreamChunker::interruptFile()
{
    if (s_isChunkingFile) {
        s_interruptFile = true;
        s_chunkCv.notify_all();
        LOG_INFO("previous dragged file has become invalid");
    }
}

} // namespace inputleap
