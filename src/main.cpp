#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>

#define SD_MISO 2
#define SD_MOSI 15
#define SD_SCLK 14
#define SD_CS 13

static const uint32_t SD_SPI_FREQ = 4000000;
static const uint16_t LIST_DEFAULT_LIMIT = 50;
static const uint16_t LIST_MAX_LIMIT = 100;
static const uint16_t MAX_PATH_LEN = 240;
static const size_t STREAM_BUFFER_SIZE = 8192;
static const uint32_t EXIF_SCAN_LIMIT = 512UL * 1024UL;
static const bool USE_WIFI_STA = true;

#ifndef WIFI_SSID
#error "WIFI_SSID is not defined. Add -D WIFI_SSID='\"your_wifi_name\"' to platformio.ini build_flags."
#endif

#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD is not defined. Add -D WIFI_PASSWORD='\"your_wifi_password\"' to platformio.ini build_flags."
#endif

#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "ESP32-SD-NAS"
#endif

#ifndef WIFI_AP_PASSWORD
#define WIFI_AP_PASSWORD "esp32nas"
#endif

WebServer server(80);

const char *HEADER_KEYS[] = {"Range"};
uint8_t streamBuffer[STREAM_BUFFER_SIZE];

File uploadFile;
String uploadPath;
uint32_t uploadStartedAt = 0;
uint64_t uploadBytes = 0;
bool uploadHadError = false;
bool uploadSawFile = false;
String uploadError;

String uint64ToString(uint64_t value) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)value);
  return String(buffer);
}

String boolJson(bool value) {
  return value ? "true" : "false";
}

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  value.replace("\t", "\\t");
  return value;
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type,Range");
  server.sendHeader("Access-Control-Expose-Headers", "Content-Length,Content-Range,Accept-Ranges");
}

void sendJson(int status, const String &body) {
  addCorsHeaders();
  server.send(status, "application/json", body);
}

void sendJsonError(int status, const String &error) {
  sendJson(status, "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
}

void logRequest(const char *label) {
  Serial.print("REQ ");
  Serial.print(label);
  Serial.print(" uri=");
  Serial.println(server.uri());
}

void handleOptions() {
  addCorsHeaders();
  server.send(204, "text/plain", "");
}

bool hasControlChars(const String &value) {
  for (size_t i = 0; i < value.length(); i++) {
    if ((uint8_t)value[i] < 32 || value[i] == 127) {
      return true;
    }
  }
  return false;
}

bool normalizePath(String raw, String &outPath) {
  raw.trim();
  raw.replace("\\", "/");

  if (raw.length() == 0) {
    raw = "/";
  }

  if (!raw.startsWith("/")) {
    raw = "/" + raw;
  }

  if (raw.length() > MAX_PATH_LEN || hasControlChars(raw)) {
    return false;
  }

  String normalized = "/";
  int index = 1;

  while (index < (int)raw.length()) {
    while (index < (int)raw.length() && raw[index] == '/') {
      index++;
    }

    if (index >= (int)raw.length()) {
      break;
    }

    int nextSlash = raw.indexOf('/', index);
    if (nextSlash < 0) {
      nextSlash = raw.length();
    }

    String segment = raw.substring(index, nextSlash);
    if (segment.length() == 0 || segment == "." || segment == ".." || segment.indexOf("..") >= 0) {
      return false;
    }

    if (normalized.length() > 1) {
      normalized += "/";
    }
    normalized += segment;

    if (normalized.length() > MAX_PATH_LEN) {
      return false;
    }

    index = nextSlash + 1;
  }

  outPath = normalized;
  return true;
}

String getBaseName(String path) {
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash >= 0) {
    return path.substring(lastSlash + 1);
  }
  return path;
}

String getParentPath(String path) {
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash <= 0) {
    return "/";
  }
  return path.substring(0, lastSlash);
}

bool sanitizeFileName(String raw, String &name) {
  raw.replace("\\", "/");
  name = getBaseName(raw);
  name.trim();

  if (name.length() == 0 || name.length() > 96 || hasControlChars(name)) {
    return false;
  }

  if (name == "." || name == ".." || name.indexOf("..") >= 0 || name.indexOf('/') >= 0) {
    return false;
  }

  return true;
}

uint32_t parseUIntArg(const char *name, uint32_t defaultValue, uint32_t maxValue) {
  if (!server.hasArg(name)) {
    return defaultValue;
  }

  String value = server.arg(name);
  value.trim();
  if (value.length() == 0) {
    return defaultValue;
  }

  char *endPtr = nullptr;
  unsigned long parsed = strtoul(value.c_str(), &endPtr, 10);
  if (endPtr == value.c_str() || *endPtr != '\0') {
    return defaultValue;
  }

  return parsed > maxValue ? maxValue : parsed;
}

bool parseBoolArg(const char *name, bool defaultValue) {
  if (!server.hasArg(name)) {
    return defaultValue;
  }

  String value = server.arg(name);
  value.toLowerCase();
  value.trim();

  if (value == "1" || value == "true" || value == "yes") {
    return true;
  }

  if (value == "0" || value == "false" || value == "no") {
    return false;
  }

  return defaultValue;
}

String getEntryName(File &file) {
  String name = String(file.name());
  int lastSlash = name.lastIndexOf('/');
  if (lastSlash >= 0) {
    name = name.substring(lastSlash + 1);
  }
  return name;
}

bool filterMatches(const String &name, const String &filter) {
  if (filter.length() == 0) {
    return true;
  }

  String lowerName = name;
  String lowerFilter = filter;
  lowerName.toLowerCase();
  lowerFilter.toLowerCase();
  return lowerName.indexOf(lowerFilter) >= 0;
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
  if (filename.endsWith(".webp")) return "image/webp";
  if (filename.endsWith(".mp4")) return "video/mp4";
  if (filename.endsWith(".mov")) return "video/quicktime";
  if (filename.endsWith(".txt")) return "text/plain";
  if (filename.endsWith(".pdf")) return "application/pdf";
  if (filename.endsWith(".json")) return "application/json";
  if (filename.endsWith(".csv")) return "text/csv";

  return "application/octet-stream";
}

bool isJpegPath(String filename) {
  filename.toLowerCase();
  return filename.endsWith(".jpg") || filename.endsWith(".jpeg");
}

bool readBytesAt(File &file, uint32_t position, uint8_t *buffer, size_t length) {
  if (!file.seek(position)) {
    return false;
  }
  return file.read(buffer, length) == length;
}

bool readU16BEAt(File &file, uint32_t position, uint16_t &value) {
  uint8_t buffer[2];
  if (!readBytesAt(file, position, buffer, sizeof(buffer))) {
    return false;
  }
  value = ((uint16_t)buffer[0] << 8) | buffer[1];
  return true;
}

uint16_t readU16Tiff(const uint8_t *buffer, bool littleEndian) {
  if (littleEndian) {
    return ((uint16_t)buffer[1] << 8) | buffer[0];
  }
  return ((uint16_t)buffer[0] << 8) | buffer[1];
}

uint32_t readU32Tiff(const uint8_t *buffer, bool littleEndian) {
  if (littleEndian) {
    return ((uint32_t)buffer[3] << 24) | ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[1] << 8) | buffer[0];
  }
  return ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) | ((uint32_t)buffer[2] << 8) | buffer[3];
}

bool readU16TiffAt(File &file, uint32_t position, bool littleEndian, uint16_t &value) {
  uint8_t buffer[2];
  if (!readBytesAt(file, position, buffer, sizeof(buffer))) {
    return false;
  }
  value = readU16Tiff(buffer, littleEndian);
  return true;
}

bool readU32TiffAt(File &file, uint32_t position, bool littleEndian, uint32_t &value) {
  uint8_t buffer[4];
  if (!readBytesAt(file, position, buffer, sizeof(buffer))) {
    return false;
  }
  value = readU32Tiff(buffer, littleEndian);
  return true;
}

bool findJpegExifThumbnail(File &file, uint32_t &thumbnailOffset, uint32_t &thumbnailLength) {
  thumbnailOffset = 0;
  thumbnailLength = 0;

  uint64_t fileSize64 = file.size();
  if (fileSize64 < 16 || fileSize64 > 0xFFFFFFFFULL) {
    return false;
  }

  uint32_t fileSize = (uint32_t)fileSize64;
  uint8_t soi[2];
  if (!readBytesAt(file, 0, soi, sizeof(soi)) || soi[0] != 0xFF || soi[1] != 0xD8) {
    return false;
  }

  uint32_t position = 2;
  uint32_t scanLimit = min(fileSize, EXIF_SCAN_LIMIT);

  while (position + 4 < scanLimit) {
    uint8_t markerPrefix = 0;
    if (!readBytesAt(file, position, &markerPrefix, 1)) {
      return false;
    }

    if (markerPrefix != 0xFF) {
      position++;
      continue;
    }

    do {
      position++;
      if (position >= scanLimit) {
        return false;
      }
      if (!readBytesAt(file, position, &markerPrefix, 1)) {
        return false;
      }
    } while (markerPrefix == 0xFF);

    uint8_t marker = markerPrefix;
    position++;

    if (marker == 0xDA || marker == 0xD9) {
      return false;
    }

    if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
      continue;
    }

    uint16_t segmentLength = 0;
    if (!readU16BEAt(file, position, segmentLength) || segmentLength < 2) {
      return false;
    }

    uint32_t payloadStart = position + 2;
    uint32_t payloadLength = segmentLength - 2;
    uint32_t segmentEnd = payloadStart + payloadLength;
    if (segmentEnd > fileSize || segmentEnd <= payloadStart) {
      return false;
    }

    if (marker == 0xE1 && payloadLength >= 14) {
      uint8_t exifHeader[6];
      if (readBytesAt(file, payloadStart, exifHeader, sizeof(exifHeader)) &&
          exifHeader[0] == 'E' && exifHeader[1] == 'x' && exifHeader[2] == 'i' &&
          exifHeader[3] == 'f' && exifHeader[4] == 0 && exifHeader[5] == 0) {
        uint32_t tiffStart = payloadStart + 6;
        uint32_t tiffLength = payloadLength - 6;
        uint8_t tiffHeader[8];

        if (!readBytesAt(file, tiffStart, tiffHeader, sizeof(tiffHeader))) {
          return false;
        }

        bool littleEndian = false;
        if (tiffHeader[0] == 'I' && tiffHeader[1] == 'I') {
          littleEndian = true;
        } else if (tiffHeader[0] == 'M' && tiffHeader[1] == 'M') {
          littleEndian = false;
        } else {
          return false;
        }

        if (readU16Tiff(tiffHeader + 2, littleEndian) != 42) {
          return false;
        }

        uint32_t ifd0Offset = readU32Tiff(tiffHeader + 4, littleEndian);
        if (ifd0Offset + 2 > tiffLength) {
          return false;
        }

        uint32_t ifd0Position = tiffStart + ifd0Offset;
        uint16_t ifd0Count = 0;
        if (!readU16TiffAt(file, ifd0Position, littleEndian, ifd0Count)) {
          return false;
        }

        uint32_t nextIfdOffsetPosition = ifd0Position + 2 + ((uint32_t)ifd0Count * 12);
        if (nextIfdOffsetPosition + 4 > tiffStart + tiffLength) {
          return false;
        }

        uint32_t ifd1Offset = 0;
        if (!readU32TiffAt(file, nextIfdOffsetPosition, littleEndian, ifd1Offset) ||
            ifd1Offset == 0 || ifd1Offset + 2 > tiffLength) {
          return false;
        }

        uint32_t ifd1Position = tiffStart + ifd1Offset;
        uint16_t ifd1Count = 0;
        if (!readU16TiffAt(file, ifd1Position, littleEndian, ifd1Count)) {
          return false;
        }

        uint32_t jpegOffset = 0;
        uint32_t jpegLength = 0;

        for (uint16_t index = 0; index < ifd1Count; index++) {
          uint32_t entryPosition = ifd1Position + 2 + ((uint32_t)index * 12);
          if (entryPosition + 12 > tiffStart + tiffLength) {
            return false;
          }

          uint8_t entry[12];
          if (!readBytesAt(file, entryPosition, entry, sizeof(entry))) {
            return false;
          }

          uint16_t tag = readU16Tiff(entry, littleEndian);
          uint32_t value = readU32Tiff(entry + 8, littleEndian);

          if (tag == 0x0201) {
            jpegOffset = value;
          } else if (tag == 0x0202) {
            jpegLength = value;
          }
        }

        uint64_t thumbnailEnd = (uint64_t)jpegOffset + jpegLength;
        if (jpegOffset == 0 || jpegLength == 0 || thumbnailEnd > tiffLength) {
          return false;
        }

        thumbnailOffset = tiffStart + jpegOffset;
        thumbnailLength = jpegLength;
        return (uint64_t)thumbnailOffset + thumbnailLength <= segmentEnd;
      }
    }

    position = segmentEnd;
  }

  return false;
}

String getSidecarPreviewPath(const String &path) {
  String sidecarPath = "/.thumbs" + path + ".jpg";
  if (sidecarPath.length() > MAX_PATH_LEN) {
    return "";
  }
  return sidecarPath;
}

size_t sendJsonChunk(const String &chunk) {
  server.sendContent(chunk);
  return chunk.length();
}

bool emitListItem(File &file, const String &name, bool &firstItem, size_t &jsonBytes) {
  if (name.length() == 0) {
    return false;
  }

  String item = firstItem ? "" : ",";
  item += "{\"name\":\"";
  item += jsonEscape(name);
  item += "\",\"type\":\"";
  item += file.isDirectory() ? "dir" : "file";
  item += "\",\"size\":";
  item += uint64ToString(file.isDirectory() ? 0 : file.size());
  item += "}";

  jsonBytes += sendJsonChunk(item);
  firstItem = false;
  return true;
}

void logListBenchmark(
  const String &path,
  uint32_t offset,
  uint32_t limit,
  uint32_t scanned,
  uint32_t matched,
  uint32_t returned,
  size_t jsonBytes,
  uint32_t elapsedMs,
  bool hasMore
) {
  Serial.print("LIST path=");
  Serial.print(path);
  Serial.print(" offset=");
  Serial.print(offset);
  Serial.print(" limit=");
  Serial.print(limit);
  Serial.print(" scanned=");
  Serial.print(scanned);
  Serial.print(" matched=");
  Serial.print(matched);
  Serial.print(" returned=");
  Serial.print(returned);
  Serial.print(" bytes=");
  Serial.print(jsonBytes);
  Serial.print(" ms=");
  Serial.print(elapsedMs);
  Serial.print(" hasMore=");
  Serial.println(hasMore ? 1 : 0);
}

void logTransferBenchmark(const char *label, const String &path, uint64_t size, uint32_t elapsedMs) {
  float mb = (float)size / (1024.0f * 1024.0f);
  float seconds = elapsedMs > 0 ? (float)elapsedMs / 1000.0f : 0.001f;

  Serial.print(label);
  Serial.print(" path=");
  Serial.print(path);
  Serial.print(" size=");
  Serial.print(uint64ToString(size));
  Serial.print(" ms=");
  Serial.print(elapsedMs);
  Serial.print(" speed=");
  Serial.print(mb / seconds, 2);
  Serial.println(" MB/s");
}

void handleInfo() {
  logRequest("INFO");
  IPAddress ip = USE_WIFI_STA ? WiFi.localIP() : WiFi.softAPIP();
  String body = "{\"ok\":true";
  body += ",\"mode\":\"";
  body += USE_WIFI_STA ? "sta" : "ap";
  body += "\",\"ip\":\"";
  body += ip.toString();
  body += "\",\"rssi\":";
  body += USE_WIFI_STA ? String(WiFi.RSSI()) : String(0);
  body += ",\"heap\":";
  body += String(ESP.getFreeHeap());
  body += ",\"sdSpiFreq\":";
  body += String(SD_SPI_FREQ);
  body += ",\"cardSize\":";
  body += uint64ToString(SD.cardSize());
  body += ",\"usedBytes\":";
  body += uint64ToString(SD.usedBytes());
  body += "}";
  sendJson(200, body);
}

void handleList() {
  uint32_t startedAt = millis();
  logRequest("LIST_START");

  String path = "/";
  if (server.hasArg("path") && !normalizePath(server.arg("path"), path)) {
    sendJsonError(400, "Invalid path");
    return;
  }

  uint32_t offset = parseUIntArg("offset", 0, 1000000);
  uint32_t limit = parseUIntArg("limit", LIST_DEFAULT_LIMIT, LIST_MAX_LIMIT);
  if (limit == 0) {
    limit = LIST_DEFAULT_LIMIT;
  }

  String sort = server.hasArg("sort") ? server.arg("sort") : "none";
  sort.toLowerCase();
  sort.trim();

  if (sort.length() == 0) {
    sort = "none";
  }

  if (sort != "none") {
    sendJsonError(400, "Sorting is not implemented yet; use sort=none");
    return;
  }

  String filter = server.hasArg("filter") ? server.arg("filter") : "";
  filter.trim();

  if (filter.length() > 64 || hasControlChars(filter)) {
    sendJsonError(400, "Invalid filter");
    return;
  }

  File dir = SD.open(path);
  if (!dir) {
    sendJsonError(404, "Folder not found");
    return;
  }

  if (!dir.isDirectory()) {
    dir.close();
    sendJsonError(400, "Not a folder");
    return;
  }

  uint32_t scanned = 0;
  uint32_t matched = 0;
  uint32_t returned = 0;
  bool hasMore = false;
  bool firstItem = true;
  size_t jsonBytes = 0;

  addCorsHeaders();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  String header = "{\"ok\":true,\"path\":\"" + jsonEscape(path) + "\",\"offset\":" + String(offset) +
                  ",\"limit\":" + String(limit) + ",\"sort\":\"none\",\"items\":[";
  jsonBytes += sendJsonChunk(header);

  File entry = dir.openNextFile();
  while (entry) {
    scanned++;
    String name = getEntryName(entry);

    if (filterMatches(name, filter)) {
      if (matched < offset) {
        matched++;
      } else if (returned < limit) {
        if (emitListItem(entry, name, firstItem, jsonBytes)) {
          returned++;
          matched++;
        }
      } else {
        hasMore = true;
        entry.close();
        break;
      }
    }

    entry.close();
    entry = dir.openNextFile();
  }

  dir.close();

  String footer = "],\"count\":" + String(returned) + ",\"hasMore\":" + boolJson(hasMore) + "}";
  jsonBytes += sendJsonChunk(footer);
  server.sendContent("");

  logListBenchmark(path, offset, limit, scanned, matched, returned, jsonBytes, millis() - startedAt, hasMore);
}

bool parseRangeValue(const String &range, uint64_t fileSize, uint64_t &start, uint64_t &end) {
  if (!range.startsWith("bytes=") || fileSize == 0) {
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

void streamFileBytes(File &file, uint64_t length) {
  WiFiClient client = server.client();
  uint64_t remaining = length;

  while (remaining > 0 && client.connected()) {
    size_t toRead = remaining > STREAM_BUFFER_SIZE ? STREAM_BUFFER_SIZE : (size_t)remaining;
    size_t bytesRead = file.read(streamBuffer, toRead);

    if (bytesRead == 0) {
      break;
    }

    client.write(streamBuffer, bytesRead);
    remaining -= bytesRead;
    delay(0);
  }
}

void handleDownload() {
  uint32_t startedAt = millis();
  logRequest("DOWNLOAD_START");

  if (!server.hasArg("path")) {
    sendJsonError(400, "Missing path");
    return;
  }

  String path;
  if (!normalizePath(server.arg("path"), path)) {
    sendJsonError(400, "Invalid path");
    return;
  }

  File file = SD.open(path, FILE_READ);
  if (!file) {
    sendJsonError(404, "File not found");
    return;
  }

  if (file.isDirectory()) {
    file.close();
    sendJsonError(400, "Cannot download folder");
    return;
  }

  String filename = getBaseName(path);
  String contentType = getContentType(filename);
  uint64_t fileSize = file.size();
  bool inlineMode = parseBoolArg("inline", false);

  addCorsHeaders();
  server.sendHeader("Accept-Ranges", "bytes");

  if (!inlineMode) {
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + jsonEscape(filename) + "\"");
  }

  if (server.hasHeader("Range")) {
    uint64_t start = 0;
    uint64_t end = fileSize > 0 ? fileSize - 1 : 0;

    if (parseRangeValue(server.header("Range"), fileSize, start, end)) {
      uint64_t length = end - start + 1;
      server.sendHeader("Content-Range", "bytes " + uint64ToString(start) + "-" + uint64ToString(end) + "/" + uint64ToString(fileSize));
      server.setContentLength((size_t)length);
      server.send(206, contentType, "");
      file.seek((uint32_t)start);
      streamFileBytes(file, length);
      logTransferBenchmark("DOWNLOAD_RANGE", path, length, millis() - startedAt);
    } else {
      server.sendHeader("Content-Range", "bytes */" + uint64ToString(fileSize));
      server.send(416, "text/plain", "Requested range not satisfiable");
    }
  } else {
    server.setContentLength((size_t)fileSize);
    server.send(200, contentType, "");
    streamFileBytes(file, fileSize);
    logTransferBenchmark("DOWNLOAD", path, fileSize, millis() - startedAt);
  }

  file.close();
}

void handlePreview() {
  uint32_t startedAt = millis();
  logRequest("PREVIEW_START");

  if (!server.hasArg("path")) {
    sendJsonError(400, "Missing path");
    return;
  }

  String path;
  if (!normalizePath(server.arg("path"), path)) {
    sendJsonError(400, "Invalid path");
    return;
  }

  String sidecarPath = getSidecarPreviewPath(path);
  if (sidecarPath.length() > 0) {
    File sidecar = SD.open(sidecarPath, FILE_READ);
    if (sidecar && !sidecar.isDirectory()) {
      uint64_t sidecarSize = sidecar.size();
      addCorsHeaders();
      server.sendHeader("Cache-Control", "public, max-age=86400");
      server.sendHeader("X-Preview-Source", "sidecar");
      server.setContentLength((size_t)sidecarSize);
      server.send(200, "image/jpeg", "");
      streamFileBytes(sidecar, sidecarSize);
      sidecar.close();
      logTransferBenchmark("PREVIEW_SIDECAR", sidecarPath, sidecarSize, millis() - startedAt);
      return;
    }

    if (sidecar) {
      sidecar.close();
    }
  }

  if (!isJpegPath(path)) {
    Serial.print("PREVIEW_MISS_NON_JPEG path=");
    Serial.print(path);
    Serial.print(" sidecar=");
    Serial.print(sidecarPath);
    Serial.print(" ms=");
    Serial.println(millis() - startedAt);
    sendJsonError(404, "No fast preview available");
    return;
  }

  File file = SD.open(path, FILE_READ);
  if (!file) {
    sendJsonError(404, "File not found");
    return;
  }

  if (file.isDirectory()) {
    file.close();
    sendJsonError(400, "Cannot preview folder");
    return;
  }

  uint32_t thumbnailOffset = 0;
  uint32_t thumbnailLength = 0;

  if (findJpegExifThumbnail(file, thumbnailOffset, thumbnailLength)) {
    addCorsHeaders();
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.sendHeader("X-Preview-Source", "exif");
    server.setContentLength(thumbnailLength);
    server.send(200, "image/jpeg", "");
    file.seek(thumbnailOffset);
    streamFileBytes(file, thumbnailLength);
    file.close();
    logTransferBenchmark("PREVIEW_EXIF", path, thumbnailLength, millis() - startedAt);
    return;
  }

  file.close();

  Serial.print("PREVIEW_MISS path=");
  Serial.print(path);
  Serial.print(" sidecar=");
  Serial.print(sidecarPath);
  Serial.print(" ms=");
  Serial.println(millis() - startedAt);
  sendJsonError(404, "No fast preview available");
}

void handleUploadChunk() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    logRequest("UPLOAD_START");
    uploadHadError = false;
    uploadSawFile = true;
    uploadError = "";
    uploadBytes = 0;
    uploadStartedAt = millis();
    uploadPath = "";

    String folder = "/";
    String name;

    if (server.hasArg("path") && !normalizePath(server.arg("path"), folder)) {
      uploadHadError = true;
      uploadError = "Invalid target path";
      return;
    }

    if (!sanitizeFileName(upload.filename, name)) {
      uploadHadError = true;
      uploadError = "Invalid upload filename";
      return;
    }

    File dir = SD.open(folder);
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        dir.close();
      }
      uploadHadError = true;
      uploadError = "Target folder not found";
      return;
    }
    dir.close();

    uploadPath = folder == "/" ? "/" + name : folder + "/" + name;
    if (SD.exists(uploadPath)) {
      SD.remove(uploadPath);
    }

    uploadFile = SD.open(uploadPath, FILE_WRITE);
    if (!uploadFile) {
      uploadHadError = true;
      uploadError = "Could not open destination file";
      return;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadHadError || !uploadFile) {
      return;
    }

    size_t written = uploadFile.write(upload.buf, upload.currentSize);
    uploadBytes += written;

    if (written != upload.currentSize) {
      uploadHadError = true;
      uploadError = "SD write failed";
      uploadFile.close();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
    }
    logTransferBenchmark("UPLOAD", uploadPath, uploadBytes, millis() - uploadStartedAt);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) {
      uploadFile.close();
    }
    uploadHadError = true;
    uploadError = "Upload aborted";
  }
}

void handleUploadDone() {
  logRequest("UPLOAD_DONE");
  if (!uploadSawFile) {
    sendJsonError(400, "No upload file received");
    return;
  }

  if (uploadHadError) {
    if (uploadPath.length() > 0 && SD.exists(uploadPath)) {
      SD.remove(uploadPath);
    }
    uploadSawFile = false;
    sendJsonError(500, uploadError.length() ? uploadError : "Upload failed");
    return;
  }

  String name = getBaseName(uploadPath);
  sendJson(200, "{\"ok\":true,\"path\":\"" + jsonEscape(uploadPath) +
                  "\",\"name\":\"" + jsonEscape(name) +
                  "\",\"size\":" + uint64ToString(uploadBytes) + "}");
  uploadSawFile = false;
}

void handleMkdir() {
  logRequest("MKDIR");
  if (!server.hasArg("path")) {
    sendJsonError(400, "Missing path");
    return;
  }

  String path;
  if (!normalizePath(server.arg("path"), path) || path == "/") {
    sendJsonError(400, "Invalid path");
    return;
  }

  if (SD.exists(path)) {
    sendJsonError(409, "Path already exists");
    return;
  }

  if (!SD.mkdir(path)) {
    sendJsonError(500, "Create folder failed");
    return;
  }

  sendJson(200, "{\"ok\":true,\"path\":\"" + jsonEscape(path) + "\"}");
}

void handleDelete() {
  logRequest("DELETE");
  if (!server.hasArg("path")) {
    sendJsonError(400, "Missing path");
    return;
  }

  String path;
  if (!normalizePath(server.arg("path"), path) || path == "/") {
    sendJsonError(400, "Invalid path");
    return;
  }

  File entry = SD.open(path);
  if (!entry) {
    sendJsonError(404, "Path not found");
    return;
  }

  bool isDir = entry.isDirectory();
  entry.close();

  bool ok = isDir ? SD.rmdir(path) : SD.remove(path);
  if (!ok) {
    sendJsonError(500, isDir ? "Delete folder failed; folder must be empty" : "Delete file failed");
    return;
  }

  sendJson(200, "{\"ok\":true,\"path\":\"" + jsonEscape(path) + "\"}");
}

void handleRename() {
  logRequest("RENAME");
  if (!server.hasArg("from") || !server.hasArg("to")) {
    sendJsonError(400, "Missing from or to path");
    return;
  }

  String fromPath;
  String toPath;
  if (!normalizePath(server.arg("from"), fromPath) || !normalizePath(server.arg("to"), toPath) ||
      fromPath == "/" || toPath == "/") {
    sendJsonError(400, "Invalid path");
    return;
  }

  if (!SD.exists(fromPath)) {
    sendJsonError(404, "Source not found");
    return;
  }

  if (SD.exists(toPath)) {
    sendJsonError(409, "Destination already exists");
    return;
  }

  if (getParentPath(fromPath) != getParentPath(toPath)) {
    sendJsonError(400, "Rename must stay in the same folder");
    return;
  }

  if (!SD.rename(fromPath, toPath)) {
    sendJsonError(500, "Rename failed");
    return;
  }

  sendJson(200, "{\"ok\":true,\"from\":\"" + jsonEscape(fromPath) +
                  "\",\"to\":\"" + jsonEscape(toPath) + "\"}");
}

void handleNotFound() {
  logRequest("NOT_FOUND");
  if (server.method() == HTTP_OPTIONS) {
    handleOptions();
    return;
  }

  sendJsonError(404, "Not found");
}

void startWiFi() {
  if (USE_WIFI_STA) {
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
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());
    Serial.print("Open API: http://");
    Serial.println(WiFi.localIP());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);

    Serial.println("WiFi AP started.");
    Serial.print("SSID: ");
    Serial.println(WIFI_AP_SSID);
    Serial.print("Open API: http://");
    Serial.println(WiFi.softAPIP());
  }
}

void registerRoute(const char *path, HTTPMethod method, void (*handler)()) {
  server.on(path, method, handler);
  server.on(path, HTTP_OPTIONS, handleOptions);
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("=================================");
  Serial.println("ESP32 SD NAS API");
  Serial.println("=================================");

  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, SPI, SD_SPI_FREQ)) {
    Serial.println("SD card mount failed!");
    return;
  }

  Serial.println("SD card mounted.");
  Serial.print("SD SPI frequency: ");
  Serial.println(SD_SPI_FREQ);
  Serial.print("Card size: ");
  Serial.print(SD.cardSize() / (1024 * 1024));
  Serial.println(" MB");

  startWiFi();

  registerRoute("/", HTTP_GET, handleInfo);
  registerRoute("/api/info", HTTP_GET, handleInfo);
  registerRoute("/api/list", HTTP_GET, handleList);
  registerRoute("/api/download", HTTP_GET, handleDownload);
  registerRoute("/api/preview", HTTP_GET, handlePreview);
  registerRoute("/api/mkdir", HTTP_POST, handleMkdir);
  registerRoute("/api/delete", HTTP_POST, handleDelete);
  registerRoute("/api/rename", HTTP_POST, handleRename);

  server.on("/api/upload", HTTP_OPTIONS, handleOptions);
  server.on("/api/upload", HTTP_POST, handleUploadDone, handleUploadChunk);
  server.onNotFound(handleNotFound);
  server.collectHeaders(HEADER_KEYS, 1);
  server.begin();

  Serial.println("API server started.");
}

void loop() {
  server.handleClient();
}
