/**
 * A BLE client example that is rich in capabilities.
 * There is a lot new capabilities implemented.
 * author unknown
 * updated by chegewara
 */

#include "BLEDevice.h"
//#include "BLEScan.h"
#include <U8g2lib.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif
// Setup teh OLED
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ 15, /* data=*/ 4, /* reset=*/ 16);

// The remote service we wish to connect to.
static BLEUUID deviceUUID("0000180a-0000-1000-8000-00805f9b34fb");

static BLEUUID serviceUUID("00001623-88EC-688C-644B-3FA706C0BB75");

// The characteristic of the remote service we are interested in.
static const char * charUUIDs[] = {
  "00001624-88EC-688C-644B-3FA706C0BB75",
  "00001625-88EC-688C-644B-3FA706C0BB75",
  "00001626-88EC-688C-644B-3FA706C0BB75",
  "00001627-88EC-688C-644B-3FA706C0BB75",
  "00001628-88EC-688C-644B-3FA706C0BB75",
  "00001629-88EC-688C-644B-3FA706C0BB75",
  "0000162A-88EC-688C-644B-3FA706C0BB75"
};

static boolean doConnect = false;
static boolean connected = false;
static boolean doScan = false;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;

static int cbctr = 0;

static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {
  char sbuffer[16];
  // Third byte is return in steps of 5%, anything better than 10% we display  
  if(((char*)pData)[2] > 2) {    
    Serial.print("Notify callback for characteristic ");
    Serial.print(pBLERemoteCharacteristic->getUUID().toString().c_str());
    Serial.print(" of data length ");
    Serial.println(length);
    Serial.print("data: ");
    Serial.printf("%02X %02X %02X\n",((char*)pData)[0],((char*)pData)[1],((char*)pData)[2]);

    float speed = ((char*)pData)[1];
    speed /= 100.0;
    speed += ((char*)pData)[0];
    speed *= 3.708;   // M/S
    speed *= 3.28084; // FPS
    sprintf (sbuffer, "%3d FPS", int(speed));

    cbctr = 0;
    u8g2.clearBuffer();					// clear the internal memory
    u8g2.setFont(u8g2_font_VCR_OSD_tr);	// choose a suitable font
    u8g2.drawStr(0,48,sbuffer);	// write something to the internal memory
    u8g2.sendBuffer();					// transfer internal memory to the display
  } else {
    cbctr++;
  }

  if(cbctr == 10) {
    u8g2.clearBuffer();					// clear the internal memory
    u8g2.setFont(u8g2_font_VCR_OSD_tr);	// choose a suitable font
    u8g2.drawStr(0,48,"--- FPS");	// write something to the internal memory
    u8g2.sendBuffer();					// transfer internal memory to the display
  }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
  }

  void onDisconnect(BLEClient* pclient) {
    connected = false;
    Serial.println("onDisconnect");
  }
};

bool writeChar(BLERemoteService* pRemoteService, int idx, uint8_t value)
{
  // Obtain a reference to the characteristic in the service of the remote BLE server.
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUIDs[idx]);
  if (pRemoteCharacteristic == nullptr) {
    Serial.print("Failed to find our characteristic UUID: ");
    Serial.println(charUUIDs[idx]);
    return false;
  }
  Serial.println(" - Found our characteristic");

  // Read the value of the characteristic.
  if(pRemoteCharacteristic->canWrite()) {
    pRemoteCharacteristic->writeValue(&value, 1);
  }
  return true;
}


bool connectToServer() {
    Serial.print("Forming a connection to ");
    Serial.println(myDevice->getAddress().toString().c_str());
    
    BLEClient*  pClient  = BLEDevice::createClient();
    Serial.println(" - Created client");

    pClient->setClientCallbacks(new MyClientCallback());

    // Connect to the remove BLE Server.
    pClient->connect(myDevice);  // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
    Serial.println(" - Connected to server");
    pClient->setMTU(517); //set client to request maximum MTU from server (default is 23 otherwise)
  
    // Obtain a reference to the service we are after in the remote BLE server.
    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == nullptr) {
      Serial.print("Failed to find our service UUID: ");
      Serial.println(serviceUUID.toString().c_str());
      pClient->disconnect();
      return false;
    }
    Serial.println(" - Found our service");

    /*
      Setup, write these two values depending on gun type
      bow/airsoft
      W: UUID: 00001626 32 
      W: UUID: 00001628 17 
      CO2 Pistol
      W: UUID: 00001626 32 
      W: UUID: 00001628 1E
      Air Pistol
      W: UUID: 00001626 32 
      W: UUID: 00001628 2D
      Air gun UK
      W: UUID: 00001626 32 
      W: UUID: 00001628 43
      Air gun High Power
      W: UUID: 00001626 64
      W: UUID: 00001628 5A

      Read UUID: 00001627 for battery voltage as mV / 20 (0xE1 or 225 is exactly 4.5V)
    */
    /* Setup for Air gun High Power */
    if(!writeChar(pRemoteService, 2, 0x64))
    {
      pClient->disconnect();
      return false;
    }
    if(!writeChar(pRemoteService, 4, 0x5A))
    {
      pClient->disconnect();
      return false;
    }


    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUIDs[0]);
    if (pRemoteCharacteristic == nullptr) {
      Serial.print("Failed to find our characteristic UUID: ");
      Serial.println(charUUIDs[0]);
      pClient->disconnect();
      return false;
    }
    Serial.println(" - Found our characteristic");

    if(pRemoteCharacteristic->canNotify())
      pRemoteCharacteristic->registerForNotify(notifyCallback);

    connected = true;
    return true;
}
/**
 * Scan for BLE servers and find the first one that advertises the service we are looking for.
 */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
 /**
   * Called for each advertising BLE server.
   */
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    Serial.print("BLE Advertised Device found: ");
    Serial.println(advertisedDevice.toString().c_str());

    // We have found a device, let us now see if it contains the service we are looking for.
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(deviceUUID)) {

      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = true;

    } // Found our server
  } // onResult
}; // MyAdvertisedDeviceCallbacks


void setup() {
  Serial.begin(115200);
  Serial.println("Starting Arduino BLE Client application...");

  u8g2.begin();
  u8g2.clearBuffer();					// clear the internal memory
  u8g2.setFont(u8g2_font_VCR_OSD_tr);	// choose a suitable font
  u8g2.drawStr(0,48,"Searching");	// write something to the internal memory
  u8g2.sendBuffer();					// transfer internal memory to the display

  BLEDevice::init("");

  // Retrieve a Scanner and set the callback we want to use to be informed when we
  // have detected a new device.  Specify that we want active scanning and start the
  // scan to run for 5 seconds.
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);
} // End of setup.


// This is the Arduino main loop function.
void loop() {

  // If the flag "doConnect" is true then we have scanned for and found the desired
  // BLE Server with which we wish to connect.  Now we connect to it.  Once we are 
  // connected we set the connected flag to be true.
  if (doConnect == true) {

    if (connectToServer()) {
      u8g2.clearBuffer();					// clear the internal memory
      u8g2.setFont(u8g2_font_VCR_OSD_tr);	// choose a suitable font
      u8g2.drawStr(0,48,"Connected");	// write something to the internal memory
      u8g2.sendBuffer();					// transfer internal memory to the display
    }
    doConnect = false;
  }

  // If we are connected to a peer BLE Server, update the characteristic each time we are reached
  // with the current time since boot.
  if (connected) {
    String newValue = "Time since boot: " + String(millis()/1000);
  }else if(doScan){
  }

  
  delay(1000); // Delay a second between loops.
} // End of loop
