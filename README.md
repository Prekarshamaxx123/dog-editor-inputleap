# 🐶 dog-editor-inputleap

**dog-editor-inputleap** is an advanced open-source KVM utility that allows seamless sharing of mouse, keyboard, clipboard, and **Drag-and-Drop File Transfer** across multiple computers over local networks.

---

## ✨ Features

- **🖱️ Mouse & Keyboard Sharing**: Seamlessly move your mouse cursor across screens of different computers as if they were a single desktop.
- **📁 Drag-and-Drop File Sharing**: Drag files directly from one computer screen and drop them onto another computer screen to instantly transfer them.
- **📋 Universal Clipboard Sharing**: Copy and paste text, rich text (HTML), images, and files across connected machines.
- **🌐 Cross-Platform**: Works across **Linux**, **Windows**, and **macOS**.
- **🔒 Secure**: Built-in OpenSSL TLS/SSL encryption support to keep your keystrokes and data private.
- **⚡ High Performance**: Low-latency event streaming with optimized 32KB file streaming chunks.

---

## 🚀 Quick Start

### 1. Launch the Full Graphical Interface (GUI)
Run `dog-editor-inputleap` directly to open the desktop GUI:
```bash
./dog-editor-inputleap
```
Or if installed to `~/.local/bin`:
```bash
dog-editor-inputleap
```

---

## 📦 Download Pre-built Binaries (GitHub Releases)

Ready-to-run binaries are available for all major operating systems on [GitHub Releases](https://github.com/Prekarshamaxx123/dog-editor-inputleap/releases):

| OS | Package | Description |
|---|---|---|
| **Linux (x86_64)** | `dog-editor-inputleap-v3.0.3-full-gui-linux-x86_64.tar.gz` | Full Qt GUI + Server + Client |
| **Linux (Server)** | `dog-editor-inputleap-v3.0.3-server-linux-x86_64.tar.gz` | Standalone Linux Server Daemon |
| **Linux (Client)** | `dog-editor-inputleap-v3.0.3-client-linux-x86_64.tar.gz` | Standalone Linux Client Daemon |
| **Windows (x64)** | `dog-editor-inputleap-v3.0.3-full-windows-x64.zip` | Full Windows Bundle (Server & Client) |
| **Windows (Server)** | `dog-editor-inputleap-v3.0.3-server-windows-x64.zip` | Standalone Windows Server Executable |
| **Windows (Client)** | `dog-editor-inputleap-v3.0.3-client-windows-x64.zip` | Standalone Windows Client Executable |
| **macOS (Apple Silicon)** | `dog-editor-inputleap-v3.0.3-macos-AppleSilicon.dmg` | DMG Installer (M1/M2/M3/M4) |
| **macOS (Intel)** | `dog-editor-inputleap-v3.0.3-macos-Intel-x86_64.dmg` | DMG Installer (Intel x86_64) |
| **macOS (Server/Client)** | `dog-editor-inputleap-v3.0.3-server-macos-*.zip` | Standalone CLI Server / Client |

---

## 🛠️ Build from Source

## 📖 Usage

### Running the Server (Primary Computer)

The machine with the physical mouse and keyboard attached acts as the server.

1. Create or edit `example.conf`:
   ```conf
   section: screens
       server-pc:
       client-pc:
   end

   section: links
       server-pc:
           right = client-pc
       client-pc:
           left  = server-pc
   end
   ```

2. Start the server:
   ```bash
   ./dog-editor-inputleap server -f -c example.conf --address 0.0.0.0:24800
   ```

*(Optional: Specify custom drop directory with `--drop-dir /path/to/folder`)*

### Running the Client (Secondary Computer)

On the second computer, connect to the server's IP address:

```bash
./dog-editor-inputleap client -f <server-ip>:24800
```

---

## 📂 File Drag & Drop Sharing

### How it works:
1. **Drag across screens**: Click and drag any file on the server computer, move the mouse cursor across the screen boundary to the client computer, and release the mouse button.
2. **Clipboard transfer**: Copy a file in your file manager (`Ctrl+C`), move the mouse cursor over to the other screen, and the file automatically transfers to the recipient's drop directory (`~/Downloads` or custom `--drop-dir`).

---

## 🧪 Testing

To run the full unit test suite (133 tests) including drag & drop validation:

```bash
./dog-editor-inputleap test
```

To run only the Drag & Drop unit tests:

```bash
./dog-editor-inputleap test --gtest_filter=DragDropTests.*
```

To run network integration tests:

```bash
./dog-editor-inputleap integtests
```

---

## 📄 License

Distributed under the GNU General Public License v2 (GPL-2.0). See [LICENSE](LICENSE) for details.
