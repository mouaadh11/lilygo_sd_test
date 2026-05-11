#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>

#define SD_MISO 2
#define SD_MOSI 15
#define SD_SCLK 14
#define SD_CS   13

#ifndef WIFI_SSID
#error "WIFI_SSID is not defined. Add -D WIFI_SSID='\"your_wifi_name\"' to platformio.ini build_flags."
#endif

#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD is not defined. Add -D WIFI_PASSWORD='\"your_wifi_password\"' to platformio.ini build_flags."
#endif

WebServer server(80);

String currentPath = "/";
const char *HEADER_KEYS[] = {"Range"};

String htmlPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>ESP32 NAS</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">

  <style>
    body {
      font-family: Arial, sans-serif;
      margin: 0;
      background: #f2f2f2;
      color: #222;
    }

    header {
      background: #222;
      color: white;
      padding: 15px;
      font-size: 20px;
    }

    #path {
      padding: 10px 15px;
      background: white;
      border-bottom: 1px solid #ddd;
      font-size: 14px;
      overflow-x: auto;
      white-space: nowrap;
    }

    .toolbar {
      padding: 10px 15px;
      background: #fafafa;
      border-bottom: 1px solid #ddd;
    }

    button {
      padding: 8px 12px;
      border: none;
      background: #222;
      color: white;
      border-radius: 5px;
      cursor: pointer;
    }

    button:hover {
      background: #444;
    }

    #list {
      padding: 10px;
    }

    .item {
      display: flex;
      justify-content: space-between;
      align-items: center;
      background: white;
      margin-bottom: 6px;
      padding: 12px;
      border-radius: 6px;
      border: 1px solid #ddd;
      cursor: pointer;
    }

    .item:hover {
      background: #f8f8f8;
    }

    .name {
      font-weight: bold;
      word-break: break-all;
    }

    .size {
      color: #666;
      font-size: 13px;
      margin-left: 10px;
      white-space: nowrap;
    }

    .loading {
      padding: 20px;
      text-align: center;
      color: #666;
    }

    .status {
      padding: 10px 15px;
      color: #666;
      font-size: 14px;
    }

    .modal {
      display: none;
      position: fixed;
      inset: 0;
      z-index: 10;
      background: rgba(0, 0, 0, 0.78);
    }

    .modal.open {
      display: flex;
      align-items: center;
      justify-content: center;
    }

    .modal-panel {
      width: min(1000px, calc(100vw - 24px));
      max-height: calc(100vh - 24px);
      background: #111;
      color: white;
      border-radius: 6px;
      overflow: hidden;
      display: flex;
      flex-direction: column;
    }

    .modal-toolbar {
      display: flex;
      justify-content: space-between;
      gap: 10px;
      align-items: center;
      padding: 10px;
      background: #222;
    }

    .modal-title {
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      font-size: 14px;
    }

    .modal-actions {
      display: flex;
      gap: 8px;
      flex-shrink: 0;
    }

    .modal-actions a {
      display: inline-block;
      padding: 8px 12px;
      background: #fff;
      color: #111;
      border-radius: 5px;
      text-decoration: none;
      font-size: 14px;
    }

    .modal-media {
      min-height: 180px;
      max-height: calc(100vh - 90px);
      display: flex;
      align-items: center;
      justify-content: center;
      background: #050505;
    }

    .modal-media img,
    .modal-media video {
      max-width: 100%;
      max-height: calc(100vh - 90px);
    }
  </style>
</head>

<body>
  <header>ESP32 SD NAS</header>

  <div id="path">Path: /</div>

  <div class="toolbar">
    <button onclick="goBack()">⬅ Back</button>
    <button onclick="loadFolder(currentPath)">🔄 Refresh</button>
  </div>

  <div id="list" class="loading">Loading...</div>

  <div id="previewModal" class="modal" onclick="closePreview()">
    <div class="modal-panel" onclick="event.stopPropagation()">
      <div class="modal-toolbar">
        <div id="previewTitle" class="modal-title"></div>
        <div class="modal-actions">
          <a id="previewDownload" href="#">Download</a>
          <button onclick="closePreview()">Close</button>
        </div>
      </div>
      <div id="previewMedia" class="modal-media"></div>
    </div>
  </div>

  <script>
    let currentPath = "/";
    let listAbortController = null;

    function formatSize(bytes) {
      if (bytes < 1024) return bytes + " B";
      if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
      if (bytes < 1024 * 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + " MB";
      return (bytes / 1024 / 1024 / 1024).toFixed(1) + " GB";
    }

    function joinPath(base, name) {
      if (base === "/") return "/" + name;
      return base + "/" + name;
    }

    function lowerName(name) {
      return name.toLowerCase();
    }

    function isImage(name) {
      name = lowerName(name);
      return name.endsWith(".jpg") || name.endsWith(".jpeg") || name.endsWith(".png") || name.endsWith(".gif");
    }

    function isVideo(name) {
      return lowerName(name).endsWith(".mp4");
    }

    function isPreviewable(name) {
      return isImage(name) || isVideo(name);
    }

    function closePreview() {
      const modal = document.getElementById("previewModal");
      const media = document.getElementById("previewMedia");

      modal.classList.remove("open");
      media.innerHTML = "";
    }

    function openPreview(path, item) {
      const modal = document.getElementById("previewModal");
      const title = document.getElementById("previewTitle");
      const download = document.getElementById("previewDownload");
      const media = document.getElementById("previewMedia");
      const previewUrl = "/preview?path=" + encodeURIComponent(path);

      title.innerText = item.name;
      download.href = "/download?path=" + encodeURIComponent(path);
      media.innerHTML = "";

      if (isVideo(item.name)) {
        const video = document.createElement("video");
        video.controls = true;
        video.preload = "metadata";
        video.src = previewUrl;
        media.appendChild(video);
      } else {
        const image = document.createElement("img");
        image.alt = item.name;
        image.src = previewUrl;
        media.appendChild(image);
      }

      modal.classList.add("open");
    }

    function createItemElement(item) {
      const div = document.createElement("div");
      div.className = "item";

      const left = document.createElement("div");
      left.className = "name";
      left.innerText = item.type === "dir" ? "📁 " + item.name : "📄 " + item.name;

      const right = document.createElement("div");
      right.className = "size";
      right.innerText = item.type === "dir" ? "Folder" : formatSize(item.size);

      div.appendChild(left);
      div.appendChild(right);

      div.onclick = () => {
        const fullPath = joinPath(currentPath, item.name);

        if (item.type === "dir") {
          loadFolder(fullPath);
        } else if (isPreviewable(item.name)) {
          openPreview(fullPath, item);
        } else {
          window.location.href = "/download?path=" + encodeURIComponent(fullPath);
        }
      };

      return div;
    }

    function goBack() {
      if (currentPath === "/") return;

      let parts = currentPath.split("/").filter(p => p.length > 0);
      parts.pop();

      currentPath = "/" + parts.join("/");
      if (currentPath === "") currentPath = "/";

      loadFolder(currentPath);
    }

    async function loadFolder(path) {
      currentPath = path;

      if (listAbortController) {
        listAbortController.abort();
      }

      listAbortController = new AbortController();

      document.getElementById("path").innerText = "Path: " + currentPath;
      const list = document.getElementById("list");

      list.className = "";
      list.innerHTML = `
        <div id="status" class="status">Loading...</div>
        <div id="folders"></div>
        <div id="files"></div>
      `;

      const status = document.getElementById("status");
      const folders = document.getElementById("folders");
      const files = document.getElementById("files");

      let count = 0;
      let buffer = "";

      try {
        const response = await fetch("/api/list?path=" + encodeURIComponent(path), {
          signal: listAbortController.signal
        });

        if (!response.ok) {
          const data = await response.json().catch(() => ({ error: "Folder could not be loaded" }));
          status.innerText = "Error: " + data.error;
          return;
        }

        const reader = response.body.getReader();
        const decoder = new TextDecoder();

        while (true) {
          const result = await reader.read();

          if (result.done) {
            break;
          }

          buffer += decoder.decode(result.value, { stream: true });

          const lines = buffer.split("\n");
          buffer = lines.pop();

          for (const line of lines) {
            if (!line) continue;

            const item = JSON.parse(line);
            const target = item.type === "dir" ? folders : files;

            target.appendChild(createItemElement(item));
            count++;
            status.innerText = "Loading... " + count + " item" + (count === 1 ? "" : "s");
          }
        }

        buffer += decoder.decode();

        if (buffer.trim().length > 0) {
          const item = JSON.parse(buffer);
          const target = item.type === "dir" ? folders : files;

          target.appendChild(createItemElement(item));
          count++;
        }

        status.innerText = count === 0
          ? "Empty folder"
          : "Loaded " + count + " item" + (count === 1 ? "" : "s");

      } catch (e) {
        if (e.name !== "AbortError") {
          status.innerText = "Connection error";
        }
      }
    }

    loadFolder("/");
  </script>
</body>
</html>
)rawliteral";
}

String safePath(String path) {
  path.replace("\\", "/");

  if (!path.startsWith("/")) {
    path = "/" + path;
  }

  // Basic protection against path traversal
  if (path.indexOf("..") >= 0) {
    return "/";
  }

  return path;
}

String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "\\r");
  return s;
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleList() {
  String path = "/";

  if (server.hasArg("path")) {
    path = safePath(server.arg("path"));
  }

  Serial.print("Listing folder: ");
  Serial.println(path);

  File dir = SD.open(path);

  if (!dir) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"Folder not found\"}");
    return;
  }

  if (!dir.isDirectory()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Not a folder\"}");
    dir.close();
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/x-ndjson", "");

  File file = dir.openNextFile();

  while (file) {
    String name = String(file.name());

    // In some cases file.name() returns full path.
    // We only want the last part.
    int lastSlash = name.lastIndexOf('/');
    if (lastSlash >= 0) {
      name = name.substring(lastSlash + 1);
    }

    if (name.length() > 0) {
      String line = "{";
      line += "\"name\":\"" + jsonEscape(name) + "\",";
      line += "\"type\":\"" + String(file.isDirectory() ? "dir" : "file") + "\",";
      line += "\"size\":" + String((unsigned long long)file.size());
      line += "}\n";

      server.sendContent(line);
    }

    file.close();
    file = dir.openNextFile();
  }

  dir.close();
  server.sendContent("");
}

String getContentType(String filename) {
  filename.toLowerCase();

  if (filename.endsWith(".html")) return "text/html";
  if (filename.endsWith(".css")) return "text/css";
  if (filename.endsWith(".js")) return "application/javascript";
  if (filename.endsWith(".png")) return "image/png";
  if (filename.endsWith(".jpg")) return "image/jpeg";
  if (filename.endsWith(".jpeg")) return "image/jpeg";
  if (filename.endsWith(".gif")) return "image/gif";
  if (filename.endsWith(".mp4")) return "video/mp4";
  if (filename.endsWith(".txt")) return "text/plain";
  if (filename.endsWith(".pdf")) return "application/pdf";

  return "application/octet-stream";
}

bool parseRangeValue(const String &range, uint64_t fileSize, uint64_t &start, uint64_t &end) {
  if (!range.startsWith("bytes=")) {
    return false;
  }

  int dash = range.indexOf('-');
  if (dash < 6) {
    return false;
  }

  String startText = range.substring(6, dash);
  String endText = range.substring(dash + 1);

  if (startText.length() == 0) {
    return false;
  }

  start = strtoull(startText.c_str(), nullptr, 10);
  end = endText.length() > 0 ? strtoull(endText.c_str(), nullptr, 10) : fileSize - 1;

  if (start >= fileSize) {
    return false;
  }

  if (end >= fileSize) {
    end = fileSize - 1;
  }

  return start <= end;
}

void streamFileRange(File &file, const String &contentType, uint64_t start, uint64_t end, uint64_t fileSize) {
  uint64_t length = end - start + 1;

  server.sendHeader("Accept-Ranges", "bytes");
  server.sendHeader("Content-Range", "bytes " + String((unsigned long long)start) + "-" + String((unsigned long long)end) + "/" + String((unsigned long long)fileSize));
  server.setContentLength(length);
  server.send(206, contentType, "");

  file.seek((uint32_t)start);

  WiFiClient client = server.client();
  uint8_t buffer[1024];
  uint64_t remaining = length;

  while (remaining > 0 && client.connected()) {
    size_t toRead = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
    size_t bytesRead = file.read(buffer, toRead);

    if (bytesRead == 0) {
      break;
    }

    client.write(buffer, bytesRead);
    remaining -= bytesRead;
  }
}

void handlePreview() {
  if (!server.hasArg("path")) {
    server.send(400, "text/plain", "Missing path");
    return;
  }

  String path = safePath(server.arg("path"));

  Serial.print("Previewing: ");
  Serial.println(path);

  File file = SD.open(path, FILE_READ);

  if (!file) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  if (file.isDirectory()) {
    server.send(400, "text/plain", "Cannot preview folder");
    file.close();
    return;
  }

  String contentType = getContentType(path);
  uint64_t fileSize = file.size();

  server.sendHeader("Accept-Ranges", "bytes");

  if (server.hasHeader("Range")) {
    uint64_t start = 0;
    uint64_t end = fileSize > 0 ? fileSize - 1 : 0;

    if (fileSize > 0 && parseRangeValue(server.header("Range"), fileSize, start, end)) {
      streamFileRange(file, contentType, start, end, fileSize);
    } else {
      server.sendHeader("Content-Range", "bytes */" + String((unsigned long long)fileSize));
      server.send(416, "text/plain", "Requested range not satisfiable");
    }
  } else {
    server.streamFile(file, contentType);
  }

  file.close();
}

void handleDownload() {
  if (!server.hasArg("path")) {
    server.send(400, "text/plain", "Missing path");
    return;
  }

  String path = safePath(server.arg("path"));

  Serial.print("Downloading: ");
  Serial.println(path);

  File file = SD.open(path, FILE_READ);

  if (!file) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  if (file.isDirectory()) {
    server.send(400, "text/plain", "Cannot download folder");
    file.close();
    return;
  }

  String filename = path;
  int lastSlash = filename.lastIndexOf('/');
  if (lastSlash >= 0) {
    filename = filename.substring(lastSlash + 1);
  }

  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.streamFile(file, getContentType(filename));

  file.close();
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("=================================");
  Serial.println("ESP32 Custom SD NAS");
  Serial.println("=================================");

  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, SPI, 4000000)) {
    Serial.println("SD card mount failed!");
    return;
  }

  Serial.println("SD card mounted.");

  Serial.print("Card size: ");
  Serial.print(SD.cardSize() / (1024 * 1024));
  Serial.println(" MB");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected.");
  Serial.print("SSID: ");
  Serial.println(WIFI_SSID);
  Serial.print("Open: http://");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/list", HTTP_GET, handleList);
  server.on("/preview", HTTP_GET, handlePreview);
  server.on("/download", HTTP_GET, handleDownload);

  server.collectHeaders(HEADER_KEYS, 1);
  server.begin();

  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();
}
