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

#include "inputleap/DragInformation.h"
#include "base/Log.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace inputleap {

DragInformation::DragInformation() :
    m_filename(),
    m_filesize(0)
{
}

void DragInformation::parseDragInfo(DragFileList& dragFileList, std::uint32_t fileNum,
                                    std::string data)
{
    dragFileList.clear();
    size_t startPos = 0;

    for (std::uint32_t index = 0; index < fileNum; ++index) {
        if (startPos >= data.size()) {
            break;
        }

        size_t comma1 = data.find(',', startPos);
        if (comma1 == std::string::npos) {
            break;
        }

        std::string fullPath = data.substr(startPos, comma1 - startPos);
        size_t lastSlash = fullPath.find_last_of("/\\");
        std::string filename = (lastSlash != std::string::npos) ? fullPath.substr(lastSlash + 1) : fullPath;

        DragInformation di;
        di.setFilename(filename);

        size_t comma2 = data.find(',', comma1 + 1);
        if (comma2 != std::string::npos) {
            std::string filesizeStr = data.substr(comma1 + 1, comma2 - comma1 - 1);
            di.setFilesize(stringToNum(filesizeStr));
            startPos = comma2 + 1;
        } else {
            std::string filesizeStr = data.substr(comma1 + 1);
            di.setFilesize(stringToNum(filesizeStr));
            startPos = data.size();
        }

        dragFileList.push_back(di);
    }

    LOG_DEBUG("drag info received, total drag file number: %zi",
        dragFileList.size());

    for (size_t i = 0; i < dragFileList.size(); ++i) {
        LOG_DEBUG("dragging file %zi name: %s",
            i + 1,
            dragFileList.at(i).getFilename().c_str());
    }
}

std::string DragInformation::getDragFileExtension(std::string filename)
{
    size_t findResult = std::string::npos;
    findResult = filename.find_last_of(".", filename.size());
    if (findResult != std::string::npos) {
        return filename.substr(findResult + 1, filename.size() - findResult - 1);
    }
    else {
        return "";
    }
}

int
DragInformation::setupDragInfo(DragFileList& fileList, std::string& output)
{
    int size = static_cast<int>(fileList.size());
    for (int i = 0; i < size; ++i) {
        output.append(fileList.at(i).getFilename());
        output.append(",");
        std::string filesize = getFileSize(fileList.at(i).getFilename());
        output.append(filesize);
        output.append(",");
    }
    return size;
}

bool DragInformation::isFileValid(std::string filename)
{
    bool result = false;
    std::fstream file(filename.c_str(), std::ios::in|std::ios::binary);

    if (file.is_open()) {
        result = true;
    }

    file. close();

    return result;
}

size_t DragInformation::stringToNum(std::string& str)
{
    std::istringstream iss(str.c_str());
    size_t size;
    iss >> size;
    return size;
}

std::string DragInformation::getFileSize(std::string& filename)
{
    std::fstream file(filename.c_str(), std::ios::in|std::ios::binary);

    if (!file.is_open()) {
      throw std::runtime_error("failed to get file size");
    }

    // check file size
    file.seekg (0, std::ios::end);
    size_t size = static_cast<size_t>(file.tellg());

    std::stringstream ss;
    ss << size;

    file. close();

    return ss.str();
}

} // namespace inputleap
