#include <Arduino.h>
#include <NimBLEDevice.h>
#include <TFT_eSPI.h>
#include "USB.h"
#include "USBHID.h"

// HID Service and Characteristic UUIDs
static NimBLEUUID HID_SERVICE_UUID((uint16_t)0x1812);
static NimBLEUUID HID_REPORT_MAP_UUID((uint16_t)0x2A4B);
static NimBLEUUID HID_REPORT_UUID((uint16_t)0x2A4D);
static NimBLEUUID HID_INFO_UUID((uint16_t)0x2A4A);
static NimBLEUUID BATTERY_SERVICE_UUID((uint16_t)0x180F);
static NimBLEUUID BATTERY_LEVEL_UUID((uint16_t)0x2A19);
static NimBLEUUID DEVICE_INFO_UUID((uint16_t)0x180A);
static NimBLEUUID GAP_SERVICE_UUID((uint16_t)0x1800);
static NimBLEUUID DEVICE_NAME_UUID((uint16_t)0x2A00);
static NimBLEUUID PNP_ID_UUID((uint16_t)0x2A50);
static NimBLEUUID MANUFACTURER_NAME_UUID((uint16_t)0x2A29);

// Scan duration in milliseconds
#define SCAN_DURATION 2000

// Connected device handle
static NimBLEClient *pClient = nullptr;
static bool deviceConnected = false;
static bool doConnect = false;
static NimBLEAdvertisedDevice *advDevice = nullptr;

// TFT Display
TFT_eSPI tft = TFT_eSPI();

// Custom USB HID device to use dynamic report descriptor
class ProxyHIDDevice : public USBHIDDevice
{
public:
  ProxyHIDDevice()
  {
    _descriptor = nullptr;
    _descriptorSize = 0;
  }

  void setDescriptor(const uint8_t *desc, uint16_t len)
  {
    _descriptor = desc;
    _descriptorSize = len;
  }

  uint16_t _onGetDescriptor(uint8_t *buffer) override
  {
    if (_descriptor && _descriptorSize > 0)
    {
      memcpy(buffer, _descriptor, _descriptorSize);
      return _descriptorSize;
    }
    return 0;
  }

private:
  const uint8_t *_descriptor;
  uint16_t _descriptorSize;
};

// USB HID
USBHID HID;
ProxyHIDDevice proxyDevice;
static uint8_t *reportMapData = nullptr;
static size_t reportMapSize = 0;
static bool usbReady = false;

// HID report parsing
struct HIDReportInfo
{
  bool hasKeyboard = false;
  bool hasMouse = false;
  bool hasConsumer = false;
  uint8_t keyboardReportId = 0;
  uint8_t mouseReportId = 0;
  uint8_t consumerReportId = 0;
};

// Forward declarations
void connectToDevice();
void subscribeToReports(NimBLEClient *client);
HIDReportInfo parseReportMap(const uint8_t *data, size_t len);

void clearDisplay()
{
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
}

// Notification callback for HID reports
void notifyCallback(NimBLERemoteCharacteristic *pChar, uint8_t *pData, size_t length, bool isNotify)
{
  Serial.printf("[%s] %s Report, Len: %d, Data: ",
                pClient ? pClient->getPeerAddress().toString().c_str() : "??:??:??:??:??:??",
                isNotify ? "INPUT" : "INDICATE", length);
  for (size_t i = 0; i < length; i++)
    Serial.printf("%02X ", pData[i]);
  Serial.println();

  // Forward the raw report to USB HID
  if (usbReady && length > 0)
  {
    uint8_t reportId = 0;
    const uint8_t *reportData = pData;
    size_t reportLen = length;

    // Heuristic detection based on common report formats:
    // Keyboard: 8 bytes (modifier, reserved, key1-6)
    // Mouse: 3-5 bytes (buttons, x, y, [wheel], [pan])
    if (length == 8)
    {
      reportId = HID_REPORT_ID_KEYBOARD;
      Serial.println("Detected as keyboard report");
    }
    else if (length >= 3 && length <= 5)
    {
      reportId = HID_REPORT_ID_MOUSE;
      Serial.println("Detected as mouse report");
    }
    else if (length == 2)
    {
      reportId = HID_REPORT_ID_CONSUMER_CONTROL;
      Serial.println("Detected as consumer control report");
    }
    else
    {
      // Unknown report type, check if first byte is a report ID
      if (pData[0] > 0 && pData[0] <= 7)
      {
        reportId = pData[0];
        reportData = pData + 1;
        reportLen = length - 1;
        Serial.printf("Using embedded report ID: %d\n", reportId);
      }
      else
        Serial.println("WARNING: Unknown report format, sending as report ID 0");
    }

    bool success = HID.SendReport(reportId, reportData, reportLen);
    Serial.printf("SendReport(id=%d, len=%d) -> %s\n", reportId, reportLen, success ? "OK" : "FAILED");
  }
}

// Battery level notification callback
void batteryNotifyCallback(NimBLERemoteCharacteristic *pChar, uint8_t *pData, size_t length, bool isNotify)
{
  if (length > 0)
  {
    Serial.printf("[BATTERY] Level: %d%%\n", pData[0]);
    tft.printf("BATT: %d%%\n", pData[0]);
  }
}

// Client callbacks
class ClientCallbacks : public NimBLEClientCallbacks
{
  void onConnect(NimBLEClient *pClient) override
  {
    Serial.printf("[%s] Connected!\n", pClient->getPeerAddress().toString().c_str());

    clearDisplay();
    tft.setTextColor(TFT_GREEN);
    tft.printf("CONNECTED to %s\n",
               pClient->getPeerAddress().toString().c_str());
    tft.setTextColor(TFT_WHITE);

    deviceConnected = true;
  }

  void onDisconnect(NimBLEClient *pClient, int reason) override
  {
    Serial.printf("[%s] Disconnected, reason: %d\n",
                  pClient->getPeerAddress().toString().c_str(), reason);

    clearDisplay();
    tft.setTextColor(TFT_RED);
    tft.drawCentreString("DISCONNECTED", tft.width() / 2, tft.height() / 2, 1);
    tft.setTextColor(TFT_WHITE);

    deviceConnected = false;

    NimBLEDevice::deleteClient(pClient);
    pClient = nullptr;
    delete advDevice;
    advDevice = nullptr;
    return;
  }

  void onConfirmPasskey(NimBLEConnInfo &connInfo, uint32_t passkey) override
  {
    Serial.printf("Confirm passkey: %06u - accepting\n", passkey);

    clearDisplay();
    tft.setTextColor(TFT_MAGENTA);
    tft.printf("Passkey: %06u\n", passkey);
    tft.setTextColor(TFT_WHITE);

    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }

  void onAuthenticationComplete(NimBLEConnInfo &connInfo) override
  {
    if (connInfo.isEncrypted())
      Serial.println("Authentication SUCCESS - connection encrypted");
    else
      Serial.println("Authentication FAILED");
  }

  void onIdentity(NimBLEConnInfo &connInfo) override
  {
    Serial.printf("Peer identity resolved: %s\n",
                  NimBLEAddress(connInfo.getIdAddress()).toString().c_str());
  }
};

static ClientCallbacks clientCallbacks;

// Scan callbacks
class ScanCallbacks : public NimBLEScanCallbacks
{
  void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override
  {
    Serial.printf("Found: %s, RSSI: %d",
                  advertisedDevice->getAddress().toString().c_str(),
                  advertisedDevice->getRSSI());

    if (advertisedDevice->haveName())
      Serial.printf(", Name: %s", advertisedDevice->getName().c_str());

    if (advertisedDevice->haveAppearance())
      Serial.printf(", Appearance: 0x%04X", advertisedDevice->getAppearance());
    Serial.println();

    // Check if this device has HID service
    if (advertisedDevice->isAdvertisingService(HID_SERVICE_UUID))
    {
      Serial.println("  -> HID Service found!");

      tft.printf("* %s, RSSI: %d",
                 advertisedDevice->getAddress().toString().c_str(),
                 advertisedDevice->getRSSI());

      if (advertisedDevice->haveName())
        tft.printf(", %s", advertisedDevice->getName().c_str());

      tft.println();

      if (advDevice == nullptr)
        advDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
    }
  }

  void onScanEnd(const NimBLEScanResults &results, int reason) override
  {
    Serial.printf("Scan complete, found %d devices\n", results.getCount());

    tft.setTextColor(TFT_GREEN);
    tft.printf("Complete, found %d devices\n", results.getCount());
    tft.setTextColor(TFT_WHITE);

    if (advDevice != nullptr && !doConnect)
      doConnect = true;
  }
};

static ScanCallbacks scanCallbacks;

void printDeviceInfo(NimBLEClient *client)
{
  Serial.println("\n========== Device Information ==========");
  Serial.printf("Address: %s\n", client->getPeerAddress().toString().c_str());

  // Get Device Name
  NimBLERemoteService *gapSvc = client->getService(GAP_SERVICE_UUID);
  if (gapSvc)
  {
    NimBLERemoteCharacteristic *nameChar = gapSvc->getCharacteristic(DEVICE_NAME_UUID);
    if (nameChar && nameChar->canRead())
    {
      Serial.printf("Device Name: %s\n", nameChar->readValue().c_str());
      tft.printf("NAME: %s\n", nameChar->readValue().c_str());
    }
  }

  // Get Device Information Service
  NimBLERemoteService *devInfoSvc = client->getService(DEVICE_INFO_UUID);
  if (devInfoSvc)
  {
    NimBLERemoteCharacteristic *manufChar = devInfoSvc->getCharacteristic(MANUFACTURER_NAME_UUID);
    if (manufChar && manufChar->canRead())
    {
      Serial.printf("Manufacturer: %s\n", manufChar->readValue().c_str());
      tft.printf("MANU: %s\n", manufChar->readValue().c_str());
    }

    NimBLERemoteCharacteristic *pnpChar = devInfoSvc->getCharacteristic(PNP_ID_UUID);
    if (pnpChar && pnpChar->canRead())
    {
      NimBLEAttValue val = pnpChar->readValue();
      if (val.size() >= 7)
      {
        const uint8_t *data = val.data();
        uint16_t vid = data[1] | (data[2] << 8);
        uint16_t pid = data[3] | (data[4] << 8);
        uint16_t ver = data[5] | (data[6] << 8);
        Serial.printf("VID: 0x%04X, PID: 0x%04X, Version: 0x%04X\n", vid, pid, ver);
        tft.printf("VID: 0x%04X, PID: 0x%04X, VER: 0x%04X\n", vid, pid, ver);
      }
    }
  }

  // Get Battery Level
  NimBLERemoteService *battSvc = client->getService(BATTERY_SERVICE_UUID);
  if (battSvc)
  {
    NimBLERemoteCharacteristic *battChar = battSvc->getCharacteristic(BATTERY_LEVEL_UUID);
    if (battChar && battChar->canRead())
    {
      uint8_t level = battChar->readValue<uint8_t>();
      Serial.printf("Battery: %d%%\n", level);
      tft.printf("BATT: %d%%\n", level);

      if (battChar->canNotify())
        battChar->subscribe(true, batteryNotifyCallback);
    }
  }

  // Get HID Information
  NimBLERemoteService *hidSvc = client->getService(HID_SERVICE_UUID);
  if (hidSvc)
  {
    NimBLERemoteCharacteristic *hidInfoChar = hidSvc->getCharacteristic(HID_INFO_UUID);
    if (hidInfoChar && hidInfoChar->canRead())
    {
      NimBLEAttValue val = hidInfoChar->readValue();
      if (val.size() >= 4)
      {
        const uint8_t *data = val.data();
        Serial.printf("HID Version: %d.%d, Country: %d, Flags: 0x%02X\n",
                      data[0], data[1], data[2], data[3]);
      }
    }

    // Print Report Map
    NimBLERemoteCharacteristic *reportMapChar = hidSvc->getCharacteristic(HID_REPORT_MAP_UUID);
    if (reportMapChar && reportMapChar->canRead())
    {
      NimBLEAttValue val = reportMapChar->readValue();
      Serial.printf("Report Map Length: %d bytes\n", val.size());

      // Store the report map for USB HID
      if (reportMapData)
        delete[] reportMapData;

      reportMapSize = val.size();
      reportMapData = new uint8_t[reportMapSize];
      memcpy(reportMapData, val.data(), reportMapSize);

      // Print the report map in hex for debugging
      Serial.println("Report Map (hex):");
      for (size_t i = 0; i < reportMapSize; i++)
      {
        Serial.printf("%02X ", reportMapData[i]);
        if ((i + 1) % 16 == 0)
          Serial.println();
      }
      if (reportMapSize % 16 != 0)
        Serial.println();
    }
  }
  Serial.println("=========================================\n");
}

void subscribeToReports(NimBLEClient *client)
{
  NimBLERemoteService *hidSvc = client->getService(HID_SERVICE_UUID);
  if (!hidSvc)
  {
    Serial.println("HID Service not found!");
    return;
  }

  // Get all characteristics and subscribe to report characteristics
  std::vector<NimBLERemoteCharacteristic *> chars = hidSvc->getCharacteristics(true);
  int reportCount = 0;

  for (auto chr : chars)
  {
    if (chr->getUUID() == HID_REPORT_UUID)
    {
      if (chr->canNotify() || chr->canIndicate())
      {
        if (chr->subscribe(true, notifyCallback))
        {
          reportCount++;
          Serial.printf("Subscribed to Report characteristic (handle: 0x%04X)\n",
                        chr->getHandle());
        }
      }
    }
  }

  Serial.printf("Subscribed to %d HID Report(s)\n", reportCount);
}

// Simple HID report map parser (basic implementation)
HIDReportInfo parseReportMap(const uint8_t *data, size_t len)
{
  HIDReportInfo info;

  // Basic detection based on usage pages
  // Usage Page 0x01 = Generic Desktop (mouse, keyboard)
  // Usage Page 0x0C = Consumer devices
  for (size_t i = 0; i < len - 1; i++)
  {
    // Look for Usage Page markers
    if (data[i] == 0x05)
    {
      // Usage Page
      uint8_t page = data[i + 1];
      if (page == 0x01)
      {
        // Check for keyboard (Usage 0x06) or mouse (Usage 0x02)
        for (size_t j = i; j < len - 1 && j < i + 20; j++)
        {
          if (data[j] == 0x09)
          {
            // Usage
            if (data[j + 1] == 0x06)
              info.hasKeyboard = true;
            else if (data[j + 1] == 0x02)
              info.hasMouse = true;
          }
        }
      }
      else if (page == 0x0C)
        info.hasConsumer = true;
    }
  }

  return info;
}

void connectToDevice()
{
  if (advDevice == nullptr)
    return;

  Serial.printf("\nConnecting to: %s\n", advDevice->getAddress().toString().c_str());

  clearDisplay();
  tft.setTextColor(TFT_MAGENTA);
  tft.drawCentreString("CONNECTING", tft.width() / 2, tft.height() / 2, 1);
  tft.setTextColor(TFT_WHITE);

  pClient = NimBLEDevice::createClient();
  pClient->setClientCallbacks(&clientCallbacks);

  // Set connection parameters
  pClient->setConnectionParams(12, 12, 0, 150);

  if (!pClient->connect(advDevice))
  {
    Serial.println("Connection failed!");

    clearDisplay();
    tft.setTextColor(TFT_RED);
    tft.drawCentreString("CONNECTION FAILED", tft.width() / 2, tft.height() / 2, 1);
    tft.setTextColor(TFT_WHITE);

    NimBLEDevice::deleteClient(pClient);
    pClient = nullptr;
    delete advDevice;
    advDevice = nullptr;
    return;
  }

  Serial.println("Connected, discovering services...");

  // Wait for bonding/encryption
  delay(500);

  // Initiate security/bonding
  if (!pClient->secureConnection())
    Serial.println("Security setup failed, continuing anyway...");

  // Print device information
  printDeviceInfo(pClient);

  // Initialize USB HID with the report descriptor from the BLE device
  if (reportMapData && reportMapSize > 0)
  {
    Serial.println("Initializing USB HID with device report map...");

    proxyDevice.setDescriptor(reportMapData, reportMapSize);

    bool deviceAdded = USBHID::addDevice(&proxyDevice, reportMapSize);
    Serial.printf("addDevice returned: %d\n", deviceAdded);

    HID.begin();
    Serial.println("HID.begin() called");

    USB.begin();
    Serial.println("USB.begin() called");

    // Give USB time to enumerate
    delay(1000);

    bool hidReady = HID.ready();
    Serial.printf("HID.ready() = %d\n", hidReady);

    usbReady = true;

    Serial.println("USB HID initialized!");

    tft.setTextColor(TFT_GREEN);
    tft.println("USB HID READY");
    tft.setTextColor(TFT_WHITE);
  }
  else
  {
    Serial.println("ERROR: No report map data available!");
  }

  // Subscribe to HID reports
  subscribeToReports(pClient);

  delete advDevice;
  advDevice = nullptr;
}

void startScan()
{
  Serial.println("\n=== Starting BLE Scan ===");

  clearDisplay();
  tft.setTextColor(TFT_MAGENTA);
  tft.println("SCANNING...");
  tft.setTextColor(TFT_WHITE);

  if (advDevice)
  {
    delete advDevice;
    advDevice = nullptr;
  }
  doConnect = false;

  NimBLEScan *pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCallbacks);
  pScan->setActiveScan(true);
  pScan->setInterval(80);
  pScan->setWindow(48);
  pScan->setDuplicateFilter(true);
  pScan->start(SCAN_DURATION);
}

void setup()
{
  Serial.begin(115200);
  delay(3000);

  Serial.println("--- BOOT START ---");

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(0, 0);
  tft.setTextDatum(MC_DATUM);
  tft.setSwapBytes(true);

  Serial.println("TFT Initialized");

  tft.drawCentreString("BLE HID Proxy", tft.width() / 2, tft.height() / 2, 2);
  Serial.println("BLE HID Proxy");

  // Initialize NimBLE
  NimBLEDevice::init("ESP_HID_Proxy");

  // Set security parameters
  NimBLEDevice::setSecurityAuth(true, true, true); // bonding, MITM, SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  // Set MTU
  NimBLEDevice::setMTU(517);

  Serial.printf("Device Address: %s\n", NimBLEDevice::getAddress().toString().c_str());

  // Start scanning
  startScan();
}

void loop()
{
  if (doConnect)
  {
    doConnect = false;
    connectToDevice();
  }

  // If disconnected and not scanning, restart scan
  if (!deviceConnected && !NimBLEDevice::getScan()->isScanning() && pClient == nullptr)
  {
    delay(2000);
    startScan();
  }

  delay(10);
}