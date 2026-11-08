/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2014-2016 Symless Ltd.
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

#include "inputleap/DropHelper.h"

#include "base/Log.h"
#include "io/filesystem.h"

#include <fstream>

namespace inputleap {

void
DropHelper::writeToDir(const std::string& destination, DragFileList& fileList, std::string& data)
{
    LOG_DEBUG("dropping file, files=%zi target=%s, data_size=%zi", fileList.size(), destination.c_str(), data.size());

    if (!destination.empty() && data.size() > 0) {
        std::error_code ec;
        inputleap::fs::create_directories(inputleap::fs::u8path(destination), ec);

        std::string filename = "received_file";
        if (fileList.size() > 0 && !fileList.at(0).getFilename().empty()) {
            filename = fileList.at(0).getFilename();
        }

        std::fstream file;
        std::string dropTarget = destination;
#ifdef SYSAPI_WIN32
        dropTarget.append("\\");
#else
        dropTarget.append("/");
#endif
        dropTarget.append(filename);
        open_utf8_path(file, dropTarget, std::ios::out | std::ios::binary);
        if (!file.is_open()) {
            LOG_ERR("drop file failed: can not open %s", dropTarget.c_str());
            return;
        }

        file.write(data.c_str(), data.size());
        file.close();

        LOG_INFO("successfully saved received file \"%s\" in \"%s\"", filename.c_str(), destination.c_str());

#if defined(WINAPI_XWINDOWS)
        // Automatically register with X11 clipboard so Ctrl+V in Dolphin / Nautilus / Thunar pastes it!
        std::string localUri = "file://" + dropTarget;
        std::string setClipCmd =
            "((which dog-editor-setclip >/dev/null 2>&1 && dog-editor-setclip '" + dropTarget + "') || "
            "(which python3 >/dev/null 2>&1 && python3 -c \"import sys, os; from PyQt5 import QtWidgets, QtCore; app = QtWidgets.QApplication(sys.argv[:1]); cb = app.clipboard(); md = QtCore.QMimeData(); path = os.path.abspath(sys.argv[1]); uri = QtCore.QUrl.fromLocalFile(path); md.setUrls([uri]); md.setText(f'file://{path}'); md.setData('x-special/gnome-copied-files', f'copy\\nfile://{path}\\n'.encode('utf-8')); cb.setMimeData(md); cb.dataChanged.connect(app.quit); app.exec_()\" '" + dropTarget + "') || "
            "(which xclip >/dev/null 2>&1 && printf '%s\\r\\n' '" + localUri + "' | xclip -selection clipboard -t text/uri-list)) >/dev/null 2>&1 &";
        int res = system(setClipCmd.c_str());
        (void)res;
#endif

        fileList.clear();
    }
    else {
        LOG_ERR("drop file failed: destination is empty or no data received");
    }
}

} // namespace inputleap
