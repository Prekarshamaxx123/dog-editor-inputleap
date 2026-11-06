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

### 1. Build from Source

To compile **dog-editor-inputleap** on Linux:

```bash
git clone https://github.com/Prekarshamaxx123/dog-editor-inputleap.git
cd dog-editor-inputleap
./build.sh
```

The compiled binaries will be generated in `build/bin/`:
- `input-leaps` (Server)
- `input-leapc` (Client)
- `dog-editor-inputleap` (Unified Tool Launcher)
- `unittests` & `integtests` (Testing suites)

---

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
