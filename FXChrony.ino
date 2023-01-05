#include "BLEDevice.h"
#include "OneButton.h"
#include <EEPROM.h>
#include <esp_sleep.h>
#include "esp_adc_cal.h"

/* Don't mess with this bit unless 
   you know what you're doing */

typedef struct gun {
  const char * gun_name;
  const char * gun_mfr;
  float gun_caliber_inch;
  float gun_caliber_mm;
  uint8_t gun_profile;
  uint8_t shot_string_length;
} gun_t;

typedef struct pellet {
  const char * pellet_name;
  const char * pellet_mfr;
  float pellet_weight_grains;
  float pellet_caliber_inch;
  float pellet_weight_grams;
  float pellet_caliber_mm;
} pellet_t;

#define NUM_PELLETS (sizeof(my_pellets)/sizeof(pellet_t))
#define NUM_GUNS    (sizeof(my_guns)/sizeof(gun_t))         // Lots!

#define MAX_SHOT_STRING_LENGTH  20
// [0] = shot string length
// [1] = pellet_idx
// SHOT_STRING_LENGTH * shots
#define EEPROM_PER_GUN (MAX_SHOT_STRING_LENGTH * sizeof(float) + 2)
#define EEPROM_SIZE (6 + EEPROM_PER_GUN * NUM_GUNS)

/************************************************************************************/
/* Add your pellets here !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
/************************************************************************************/
/* Every row EXCEPT the last one must have a comma at the end. 
   
   Format is:
   
Name, Manufacturer, weight in grains, caliber in inches, weight in grams, caliber in mm
*/
pellet_t my_pellets[] = {
  {"Diabolo",         "JSB",  8.44,  0.177, 0.547, 4.5},
  {"Field Tgt Trophy","H&N",  14.66, 0.22,  0.95,  5.5},
  {"Diabolo Monster", "JSB",  13.43, 0.177, 0.87,  4.5},
  {"King Hvy MkII",   "JSB",  33.95, 0.25,  2.2,   6.35},
  {"Slug",            "NSA",  33.5,  0.25,  0,     0}
};
/************************************************************************************/

#define PROFILE_BOW_AIRSOFT 0
#define PROFILE_CO2_PISTOL  1
#define PROFILE_AIR_PISTOL  2
#define PROFILE_AIR_GUN_UK  3
#define PROFILE_AIR_GUN_FAC 4

/************************************************************************************/
/* Add your guns here !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
/************************************************************************************/
/* Every row EXCEPT the last one must have a comma at the end. 
   
   Format is:
   
Name, Manufacturer, caliber in inches, caliber in mm, profile (speed range)
*/
gun_t my_guns[] = {
  {"2240",    "Crosman",  0.22,  5.5, PROFILE_CO2_PISTOL,   20},
  {"Pulsar",  "Daystate", 0.177, 4.5, PROFILE_AIR_GUN_FAC,  10},
  {"Leshiy2", "Ed Gun",   0.25,  6.35,PROFILE_AIR_GUN_FAC,  8},
  {"Red Wolf","Daystate", 0.25,  6.35, PROFILE_AIR_GUN_FAC,  10}
};
/************************************************************************************/

#if defined(ARDUINO_heltec_wifi_kit_32) || defined(ARDUINO_LOLIN32)
#include <U8g2lib.h>
#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif
#endif

#if defined(ARDUINO_heltec_wifi_kit_32)
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ 15, /* data=*/ 4, /* reset=*/ 16);
static esp_adc_cal_characteristics_t adc_chars;
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

#define DISPLAY_FLIP_OFF    0
#define DISPLAY_FLIP_ON     1
#define DISPLAY_FLIP_MAX    1

#define SENSITIVITY_MAX     100

#define STATE_IDLE          0
#define STATE_CONNECTING    1
#define STATE_CONNECTED     2
#define STATE_OFFLINE 3

#define seconds() (millis()/1000)

static uint8_t state = STATE_IDLE;

static float chronyVBattery;
static unsigned long chronyVBattLastRead = 0;

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
static BLERemoteService* pRemoteService;

static bool renderMenu = false;
static bool dirty = false;
static bool profile_changed = false;
static bool power_saving = false;
static unsigned long display_on_at = 0;
static uint8_t searching_ctr = 0;

static uint8_t sensitivity = 50;
static uint8_t units = UNITS_IMPERIAL;
static uint8_t display_flip = 0;
static uint8_t power_save_duration = 0;
static uint8_t gun_index = 0;

static uint32_t shot_count = 0;
static uint8_t nc_counter = 0;

typedef  void (* menuItemCallback_t)(uint8_t);
typedef  void (* menuItemGenString_t)(uint8_t, char *);

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    Serial.println("onConnect");
  }

  void onDisconnect(BLEClient* pclient) {
    Serial.println("onDisconnect");
    /* this make me sad, can't get the code to reliably reconnect 
    more than 4-5 times so here it is ... 
    (On the plus side it seems to work well and I can get 
    on with shot strings :) )*/
    ESP.restart();
    while(1){}
  }
};

typedef struct shot_stats {
  float min;
  float max;
  float avg;
  float es;
  float sd;
} shot_stats_t;

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

static menuItem_t * menu_gun = NULL;
static menuItem_t * menu_pellet = NULL;
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

#if defined(ARDUINO_heltec_wifi_kit_32)
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_11);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, ESP_ADC_CAL_VAL_DEFAULT_VREF, &adc_chars);
  pinMode(21, OUTPUT);
  digitalWrite(21, LOW);
#endif

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
  gun_index = EEPROM.read(2);
  if(gun_index >= NUM_GUNS) {
    gun_index = 0;
    EEPROM.write(2, gun_index);    
  }
  display_flip = EEPROM.read(3);
  if(display_flip > DISPLAY_FLIP_MAX) {
    display_flip = 0;
    EEPROM.write(3, display_flip);
  }
  power_save_duration = EEPROM.read(4);
  if(power_save_duration > 60) {
    power_save_duration = 60;
    EEPROM.write(4, power_save_duration);
  }
 
  EEPROM.commit();
 
  BLEDevice::init("");
  
  build_gun_menu();

  u8g2.begin();
  u8g2.setFlipMode(display_flip);
  dirty = true;
}

uint8_t get_string_length(uint8_t gidx) {
  return EEPROM.read(6 + (gidx * EEPROM_PER_GUN));
}

void clear_string(uint8_t gidx) {
  EEPROM.write(6 + (gidx * EEPROM_PER_GUN), 0);
  EEPROM.commit();
}

float get_shot(uint8_t gidx, uint8_t sidx) {
  float result;
  EEPROM.get(6 + (gidx * EEPROM_PER_GUN) + sidx * sizeof(float) + 2, result);
  return result;
}

void add_shot(uint8_t gidx, float speed) {
  uint8_t sidx = get_string_length(gidx);
  if(sidx >= my_guns[gun_index].shot_string_length) {
    clear_string(gidx);
    sidx = 0;
  }
  EEPROM.put(6 + (gidx * EEPROM_PER_GUN) + sidx * sizeof(float) + 2, speed);
  EEPROM.write(6 + (gidx * EEPROM_PER_GUN), sidx + 1);
  EEPROM.commit();
}

uint8_t get_pellet_index(uint8_t gidx) 
{
  return EEPROM.read(6 + (gidx * EEPROM_PER_GUN) + 1);
}

void set_pellet_index(uint8_t gidx, uint8_t pidx) 
{
  EEPROM.write(6 + (gidx * EEPROM_PER_GUN) + 1, pidx);
  EEPROM.commit();
  get_pellet_index(gidx); 
}


void doRenderMenu() {
    char genHeader[256];
    u8g2.clearBuffer();					        // clear the internal memory
    u8g2.setFont(u8g2_font_crox4h_tf);	// 14pt

    const char * pHeadertxt;
    if(pCurrentMenuItem->menuItemGenString == NULL) {
      pHeadertxt = pCurrentMenuItem->menuString;
    } else {
        pHeadertxt = genHeader;
        pCurrentMenuItem->menuItemGenString(pCurrentMenuItem->currentSubMenu->param, genHeader);
    }

    u8g2_uint_t hdr_w = u8g2.getStrWidth(pHeadertxt);
    u8g2.drawStr((128-hdr_w)/2, 16, pHeadertxt);	

    // Pellet names tend to be long :(
    if(pCurrentMenuItem->subMenu == menu_pellet) {
      u8g2.setFont(u8g2_font_t0_14_tf);	// 9pt
    }
    
    const char * psubHeadertxt = pCurrentMenuItem->currentSubMenu->menuString;
    if(psubHeadertxt == NULL) {
        psubHeadertxt = genHeader;
        pCurrentMenuItem->currentSubMenu->menuItemGenString(pCurrentMenuItem->currentSubMenu->param, genHeader);
    }

    hdr_w = u8g2.getStrWidth(psubHeadertxt);
    u8g2.drawStr((128-hdr_w)/2, 40, psubHeadertxt);

    u8g2.setFont(u8g2_font_t0_14_tf);	// 9pt

    const char * pcurSelHeadertxt;
    if(pCurrentMenuItem->currentSubMenu->menuItemGenCurSelString) {
        pcurSelHeadertxt = genHeader;
        pCurrentMenuItem->currentSubMenu->menuItemGenCurSelString(pCurrentMenuItem->currentSubMenu->param, genHeader);

        hdr_w = u8g2.getStrWidth(pcurSelHeadertxt);
        u8g2.drawStr((128-hdr_w)/2, 60, pcurSelHeadertxt);
        
      }

    u8g2.sendBuffer();
}

#if defined(ARDUINO_heltec_wifi_kit_32)
void renderDeviceVBatt() {
    char temp_str[16];
    uint32_t vbat = esp_adc_cal_raw_to_voltage(analogRead(37), &adc_chars);
    u8g2.setFont(u8g2_font_5x7_mr);	// 6pt
    sprintf (temp_str, "D %.1fV", ((float)vbat) * 0.0025);
    u8g2.drawStr(0, 6, temp_str);
}
#endif

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
#if defined(ARDUINO_heltec_wifi_kit_32)
  renderDeviceVBatt();
#endif
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
  pBLEScan->start(1, false);
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
  return true;
}

void renderChronyVBatt(){
  uint8_t vb;
  char temp_str[16];
  u8g2.setFont(u8g2_font_5x7_mr);	// 6pt
  sprintf (temp_str, "C %.1fV", chronyVBattery);
  u8g2_uint_t w = u8g2.getStrWidth(temp_str);
  u8g2.drawStr(128 - w, 6, temp_str);
}

bool readBattery(){
  uint8_t vb;
  if(!readChar(pRemoteService, 3, &vb)) {
    return false;
  }
  chronyVBattery = (vb * 20.0)/1000;
  return true;  
}

static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {
  char sbuffer[256];
  u8g2_uint_t w, h;
  // Third byte is return in steps of 5%
  uint8_t r = ((char*)pData)[2] * 5;
  uint8_t pellet_index = get_pellet_index(gun_index);
  uint16_t speed;    
  if((r >= sensitivity) && !renderMenu) {
    display_on_at = seconds();
    u8g2.setPowerSave(0);
    power_saving = false;

    renderMenu = false;
    shot_count++;    

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_VCR_OSD_tr);

    speed = ((char*)pData)[0];
    speed <<= 8;
    speed |= ((char*)pData)[1];

    float energy;
    float fspeed = speed;
    /* Draw the speed string */
    if(units == UNITS_IMPERIAL) {
      fspeed *= 0.0475111859;
      sprintf (sbuffer, "%d FPS", int(fspeed));
    } else {
      fspeed *= 0.014481409;
      sprintf (sbuffer, "%d M/S", int(fspeed));
    }
    /* w is the width of the string in pixels for the currently select font */
    w = u8g2.getStrWidth(sbuffer);
    /* centre the string horizontally, 27 pixels from the top */
    u8g2.drawStr((128-w)/2, 27, sbuffer);

    /* Draw the Pellet energy */
    if(units == UNITS_IMPERIAL) {
      energy = (my_pellets[pellet_index].pellet_weight_grains * powf(fspeed, 2))/450240;
      sprintf (sbuffer, "%.2f FPE", energy);
    } else {
      energy = 0.5 * (my_pellets[pellet_index].pellet_weight_grams / 1000) * powf(fspeed, 2);
      sprintf (sbuffer, "%.2f J", energy);
    }
    u8g2.setFont(u8g2_font_t0_14_tf);	// 9pt
    w = u8g2.getStrWidth(sbuffer);
    u8g2.drawStr((128-w)/2, 40, sbuffer);

    u8g2.setFont(u8g2_font_5x7_mr);	// 6pt
        
    sprintf(sbuffer, "%s / %s", my_guns[gun_index].gun_name, my_pellets[pellet_index].pellet_name);
    w = u8g2.getStrWidth(sbuffer);
    u8g2.drawStr((128-w)/2, 52, sbuffer);

    add_shot(gun_index, fspeed);

    sprintf (sbuffer, "# %d/%d", shot_count, my_guns[gun_index].shot_string_length);
    w = u8g2.getStrWidth(sbuffer);
    u8g2.drawStr(0, 63, sbuffer);

    sprintf (sbuffer, "R: %d%%", r);
    w = u8g2.getStrWidth(sbuffer);
    u8g2.drawStr(128-w, 63, sbuffer);

#if defined(ARDUINO_heltec_wifi_kit_32)
    renderDeviceVBatt();
#endif
    renderChronyVBatt();
    u8g2.sendBuffer();					
  }
  nc_counter++;
  nc_counter %= 5;
  digitalWrite(PIN_LED, nc_counter == 0 ? HIGH : LOW);
}

uint8_t profile_bytes[2][5] = {{0x32, 0x32, 0x32, 0x32, 0x64},{0x17, 0x1E, 0x2D, 0x43, 0x5A}};

//151-462

void connectToChrony() {
  Serial.print("Forming a connection to ");
  Serial.println(myDevice->getAddress().toString().c_str());
  
  pClient  = BLEDevice::createClient();
  Serial.println(" - Created client");

  pClient->setClientCallbacks(new MyClientCallback());

  // Connect to the remove BLE Server.
  pClient->connect(myDevice);  // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
  Serial.println(" - Connected to server");

  // Obtain a reference to the service we are after in the remote BLE server.
  pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(serviceUUID.toString().c_str());
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }
  Serial.println(" - Found our service");

  if(!writeChar(pRemoteService, 2, profile_bytes[0][my_guns[gun_index].gun_profile]))
  {
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }
  if(!writeChar(pRemoteService, 4, profile_bytes[1][my_guns[gun_index].gun_profile]))
  {
    pClient->disconnect();
    dirty = true;
    state = STATE_IDLE;
    return;
  }
  Serial.println(" - Profile Set");

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

  readBattery();

  state = STATE_CONNECTED;    
  u8g2.clearBuffer();					// clear the internal memory
  u8g2.sendBuffer();					// transfer internal memory to the display
  u8g2.setPowerSave(1);
  power_saving = true;
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
      renderSearching();
      do_scan(); 
      break;
    case STATE_CONNECTING:
      connectToChrony();
      break;
    case STATE_OFFLINE:
      break;      
    case STATE_CONNECTED:
      if(now - chronyVBattLastRead > 5) {
        readBattery();
        chronyVBattLastRead = now;
      }        
      break;
  }
}


/* 
  Menu system
*/


static void sleepCallback(uint8_t param)
{
  Serial.printf("Good bye cruel world\n");
  u8g2.setPowerSave(1);
  esp_deep_sleep_start();
}

static menuItem_t menu_sleep[] = {
  { "Zzzz",  NULL, menu_sleep, NULL, NULL, sleepCallback, 0, NULL},
};

void menuItemGenStringCurSleep(uint8_t, char * buffer)
{
  sprintf(buffer, "[%s]", menu_sleep[0].menuString);
}

static const uint8_t power_save_duration_lut[] = {0,2,5,10,20,40,60};

static void powerSaveCallback(uint8_t param)
{
  if(power_save_duration != power_save_duration_lut[param]) {
    power_save_duration = power_save_duration_lut[param];
    EEPROM.write(4, power_save_duration);
    EEPROM.commit();
  }  
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_power_save[] = {
  { "Off",  NULL, &menu_power_save[1], NULL, NULL, powerSaveCallback, 0, NULL},
  { "2s",   NULL, &menu_power_save[2], NULL, NULL, powerSaveCallback, 1, NULL},
  { "5s",   NULL, &menu_power_save[3], NULL, NULL, powerSaveCallback, 2, NULL},
  { "10s",  NULL, &menu_power_save[4], NULL, NULL, powerSaveCallback, 3, NULL},
  { "20s",  NULL, &menu_power_save[5], NULL, NULL, powerSaveCallback, 4, NULL},
  { "40s",  NULL, &menu_power_save[6], NULL, NULL, powerSaveCallback, 5, NULL},
  { "60s",  NULL, &menu_power_save[0], NULL, NULL, powerSaveCallback, 6, NULL},
};

void menuItemGenStringCurPowerSaving(uint8_t, char * buffer)
{
  uint8_t idx = 0;
  switch(power_save_duration)
  {
    case 2:  idx = 1;  break;
    case 5:  idx = 2;  break;
    case 10: idx = 3;  break;
    case 20: idx = 4;  break;
    case 40: idx = 5;  break;
    case 60: idx = 6;  break;    
  }  
  sprintf(buffer, "[%s]", menu_power_save[idx].menuString);
}

static void displayFlipCallback(uint8_t param)
{
  display_flip = param;
  EEPROM.write(3, display_flip);
  EEPROM.commit();
  u8g2.setFlipMode(display_flip);
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_display_flip[] = {
  { "Off",         NULL, &menu_display_flip[1], NULL, NULL, displayFlipCallback, DISPLAY_FLIP_OFF, NULL},
  { "On",          NULL, &menu_display_flip[0], NULL, NULL, displayFlipCallback, DISPLAY_FLIP_ON, NULL}
};

void menuItemGenStringCurDisplayFlip(uint8_t, char * buffer)
{
  sprintf(buffer, "[%s]", menu_display_flip[display_flip].menuString);
}

static void unitsCallback(uint8_t param)
{
  units = param;
  EEPROM.write(1, units);
  EEPROM.commit();
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_units[] = {
  { "FPS",          NULL, &menu_units[1], NULL, NULL, unitsCallback, UNITS_IMPERIAL, NULL},
  { "M/S",          NULL, &menu_units[0], NULL, NULL, unitsCallback, UNITS_METRIC, NULL}
};

void menuItemGenStringCurSelUnits(uint8_t, char * buffer)
{
  sprintf(buffer, "[%s]", menu_units[units].menuString);
}

static void sensitivityIncCallback(uint8_t param)
{
  if(sensitivity <= 95){ 
    sensitivity += 5;
    EEPROM.write(0, sensitivity);
    EEPROM.commit();
  }
}
static void sensitivityDecCallback(uint8_t param)
{ 
  if(sensitivity >= 5){
    sensitivity -= 5;
    EEPROM.write(0, sensitivity);
    EEPROM.commit();
  }
}

static menuItem_t menu_sensitivity[] = {
  { "Increase",     NULL, &menu_sensitivity[1], NULL, NULL, sensitivityIncCallback, 0, NULL},
  { "Decrease",     NULL, &menu_sensitivity[0], NULL, NULL, sensitivityDecCallback, 0, NULL}
};

void menuItemGenStringSensitivity(uint8_t, char * buffer)
{
  sprintf(buffer, "%d %%", sensitivity);
}

void menuItemGenStringCurSelSensitivity(uint8_t, char * buffer)
{
  sprintf(buffer, "[%d %%]", sensitivity);
}

static void selectPelletCallback(uint8_t param)
{
  set_pellet_index(gun_index, param);
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

void menuItemGenStringPellet(uint8_t i, char * buffer)
{
  if(units == UNITS_IMPERIAL) {
    sprintf(buffer, "%s %.3f %.3f", my_pellets[i].pellet_mfr, my_pellets[i].pellet_weight_grains, my_pellets[i].pellet_caliber_inch);
  } else {
    sprintf(buffer, "%s %.3f %.3f", my_pellets[i].pellet_mfr, my_pellets[i].pellet_weight_grams, my_pellets[i].pellet_caliber_mm);
  }
}

void menuItemGenStringCurSelPellet(uint8_t i, char * buffer)
{
  uint8_t pellet_index = get_pellet_index(gun_index);
  sprintf(buffer, "[%s]",my_pellets[pellet_index].pellet_name);
}

static void selectGunCallback(uint8_t param)
{
  gun_index = param;
  EEPROM.write(2, gun_index);
  EEPROM.commit();
  pCurrentMenuItem = menuStack[--menuStackIndex];
  build_pellet_menu();
  profile_changed = true;
}

void menuItemGenStringGun(uint8_t i, char * buffer)
{
  if(units == UNITS_IMPERIAL) {
    sprintf(buffer, "%s %.3f", my_guns[i].gun_mfr, my_guns[i].gun_caliber_inch);
  } else {
    sprintf(buffer, "%s %.3f", my_guns[i].gun_mfr, my_guns[i].gun_caliber_mm);
  }
}

void menuItemGenStringCurSelGun(uint8_t i, char * buffer)
{
  sprintf(buffer, "[%s]",my_guns[gun_index].gun_name);
}

void shotStringStats(shot_stats_t * ss)
{
  float speed, dist, sum = 0, sum_of_dists_sqd = 0;
  uint8_t i, scnt = get_string_length(gun_index);
  if(scnt == 0) {
    ss->min = 0;
    ss->max = 0;
    ss->avg = 0;
    ss->es = 0;
    ss->sd = 0;
    return;    
  }

  ss->min = 1000000;
  ss->max = 0;

  for(i = 0; i < scnt; i++) {
    speed = get_shot(gun_index, i);
    if(speed < ss->min){
      ss->min = speed;
    }
    if(speed > ss->max){
      ss->max = speed;
    }
    sum += speed;
  }

  ss->avg = sum / (float)scnt;
  ss->es = ss->max - ss->min;

  for(i = 0; i < scnt; i++) {
    speed = get_shot(gun_index, i);
    if(speed > ss->avg) {
      dist = (speed - ss->avg);
    } else {
      dist = (ss->avg - speed);
    }
    sum_of_dists_sqd += powf(dist, 2);
  }
  ss->sd = sqrtf(sum_of_dists_sqd / (float)scnt);
}

static void shotStringClearCallback(uint8_t param)
{
  clear_string(gun_index);
}

static void shotStringDumpCallback(uint8_t param)
{
  shot_stats_t stats;
  uint8_t pellet_index = get_pellet_index(gun_index);
  uint8_t i, scnt = get_string_length(gun_index);
  Serial.printf("Shot String\n");
  Serial.printf("Gun %s %s\n",my_guns[gun_index].gun_mfr, my_guns[gun_index].gun_name);
  if(units == UNITS_IMPERIAL) {
    Serial.printf("Ammo %s %.3f %.3f\n",my_pellets[pellet_index].pellet_name, my_pellets[pellet_index].pellet_caliber_inch, my_pellets[pellet_index].pellet_weight_grains);
  } else {
    Serial.printf("Ammo %s %.3f %.3f\n",my_pellets[pellet_index].pellet_name, my_pellets[pellet_index].pellet_caliber_mm, my_pellets[pellet_index].pellet_weight_grams);
  }

  shotStringStats(&stats);  
  Serial.printf("Minimum : %.2f\n",stats.min);
  Serial.printf("Maximum : %.2f\n",stats.max);
  Serial.printf("Average : %.2f\n",stats.avg);
  Serial.printf("ES      : %.2f\n",stats.es);
  Serial.printf("SD      : %.2f\n",stats.sd);

  for(i = 0; i < scnt; i++) {
    Serial.printf("Shot %d: %.2f\n", i+1, get_shot(gun_index, i));
  }
}

static void shotStringInitCallback(uint8_t param)
{
  uint8_t i;
  for(i = 0; i < NUM_GUNS; i++) {
    clear_string(i);
  }
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static void shotStringStatsCallback(uint8_t param)
{
  shot_stats_t stats;
  char sbuffer[128];
  shotStringStats(&stats);  
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_t0_14_tf);
  sprintf(sbuffer, "Min : %d\n",(int)stats.min);
  u8g2_uint_t hdr_w = u8g2.getStrWidth(sbuffer);
  u8g2.drawStr((128-hdr_w)/2, 9, sbuffer);	

  sprintf(sbuffer, "Max : %d\n",(int)stats.max);
  hdr_w = u8g2.getStrWidth(sbuffer);
  u8g2.drawStr((128-hdr_w)/2, 23, sbuffer);	

  sprintf(sbuffer, "Ave : %d\n",(int)stats.avg);
  hdr_w = u8g2.getStrWidth(sbuffer);
  u8g2.drawStr((128-hdr_w)/2, 37, sbuffer);	

  sprintf(sbuffer, "ES  : %.2f\n",stats.es);
  hdr_w = u8g2.getStrWidth(sbuffer);
  u8g2.drawStr((128-hdr_w)/2, 51, sbuffer);	

  sprintf(sbuffer, "SD  : %.2f\n",stats.sd);
  hdr_w = u8g2.getStrWidth(sbuffer);
  u8g2.drawStr((128-hdr_w)/2, 63, sbuffer);	

  u8g2.sendBuffer();

  sleep(5);
}

static uint8_t review_counter;

void menuItemGenStringShotStringReview(uint8_t, char * buffer)
{
  if(get_string_length(gun_index) == 0){
    sprintf(buffer, "0/0: ---");
  } else {
    sprintf(buffer, "%d/%d: %d", review_counter + 1, get_string_length(gun_index), (int)get_shot(gun_index, review_counter));
    review_counter++;
    if(review_counter >= get_string_length(gun_index)){
      review_counter = 0;
    }
  }
}

void menuItemGenStringCurSelReview(uint8_t, char * buffer)
{
  sprintf(buffer, "");
  review_counter = 0; 
}

void shotStringReviewCallback(uint8_t)
{

}

static menuItem_t menu_shot_string_review[] = {
  { "Next",       NULL, &menu_shot_string_review[0], NULL, NULL, shotStringReviewCallback, 0, NULL},
};


static menuItem_t menu_shot_string[] = {
  { "Stats",       NULL, &menu_shot_string[1], NULL, NULL, shotStringStatsCallback, 0, NULL},
  { "Review", menuItemGenStringShotStringReview, &menu_shot_string[2], menu_shot_string_review, menu_shot_string_review, NULL, 0, menuItemGenStringCurSelReview},
  { "Clear",       NULL, &menu_shot_string[3], NULL, NULL, shotStringClearCallback, 0, NULL},
  { "Dump",        NULL, &menu_shot_string[4], NULL, NULL, shotStringDumpCallback, 0, NULL},
  { "Initialise",  NULL, &menu_shot_string[0], NULL, NULL, shotStringInitCallback, 0, NULL}
};

void menuItemGenStringCurShotString(uint8_t, char * buffer)
{
  sprintf(buffer, "[%d]", get_string_length(gun_index));
}

static menuItem_t menu_top_level[] = {
  { "Gun",          NULL,                         &menu_top_level[1], NULL,             NULL,             NULL, 0, menuItemGenStringCurSelGun},
  { "Pellet",       NULL,                         &menu_top_level[2], NULL,             NULL,             NULL, 0, menuItemGenStringCurSelPellet},
  { "Shot String",  NULL,                         &menu_top_level[3], menu_shot_string, menu_shot_string, NULL, 0, menuItemGenStringCurShotString},
  { "Min. Return",  menuItemGenStringSensitivity, &menu_top_level[4], menu_sensitivity, menu_sensitivity, NULL, 0, menuItemGenStringCurSelSensitivity},
  { "Units",        NULL,                         &menu_top_level[5], menu_units,       menu_units,       NULL, 0, menuItemGenStringCurSelUnits},
  { "Display Flip", NULL,                         &menu_top_level[6], menu_display_flip,menu_display_flip,NULL, 0, menuItemGenStringCurDisplayFlip},
  { "Power Save",   NULL,                         &menu_top_level[7], menu_power_save,  menu_power_save,  NULL, 0, menuItemGenStringCurPowerSaving},
  { "Sleep",        NULL,                         &menu_top_level[0], menu_sleep,       menu_sleep,       NULL, 0, menuItemGenStringCurSleep}
};

bool is_pellet_for_gun(uint8_t pidx)
{
  bool result = false;

  if (units == UNITS_IMPERIAL) {
    result = ((my_pellets[pidx].pellet_caliber_inch != 0) && (my_guns[gun_index].gun_caliber_inch != 0)) && (my_pellets[pidx].pellet_caliber_inch == my_guns[gun_index].gun_caliber_inch);    
  } else {
    result = ((my_pellets[pidx].pellet_caliber_mm != 0) && (my_guns[gun_index].gun_caliber_mm != 0)) && (my_pellets[pidx].pellet_caliber_mm == my_guns[gun_index].gun_caliber_mm);
  }
  return result;
}

uint8_t num_pellets_for_gun()
{
  uint8_t i, pcnt = 0;
  for(i=0;i<NUM_PELLETS;i++) {
    if(is_pellet_for_gun(i)) {
      pcnt += 1;
    }
  }
  return pcnt;
}

void build_pellet_menu()
{
  bool found_pellet;
  uint8_t pellet_index, i, npellets, nctr = 0;
  if(menu_pellet) {
    free(menu_pellet);
  }
  npellets = num_pellets_for_gun();
  Serial.printf("npellets %d\n",npellets);
  menu_pellet = (menuItem_t *)malloc(npellets * sizeof(menuItem_t));
  
  for(i=0;i<NUM_PELLETS;i++) {
    if(is_pellet_for_gun(i)) {
      menu_pellet[nctr].menuString = my_pellets[i].pellet_name;
      menu_pellet[nctr].menuItemGenString = NULL;
      menu_pellet[nctr].nextMenuItem = ((nctr == npellets - 1) ? &menu_pellet[0] : &menu_pellet[nctr+1]);
      menu_pellet[nctr].currentSubMenu = NULL;
      menu_pellet[nctr].subMenu = NULL;
      menu_pellet[nctr].menuItemCallback = selectPelletCallback;
      menu_pellet[nctr].param = i;
      menu_pellet[nctr].menuItemGenCurSelString = menuItemGenStringPellet;
      nctr += 1;
    }
  }
  menu_top_level[1].currentSubMenu = menu_pellet;
  menu_top_level[1].subMenu = menu_pellet;
  
  found_pellet = false;  
  pellet_index = get_pellet_index(gun_index);
 
  for(i = 0; i < nctr; i++) {
    if(pellet_index == menu_pellet[i].param) {
      found_pellet = true;
      break;
    }
  }
  if(!found_pellet) {
    set_pellet_index(gun_index, menu_pellet[0].param);
  }
}

void build_gun_menu()
{
  uint8_t i;
  menu_gun = (menuItem_t *)malloc(NUM_GUNS * sizeof(menuItem_t));
  for(i=0;i<NUM_GUNS;i++) {
    menu_gun[i].menuString = my_guns[i].gun_name;
    menu_gun[i].menuItemGenString = NULL;
    menu_gun[i].nextMenuItem = ((i == NUM_GUNS - 1) ? &menu_gun[0] : &menu_gun[i+1]);
    menu_gun[i].currentSubMenu = NULL;
    menu_gun[i].subMenu = NULL;
    menu_gun[i].menuItemCallback = selectGunCallback;
    menu_gun[i].param = i;
    menu_gun[i].menuItemGenCurSelString = menuItemGenStringGun;
  }
  menu_top_level[0].currentSubMenu = menu_gun;
  menu_top_level[0].subMenu = menu_gun;
  build_pellet_menu();
}

static menuItem_t menu_entry = {  "Settings", NULL, menu_top_level, menu_top_level, NULL, NULL, 0, NULL };

void singleClick()
{
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
      if(pCurrentMenuItem->currentSubMenu != NULL) {
        menuStack[menuStackIndex++] = pCurrentMenuItem;
        pCurrentMenuItem = pCurrentMenuItem->currentSubMenu;
      }
    }
  }
}

void longPressStop()
{
  if(renderMenu) {
    if(menuStackIndex == 1) {
      renderMenu = false;
      u8g2.clearBuffer();
      u8g2.sendBuffer();
      if(profile_changed) {
        ESP.restart();
        while(1){}
      }
      if(state == STATE_OFFLINE) {
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
    menu_entry.currentSubMenu = menu_top_level;
    menuStackIndex = 0;
    menuStack[menuStackIndex++] = pCurrentMenuItem;
  } else if(state == STATE_IDLE) {
    state = STATE_OFFLINE;
    profile_changed = false;
    dirty = true;
    renderMenu = true;
    pCurrentMenuItem = &menu_entry;
    menu_entry.currentSubMenu = menu_top_level;
    menuStackIndex = 0;
    menuStack[menuStackIndex++] = pCurrentMenuItem;
  }
}
