#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <ESPFMfGK.h>

#define SD_MISO 2
#define SD_MOSI 15
#define SD_SCLK 14
#define SD_CS 13

const char *AP_SSID = "ESP32_SD_SERVER";
const char *AP_PASS = "12345678";

ESPFMfGK filemgr(80);

uint32_t checkFileFlags(fs::FS &fs, String filename, uint32_t flags)
{
  Serial.print("checkFileFlags: ");
  Serial.println(filename);

  if (filename.indexOf("/.Trash") >= 0 ||
      filename.indexOf("/Android") >= 0 ||
      filename.indexOf("/LOST.DIR") >= 0 ||
      // filename.indexOf("/DCIM") >= 0 ||
      filename.indexOf("/System Volume Information") >= 0)
  {
    return ESPFMfGK::flagIsNotVisible;
  }

  return ESPFMfGK::flagCanDelete |
         ESPFMfGK::flagCanRename |
         ESPFMfGK::flagCanEdit |
         ESPFMfGK::flagAllowPreview |
         ESPFMfGK::flagCanDownload |
         ESPFMfGK::flagCanUpload |
         ESPFMfGK::flagIsValidTargetFilename |
         ESPFMfGK::flagIsValidAction |
         ESPFMfGK::flagCanCreateNew;
}

void setup()
{
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("ESP32 SD Web File Manager");

  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

  // Slower SPI = more stable for big SD cards
  if (!SD.begin(SD_CS, SPI, 4000000))
  {
    Serial.println("SD card mount failed!");
    return;
  }

  Serial.println("SD mounted.");

  if (!SD.exists("/ESP32"))
  {
    SD.mkdir("/ESP32");
    Serial.println("Created /ESP32 folder.");
  }

  if (!SD.exists("/ESP32/hello.txt"))
  {
    File f = SD.open("/ESP32/hello.txt", FILE_WRITE);
    if (f)
    {
      f.println("Hello from ESP32 file manager!");
      f.close();
      Serial.println("Created /ESP32/hello.txt");
    }
  }
  Serial.println("Checking /ESP32 content:");

  File dir = SD.open("/ESP32");
  File file = dir.openNextFile();

  while (file)
  {
    Serial.print(file.isDirectory() ? "DIR : " : "FILE: ");
    Serial.print(file.name());
    Serial.print(" SIZE: ");
    Serial.println(file.size());

    file = dir.openNextFile();
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  Serial.print("Connect to WiFi: ");
  Serial.println(AP_SSID);
  Serial.print("Password: ");
  Serial.println(AP_PASS);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());

  filemgr.BackgroundColor = "white";
  filemgr.checkFileFlags = checkFileFlags;

  filemgr.WebPageTitle = "ESP32 SD Card Manager";

  filemgr.HttpUsername = "admin";
  filemgr.HttpPassword = "admin123";

  if (!filemgr.AddFS(SD, "SD Card", true))
  {
    Serial.println("AddFS failed!");
    return;
  }

  if (!filemgr.begin())
  {
    Serial.println("File manager failed!");
    return;
  }

  Serial.println("File manager started.");
}

void loop()
{
  filemgr.handleClient();
}