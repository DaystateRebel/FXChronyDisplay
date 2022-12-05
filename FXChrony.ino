#include <U8g2lib.h>
#include "BLEDevice.h"
#include "OneButton.h"
#include <EEPROM.h>

#define EEPROM_SIZE 5

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

#if defined(ARDUINO_heltec_wifi_kit_32)
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ 15, /* data=*/ 4, /* reset=*/ 16);
#endif

#if defined(ARDUINO_LOLIN32)
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ 4, /* data=*/ 5, /* reset=*/ U8X8_PIN_NONE);
#endif

// HELTEC WiFi 32 kit has a button on GPIO0
#define PIN_INPUT 0

#define PIN_LED 25

OneButton button(PIN_INPUT, true);

#define UNITS_IMPERIAL  0
#define UNITS_METRIC    1
#define UNITS_MAX       1

#define PROFILE_BOW_AIRSOFT 0
#define PROFILE_CO2_PISTOL  1
#define PROFILE_AIR_PISTOL  2
#define PROFILE_AIR_GUN_UK  3
#define PROFILE_AIR_GUN_FAC 4
#define PROFILE_MAX         4

#define DISPLAY_FLIP_OFF    0
#define DISPLAY_FLIP_ON     1
#define DISPLAY_FLIP_MAX    1

#define SENSITIVITY_MAX     100

#define STATE_IDLE          0
#define STATE_CONNECTING    1
#define STATE_CONNECTED     2
#define STATE_READY         3

#define seconds() (millis()/1000)

static uint8_t state = STATE_IDLE;

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

static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;
static BLEClient*  pClient;

static bool renderMenu = false;
static bool dirty = false;
static bool profile_changed = false;
static bool power_saving = false;
static unsigned long display_on_at = 0;
static uint8_t searching_ctr = 0;

static uint8_t sensitivity = 50;
static uint8_t units = UNITS_IMPERIAL;
static uint8_t profile = PROFILE_AIR_GUN_FAC;
static uint8_t display_flip = 0;
static uint8_t power_save_duration = 0;

static uint32_t shot_count = 0;

typedef  void (* menuItemCallback_t)(uint8_t);
typedef  void (* menuItemGenString_t)(char *);

typedef struct menuItem {
    const char * menuString;
    menuItemGenString_t menuItemGenString;
    struct menuItem * nextMenuItem;
    struct menuItem * currentSubMenu;
    struct menuItem * subMenu;
    menuItemCallback_t menuItemCallback;
    uint8_t param;
    menuItemGenString_t menuItemGenCurSelString;
} menuItem_t;

static menuItem_t * menuStack[4];
static int menuStackIndex;
static menuItem_t * pCurrentMenuItem;

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Open Display...");

  button.attachDoubleClick(doubleClick);
  button.attachLongPressStop(longPressStop);
  button.attachClick(singleClick);
  
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

/*
 * Read the Settings, if any setting is out of range
 * give it a sensible default value (cope with first use)
 */
  EEPROM.begin(EEPROM_SIZE);
  sensitivity = EEPROM.read(0);
  if(sensitivity > SENSITIVITY_MAX) {
    sensitivity = 30;
    EEPROM.write(0, sensitivity);    
  }
  units = EEPROM.read(1);
  if(units > UNITS_MAX) {
    units = UNITS_MAX;
    EEPROM.write(1, units);
  }
  profile = EEPROM.read(2);
  if(profile > PROFILE_MAX) {
    profile = PROFILE_MAX;
    EEPROM.write(2, profile);
  }
  display_flip = EEPROM.read(3);
  if(display_flip > DISPLAY_FLIP_MAX) {
    display_flip = 0;
    EEPROM.write(3, display_flip);
  }
  power_save_duration = EEPROM.read(4);
  if(power_save_duration > 10) {
    power_save_duration = 10;
    EEPROM.write(4, power_save_duration);
  }
  EEPROM.commit();
 
  BLEDevice::init("");
 
  u8g2.begin();
  u8g2.setFlipMode(display_flip);
  dirty = true;
}

void doRenderMenu() {
    char genHeader[16];
    u8g2.clearBuffer();					        // clear the internal memory
    u8g2.setFont(u8g2_font_crox4h_tf);	// 14pt

    const char * pHeadertxt;
    if(pCurrentMenuItem->menuItemGenString == NULL) {
      pHeadertxt = pCurrentMenuItem->menuString;
    } else {
        pHeadertxt = genHeader;
        pCurrentMenuItem->menuItemGenString(genHeader);
    }

    u8g2_uint_t hdr_w = u8g2.getStrWidth(pHeadertxt);
    u8g2.drawStr((128-hdr_w)/2, 16, pHeadertxt);	

    const char * psubHeadertxt = pCurrentMenuItem->currentSubMenu->menuString;
    if(psubHeadertxt == NULL) {
        psubHeadertxt = genHeader;
        pCurrentMenuItem->currentSubMenu->menuItemGenString(genHeader);
    }

    hdr_w = u8g2.getStrWidth(psubHeadertxt);
    u8g2.drawStr((128-hdr_w)/2, 40, psubHeadertxt);

    u8g2.setFont(u8g2_font_t0_14_tf);	// 9pt

    const char * pcurSelHeadertxt;
    if(pCurrentMenuItem->currentSubMenu->menuItemGenCurSelString) {
        pcurSelHeadertxt = genHeader;
        pCurrentMenuItem->currentSubMenu->menuItemGenCurSelString(genHeader);

        hdr_w = u8g2.getStrWidth(pcurSelHeadertxt);
        u8g2.drawStr((128-hdr_w)/2, 60, pcurSelHeadertxt);
        
      }

    u8g2.sendBuffer();
}

#define STR_SEARCHING "Searching"

void renderSearching() {
  char temp_str[16];
  u8g2_uint_t w;
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_crox4h_tf);	// choose a suitable font
  strcpy(temp_str,STR_SEARCHING);
  for(uint8_t i = 0; i < searching_ctr; i++) {
    strcat(temp_str,".");
  }      
  w = u8g2.getStrWidth(STR_SEARCHING);
  u8g2.drawStr((128-w)/2, 40, temp_str);
  u8g2.sendBuffer();
  searching_ctr++;
  searching_ctr %= 4;
}

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
 /**
   * Called for each advertising BLE server.
   */
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    Serial.println(advertisedDevice.toString().c_str());
    // We have found a device, let us now see if it contains the service we are looking for.
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(deviceUUID)) {
      Serial.print("Found Chrony");
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      state = STATE_CONNECTING;
    } // Found our server
  } // onResult
}; // MyAdvertisedDeviceCallbacks

void do_scan() {
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);
}

bool writeChar(BLERemoteService* pRemoteService, int idx, uint8_t value)
{
  // Obtain a reference to the characteristic in the service of the remote BLE server.
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUIDs[idx]);
  if (pRemoteCharacteristic == nullptr) {
    Serial.print("Failed to find our characteristic UUID: ");
    Serial.println(charUUIDs[idx]);
    return false;
  }

  // Read the value of the characteristic.
  if(pRemoteCharacteristic->canWrite()) {
    pRemoteCharacteristic->writeValue(&value, 1);
  }
  Serial.printf(" - Wrote to characteristic %s value %X OK", charUUIDs[idx], value);

  return true;
}

bool readChar(BLERemoteService* pRemoteService, int idx, uint8_t * value)
{
  // Obtain a reference to the characteristic in the service of the remote BLE server.
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUIDs[idx]);
  if (pRemoteCharacteristic == nullptr) {
    Serial.print("Failed to find our characteristic UUID: ");
    Serial.println(charUUIDs[idx]);
    return false;
  }

  // Read the value of the characteristic.
  if(pRemoteCharacteristic->canRead()) {
    std::string v = pRemoteCharacteristic->readValue();
    *value = v[0];
  }
  Serial.println(" - Read from characteristic OK");
  return true;
}

bool readBattery(BLERemoteService* pRemoteService){
  uint8_t vb;
  if(!readChar(pRemoteService, 3, &vb)) {
    return false;
  }
  float vBattery = (vb * 20.0)/1000;
  Serial.printf(" - Battery %f v\n",vBattery);
  return true;  
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
  }

  void onDisconnect(BLEClient* pclient) {
    state = STATE_IDLE;
    dirty = true;
    renderMenu = false;
    Serial.println("onDisconnect");
  }
};


static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {
  char sbuffer[16];

  //Serial.print("Notify callback for characteristic ");
  // Third byte is return in steps of 5%, anything better than 10% we display  
  uint8_t r = ((char*)pData)[2] * 5;
  uint16_t speed;    
  if((r >= sensitivity) && !renderMenu) {
    display_on_at = seconds();
    u8g2.setPowerSave(0);
    power_saving = false;

    renderMenu = false;
    shot_count++;    
    Serial.print(pBLERemoteCharacteristic->getUUID().toString().c_str());
    Serial.print(" of data length ");
    Serial.println(length);
    Serial.print("data: ");
    Serial.printf("%02X %02X %02X\n",((char*)pData)[0],((char*)pData)[1],((char*)pData)[2]);

    speed = ((char*)pData)[0];
    speed <<= 8;
    speed |= ((char*)pData)[1];

    float fspeed = speed;

    if(units == UNITS_IMPERIAL) {
      fspeed *= 0.0475111859;
      sprintf (sbuffer, "%d FPS", int(fspeed));
    } else {
      fspeed *= 0.014481409;
      sprintf (sbuffer, "%d M/S", int(fspeed));
    }
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_VCR_OSD_tr);
    u8g2_uint_t w = u8g2.getStrWidth(sbuffer);
    u8g2.drawStr((128-w)/2, 27, sbuffer);
    
    u8g2.setFont(u8g2_font_t0_14_tf);	// 9pt
    sprintf (sbuffer, "Shot# %d", shot_count);
    w = u8g2.getStrWidth(sbuffer);
    u8g2.drawStr((128-w)/2, 45, sbuffer);

    u8g2.setFont(u8g2_font_t0_14_tf);	// 9pt
    sprintf (sbuffer, "Return %d%%", r);
    w = u8g2.getStrWidth(sbuffer);
    u8g2.drawStr((128-w)/2, 60, sbuffer);

    u8g2.sendBuffer();					
  }
}

uint8_t profile_bytes[2][5] = {{0x32, 0x32, 0x32, 0x32, 0x64},{0x17, 0x1E, 0x2D, 0x43, 0x5A}};

void connectToChrony() {
  Serial.print("Forming a connection to ");
  Serial.println(myDevice->getAddress().toString().c_str());
  
  pClient  = BLEDevice::createClient();
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
    dirty = true;
    state = STATE_IDLE;
    return;
  }
  Serial.println(" - Found our service");

  if(!writeChar(pRemoteService, 2, profile_bytes[0][profile]))
  {
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }
  if(!writeChar(pRemoteService, 4, profile_bytes[1][profile]))
  {
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }
  Serial.println(" - Profile Set");

  if(!readBattery(pRemoteService)){
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }


  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUIDs[0]);
  if (pRemoteCharacteristic == nullptr) {
    Serial.print("Failed to find our characteristic UUID: ");
    Serial.println(charUUIDs[0]);
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }
  Serial.println(" - Found our speed data char");

  if(pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
  } else {
    Serial.println(" - But Cannot Notify?!?!");
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;      
  }
  state = STATE_CONNECTED;    
  u8g2.clearBuffer();					// clear the internal memory
  u8g2.sendBuffer();					// transfer internal memory to the display
  u8g2.setPowerSave(1);
  power_saving = true;
  digitalWrite(PIN_LED, HIGH);
}

void loop() {
  unsigned long now = seconds();  

  if( !power_saving && 
      !renderMenu && 
      state != STATE_IDLE && 
      (power_save_duration != 0) &&
      (display_on_at + power_save_duration) < now) 
  {
    display_on_at = seconds();
    u8g2.setPowerSave(1);
    power_saving = true;
  }


  button.tick();
  if(dirty) {
    if(power_saving) {
      display_on_at = seconds();
      u8g2.setPowerSave(0);
      power_saving = false;
    }
    if(renderMenu){
      doRenderMenu();
    }
    dirty = false;
  }

  switch(state)
  {
    case STATE_IDLE:
      digitalWrite(PIN_LED, LOW);
      if(!dirty) { 
        renderSearching();
        do_scan(); 
      }
      break;
    case STATE_CONNECTING:
      connectToChrony();
      break;
    case STATE_CONNECTED:
      break;
    case STATE_READY:
      break;
  }
}


/* 
  Menu
  Profile       bow/airsoft   CO2 Pistol    Air Pistol    Air Gun UK    Air Gun FAC
  Sensitivity   0-100%
  Units         FPS M/S
  Display flip  on/off 
*/

static const uint8_t power_save_duration_lut[] = {0,2,5,10};

static void powerSaveCallback(uint8_t param)
{
  if(power_save_duration != power_save_duration_lut[param]) {
    power_save_duration = power_save_duration_lut[param];
    EEPROM.write(4, power_save_duration);
    EEPROM.commit();
  }  
  Serial.printf("Menu Item Selected PWR SAVE %d\n", power_save_duration);
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_power_save[] = {
  { "Off",  NULL, &menu_power_save[1], NULL, NULL, powerSaveCallback, 0, NULL},
  { "2s",   NULL, &menu_power_save[2], NULL, NULL, powerSaveCallback, 1, NULL},
  { "5s",   NULL, &menu_power_save[3], NULL, NULL, powerSaveCallback, 2, NULL},
  { "10s",  NULL, &menu_power_save[0], NULL, NULL, powerSaveCallback, 3, NULL}
};

void menuItemGenStringCurPowerSaving(char * buffer)
{
  uint8_t idx = 0;
  switch(power_save_duration)
  {
    case 2:  idx = 1;  break;
    case 5:  idx = 2;  break;
    case 10: idx = 3;  break;
  }  
  sprintf(buffer, "[%s]", menu_power_save[idx].menuString);
  Serial.println(buffer);  
}

static void displayFlipCallback(uint8_t param)
{
  display_flip = param;
  EEPROM.write(3, display_flip);
  EEPROM.commit();
  Serial.printf("Menu Item Selected DISPLAY_FLIP %d\n", display_flip);
  u8g2.setFlipMode(display_flip);
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_display_flip[] = {
  { "Off",         NULL, &menu_display_flip[1], NULL, NULL, displayFlipCallback, DISPLAY_FLIP_OFF, NULL},
  { "On",          NULL, &menu_display_flip[0], NULL, NULL, displayFlipCallback, DISPLAY_FLIP_ON, NULL}
};

void menuItemGenStringCurDisplayFlip(char * buffer)
{
  sprintf(buffer, "[%s]", menu_display_flip[display_flip].menuString);
  Serial.println(buffer);  
}

static void unitsCallback(uint8_t param)
{
  units = param;
  EEPROM.write(1, units);
  EEPROM.commit();
  Serial.printf("Menu Item Selected UNITS %d\n", units);
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_units[] = {
  { "FPS",          NULL, &menu_units[1], NULL, NULL, unitsCallback, UNITS_IMPERIAL, NULL},
  { "M/S",          NULL, &menu_units[0], NULL, NULL, unitsCallback, UNITS_METRIC, NULL}
};

void menuItemGenStringCurSelUnits(char * buffer)
{
  sprintf(buffer, "[%s]", menu_units[units].menuString);
  Serial.println(buffer);  
}

static void profileCallback(uint8_t param)
{
  if(profile != param) {
    profile = param;
    EEPROM.write(2, profile);
    EEPROM.commit();
    profile_changed = true;
  }  
  Serial.printf("Menu Item Selected PROFILE %d\n", profile);
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_profile[] = {
  { "Bow/Airsoft",  NULL, &menu_profile[1], NULL, NULL, profileCallback, PROFILE_BOW_AIRSOFT, NULL},
  { "CO2 Pistol",   NULL, &menu_profile[2], NULL, NULL, profileCallback, PROFILE_CO2_PISTOL, NULL},
  { "Air Pistol",   NULL, &menu_profile[3], NULL, NULL, profileCallback, PROFILE_AIR_PISTOL, NULL},
  { "Air Gun UK",   NULL, &menu_profile[4], NULL, NULL, profileCallback, PROFILE_AIR_GUN_UK, NULL},
  { "Air Gun FAC",  NULL, &menu_profile[0], NULL, NULL, profileCallback, PROFILE_AIR_GUN_FAC, NULL}
};

void menuItemGenStringCurSelProfile(char * buffer)
{
  sprintf(buffer, "[%s]", menu_profile[profile].menuString);
  Serial.println(buffer);  
}

static void sensitivityIncCallback(uint8_t param)
{
  if(sensitivity <= 95){ 
    sensitivity += 5;
    EEPROM.write(0, sensitivity);
    EEPROM.commit();
    Serial.printf("Menu Item Selected SENSITIVITY INC %d\n", sensitivity);
  }
}
static void sensitivityDecCallback(uint8_t param)
{ 
  if(sensitivity >= 5){
    sensitivity -= 5;
    EEPROM.write(0, sensitivity);
    EEPROM.commit();
    Serial.printf("Menu Item Selected SENSITIVITY DEC %d\n", sensitivity);
  }
}

static menuItem_t menu_sensitivity[] = {
  { "Increase",     NULL, &menu_sensitivity[1], NULL, NULL, sensitivityIncCallback, 0, NULL},
  { "Decrease",     NULL, &menu_sensitivity[0], NULL, NULL, sensitivityDecCallback, 0, NULL}
};

void menuItemGenStringSensitivity(char * buffer)
{
  sprintf(buffer, "%d %%", sensitivity);
  Serial.println(buffer);  
}

void menuItemGenStringCurSelSensitivity(char * buffer)
{
  sprintf(buffer, "[%d %%]", sensitivity);
  Serial.println(buffer);  
}


static menuItem_t menu_top_level[] = {
  { "Profile",      NULL, &menu_top_level[1], menu_profile,     menu_profile,     NULL, 0, menuItemGenStringCurSelProfile},
  { "Min. Return", menuItemGenStringSensitivity, &menu_top_level[2], menu_sensitivity, menu_sensitivity, NULL, 0, menuItemGenStringCurSelSensitivity},
  { "Units",        NULL, &menu_top_level[3], menu_units,       menu_units,       NULL, 0, menuItemGenStringCurSelUnits},
  { "Display Flip", NULL, &menu_top_level[4], menu_display_flip,menu_display_flip,NULL, 0, menuItemGenStringCurDisplayFlip},
  { "Power Save",   NULL, &menu_top_level[0], menu_power_save, menu_power_save,  NULL, 0, menuItemGenStringCurPowerSaving}
  
};

static menuItem_t menu_entry = {  "Settings", NULL, menu_top_level, menu_top_level, NULL, NULL, 0, NULL };

void singleClick()
{
  Serial.println("x1");
  if(renderMenu)
  {
    dirty = true;
    pCurrentMenuItem->currentSubMenu = pCurrentMenuItem->currentSubMenu->nextMenuItem;
  } else {
    if(power_saving) {
      display_on_at = seconds();
      u8g2.setPowerSave(0);
      power_saving = false;
    }
  }
}

void doubleClick()
{
  if(renderMenu)
  {
      dirty = true;
      if(pCurrentMenuItem->currentSubMenu->menuItemCallback != NULL)
      {
          (*pCurrentMenuItem->currentSubMenu->menuItemCallback)(pCurrentMenuItem->currentSubMenu->param);
      } else {
          menuStack[menuStackIndex++] = pCurrentMenuItem;
          pCurrentMenuItem = pCurrentMenuItem->currentSubMenu;
      }
  }
  Serial.printf("x2 %d\n",menuStackIndex);
}

void longPressStop()
{
  if(renderMenu) {
    if(menuStackIndex == 1) {
      renderMenu = false;
      u8g2.clearBuffer();					// clear the internal memory
      u8g2.sendBuffer();					// transfer internal memory to the display
      if(profile_changed) {
        pClient->disconnect();
        state = STATE_IDLE;
      }
    } else {
      dirty = true;
      pCurrentMenuItem = menuStack[--menuStackIndex];
    }
  } else if(state == STATE_CONNECTED) {
    profile_changed = false;
    dirty = true;
    renderMenu = true;
    pCurrentMenuItem = &menu_entry;
    menuStackIndex = 0;
    menuStack[menuStackIndex++] = pCurrentMenuItem;
  }
  Serial.printf("LPS %s %d\n",renderMenu ? "Menu On" : "Menu Off", menuStackIndex);
}
