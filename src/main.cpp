#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>

#define SD_MISO 2
#define SD_MOSI 15
#define SD_SCLK 14
#define SD_CS   13

const char *AP_SSID = "ESP32_NAS";
const char *AP_PASS = "12345678";

WebServer server(80);

String currentPath = "/";

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

  <script>
    let currentPath = "/";

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
      document.getElementById("path").innerText = "Path: " + currentPath;
      document.getElementById("list").innerHTML = "<div class='loading'>Loading...</div>";

      try {
        const response = await fetch("/api/list?path=" + encodeURIComponent(path));
        const data = await response.json();

        if (!data.ok) {
          document.getElementById("list").innerHTML = "<div class='loading'>Error: " + data.error + "</div>";
          return;
        }

        const list = document.getElementById("list");
        list.innerHTML = "";

        if (data.items.length === 0) {
          list.innerHTML = "<div class='loading'>Empty folder</div>";
          return;
        }

        data.items.forEach(item => {
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
            } else {
              window.location.href = "/download?path=" + encodeURIComponent(fullPath);
            }
          };

          list.appendChild(div);
        });

      } catch (e) {
        document.getElementById("list").innerHTML = "<div class='loading'>Connection error</div>";
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

  struct Item {
    String name;
    bool isDir;
    uint64_t size;
  };

  std::vector<Item> items;

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
      Item item;
      item.name = name;
      item.isDir = file.isDirectory();
      item.size = file.size();
      items.push_back(item);
    }

    file.close();
    file = dir.openNextFile();
  }

  dir.close();

  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
    if (a.isDir != b.isDir) {
      return a.isDir > b.isDir; // folders first
    }

    String nameA = a.name;
    String nameB = b.name;

    nameA.toLowerCase();
    nameB.toLowerCase();

    return nameA < nameB;
  });

  String json = "{\"ok\":true,\"path\":\"" + jsonEscape(path) + "\",\"items\":[";

  for (size_t i = 0; i < items.size(); i++) {
    if (i > 0) json += ",";

    json += "{";
    json += "\"name\":\"" + jsonEscape(items[i].name) + "\",";
    json += "\"type\":\"" + String(items[i].isDir ? "dir" : "file") + "\",";
    json += "\"size\":" + String((unsigned long long)items[i].size);
    json += "}";
  }

  json += "]}";

  server.send(200, "application/json", json);
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

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  Serial.println("WiFi Access Point started.");
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Password: ");
  Serial.println(AP_PASS);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/list", HTTP_GET, handleList);
  server.on("/download", HTTP_GET, handleDownload);

  server.begin();

  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();
}