#include <MatrixHardware_ESP32_V0.h>
#include <SmartMatrix.h>
#include <time.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>

char bluetoothMode = 0;
//0-map data to buttons
//1-map data to wifi_ssid
//2-map data to wifi_password
std::string wifi_ssid = "";
std::string wifi_password = "";
long gmtOffsetSeconds = -4 * 3600;

////////////////////////////////////////////////////////////////////////////////
//MATRIX VARIABLES
////////////////////////////////////////////////////////////////////////////////

#define COLOR_DEPTH 24                  // Choose the color depth used for storing pixels in the layers: 24 or 48 (24 is good for most sketches - If the sketch uses type `rgb24` directly, COLOR_DEPTH must be 24)
const uint16_t kMatrixWidth = 32;       // Set to the width of your display, must be a multiple of 8
const uint16_t kMatrixHeight = 16;      // Set to the height of your display
const uint8_t kRefreshDepth = 36;       // Tradeoff of color quality vs refresh rate, max brightness, and RAM usage.  36 is typically good, drop down to 24 if you need to.  On Teensy, multiples of 3, up to 48: 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45, 48.  On ESP32: 24, 36, 48
const uint8_t kDmaBufferRows = 4;       // known working: 2-4, use 2 to save RAM, more to keep from dropping frames and automatically lowering refresh rate.  (This isn't used on ESP32, leave as default)
const uint8_t kPanelType = SM_PANELTYPE_HUB75_16ROW_MOD8SCAN;   // Choose the configuration that matches your panels.  See more details in MatrixCommonHub75.h and the docs: https://github.com/pixelmatix/SmartMatrix/wiki
const uint32_t kMatrixOptions = (SM_HUB75_OPTIONS_NONE);        // see docs for options: https://github.com/pixelmatix/SmartMatrix/wiki
const uint8_t kBackgroundLayerOptions = (SM_BACKGROUND_OPTIONS_NONE);
const uint8_t kScrollingLayerOptions = (SM_SCROLLING_OPTIONS_NONE);
const uint8_t kIndexedLayerOptions = (SM_INDEXED_OPTIONS_NONE);

SMARTMATRIX_ALLOCATE_BUFFERS(matrix, kMatrixWidth, kMatrixHeight, kRefreshDepth, kDmaBufferRows, kPanelType, kMatrixOptions);
SMARTMATRIX_ALLOCATE_BACKGROUND_LAYER(backgroundLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kBackgroundLayerOptions);
SMARTMATRIX_ALLOCATE_SCROLLING_LAYER(scrollingLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kScrollingLayerOptions);
SMARTMATRIX_ALLOCATE_INDEXED_LAYER(indexedLayer, kMatrixWidth, kMatrixHeight, COLOR_DEPTH, kIndexedLayerOptions);

const int defaultBrightness = 100;
const int defaultScrollOffset = 6;
const rgb24 defaultBackgroundColor = {0x40, 0, 0};
const rgb24 sunlightColor = {0xff, 0xca, 0x7c};

////////////////////////////////////////////////////////////////////////////////
//BUTTON VARIABLES
////////////////////////////////////////////////////////////////////////////////

//struct used for button debouncing
struct button {
  int pin;
  int state;
  int lastState;
  int reading;
  int debounceTime;
};

int debounceDelay = 50;
int buttonPins[] = {35, 34, 39, 36};
button buttons[4];

////////////////////////////////////////////////////////////////////////////////
//CLOCK VARIABLES
////////////////////////////////////////////////////////////////////////////////

time_t currentTime = 1619132563;
time_t alarmTime = 0;
struct tm *currentTimeInfo;
bool alarmOn = 0;

int lastMillis = 0;
int alarmStartMillis = 0;

int alarmLightPeriod = 60 * 1000;
int fullBrightnessTime = alarmLightPeriod / 5;

/*int tempSecond = 0;
  int tempMinute = 0;
  int tempHour = 0;
  int tempDay = 0;
  int tempMonth = 0;
  int tempYear = 0;*/

////////////////////////////////////////////////////////////////////////////////
//USER INTERFACE VARIABLES
////////////////////////////////////////////////////////////////////////////////

int programState = 0;
#define DISPLAY_TIME 0
#define MENU 1
#define SET_TIME 2
#define SET_DATE 3
#define SET_ALARM 4
#define ALARM_REACHED 5

int menuSelection = 0;
#define M_SET_TIME 0
#define M_SET_DATE 1
#define M_SET_ALARM 2

#define M_NUM_OPTIONS 3

int currentDigit = 0;
#define SECOND 0
#define MINUTE 1
#define HOUR 2
#define DAY 3
#define MONTH 4
#define YEAR 5

bool beepPlaying = 0;
int beepTime = 0;

////////////////////////////////////////////////////////////////////////////////
//WIFI/BLUETOOTH VARIABLES
////////////////////////////////////////////////////////////////////////////////

#define DEBUG true

BLEServer *pServer = NULL;
BLECharacteristic * pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;
uint8_t txValue = 0;

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 4; i++) {
    pinMode(buttonPins[i], INPUT);
    buttons[i] = {buttonPins[i], 1, 1, 0, 0};
  }

  ledcSetup(0, 1000, 16);
  ledcAttachPin(23, 0);
  ledcWrite(0, 0);

  matrix.addLayer(&backgroundLayer);
  matrix.addLayer(&scrollingLayer);
  matrix.addLayer(&indexedLayer);
  matrix.begin();

  matrix.setBrightness(defaultBrightness);

  scrollingLayer.setOffsetFromTop(defaultScrollOffset);

  backgroundLayer.enableColorCorrection(true);
  backgroundLayer.setFont(font5x7);

  lastMillis = millis();

  bluetoothSetup();

}

void loop() {
  //struct tm *timeInfo;
  //timeInfo = localtime(&currentTime);

  if (beepPlaying && millis() - beepTime > 20) {
    stopBeep();
  }

  if (programState != SET_TIME && programState != SET_DATE && millis() - lastMillis > 1000) {
    lastMillis = millis();
    currentTime++;
  }


  switch (programState) {
    case DISPLAY_TIME:
      currentTimeInfo = localtime(&currentTime);

      if (alarmOn && currentTime >= alarmTime) {
        programState = ALARM_REACHED;
        alarmStartMillis = millis();
      }

      drawTimeDate(currentTimeInfo);
      break;
    case MENU:
      drawMenu();
      break;
    case SET_TIME:
      drawEditTime(currentTimeInfo);
      break;
    case SET_DATE:
      drawEditDate(currentTimeInfo);
      break;
    case SET_ALARM:
      drawEditTime(currentTimeInfo);
      break;
    case ALARM_REACHED:
      handleAlarm();
      break;
  }

  updateButtons();
  bluetoothConnectionCheck();
}

////////////////////////////////////////////////////////////////////////////////
//BUTTON FUNCTIONS
////////////////////////////////////////////////////////////////////////////////

//check button inputs and update
void updateButtons() {
  for (int i = 0; i < 4; i++) {
    button *currentButton = buttons + i;
    int currentPin = currentButton->pin;

    currentButton->reading = digitalRead(currentPin);
    if (currentButton->reading != currentButton->lastState) {
      currentButton->debounceTime = millis();
    }

    if (millis() - currentButton->debounceTime > debounceDelay) {
      if (currentButton->state != currentButton->reading) {
        currentButton->state = currentButton->reading;
        if (currentButton->state == LOW) {
          handleButton(i);
        }
      }
    }
    currentButton->lastState = currentButton->reading;
  }
}

//take action based on which button has been pressed
void handleButton(int bNum) {
  startBeep();
  switch (programState) {
    case DISPLAY_TIME:
      if (bNum == 3) {
        menuSelection = 0;
        programState = MENU;
      }
      break;

    case MENU:
      switch (bNum) {
        case 0:
          if (menuSelection == 0) menuSelection = M_NUM_OPTIONS - 1;
          else menuSelection--;
          break;
        case 1:
          if (menuSelection == (M_NUM_OPTIONS - 1)) menuSelection = 0;
          else menuSelection++;
          break;
        case 2:
          programState = DISPLAY_TIME;
          break;
        case 3:
          switch (menuSelection) {
            case M_SET_TIME:
              currentDigit = HOUR;
              programState = SET_TIME;
              currentTimeInfo = localtime(&currentTime);
              break;
            case M_SET_DATE:
              currentDigit = MONTH;
              programState = SET_DATE;
              currentTimeInfo = localtime(&currentTime);
              break;
            case M_SET_ALARM:
              currentDigit = HOUR;
              programState = SET_ALARM;

              if (alarmTime == 0) {
                currentTimeInfo = localtime(&currentTime);
                currentTimeInfo->tm_hour = 0;
                currentTimeInfo->tm_min = 0;
                currentTimeInfo->tm_sec = 0;
                alarmTime = mktime(currentTimeInfo);
              }

              currentTimeInfo = localtime(&alarmTime);
              break;
          }
      }
      break;
    case SET_TIME:
      switch (bNum) {
        case 0:
          if (currentDigit == HOUR) {
            currentTime = mktime(currentTimeInfo);
            updateAlarmTime();
            programState = DISPLAY_TIME;
          }
          else {
            currentDigit = HOUR;
          }
          break;
        case 1:
          if (currentDigit == MINUTE) {
            currentTime = mktime(currentTimeInfo);
            updateAlarmTime();
            programState = DISPLAY_TIME;
          }
          else {
            currentDigit = MINUTE;
          }
          break;
        case 2:
          if (currentDigit == HOUR) currentTimeInfo->tm_hour = currentTimeInfo->tm_hour - 1;
          else if (currentDigit == MINUTE) currentTimeInfo->tm_min = currentTimeInfo->tm_min - 1;
          mktime(currentTimeInfo);
          break;
        case 3:
          if (currentDigit == HOUR) currentTimeInfo->tm_hour = currentTimeInfo->tm_hour + 1;
          else if (currentDigit == MINUTE) currentTimeInfo->tm_min = currentTimeInfo->tm_min + 1;
          mktime(currentTimeInfo);
          break;
      }
      break;
    case SET_DATE:
      switch (bNum) {
        case 0:
          if (currentDigit == MONTH) {
            currentTime = mktime(currentTimeInfo);
            updateAlarmTime();
            programState = DISPLAY_TIME;
          }
          else if (currentDigit == DAY) currentDigit = MONTH;
          else currentDigit = DAY;
          break;
        case 1:
          if (currentDigit == YEAR) {
            currentTime = mktime(currentTimeInfo);
            updateAlarmTime();
            programState = DISPLAY_TIME;
          }
          else if (currentDigit == DAY) currentDigit = YEAR;
          else currentDigit = DAY;
          break;
        case 2:
          if (currentDigit == MONTH) currentTimeInfo->tm_mon = currentTimeInfo->tm_mon - 1;
          else if (currentDigit == DAY) currentTimeInfo->tm_mday = currentTimeInfo->tm_mday - 1;
          else if (currentDigit == YEAR) currentTimeInfo->tm_year = currentTimeInfo->tm_year - 1;
          mktime(currentTimeInfo);
          break;
        case 3:
          if (currentDigit == MONTH) currentTimeInfo->tm_mon = currentTimeInfo->tm_mon + 1;
          else if (currentDigit == DAY) currentTimeInfo->tm_mday = currentTimeInfo->tm_mday + 1;
          else if (currentDigit == YEAR) currentTimeInfo->tm_year = currentTimeInfo->tm_year + 1;
          mktime(currentTimeInfo);
          break;
      }
      break;
    case SET_ALARM:
      switch (bNum) {
        case 0:
          if (currentDigit == HOUR) {
            alarmTime = mktime(currentTimeInfo);
            updateAlarmTime();
            alarmOn = 1;
            programState = DISPLAY_TIME;
          }
          else {
            currentDigit = HOUR;
          }
          break;
        case 1:
          if (currentDigit == MINUTE) {
            alarmTime = mktime(currentTimeInfo);
            updateAlarmTime();
            alarmOn = 1;
            programState = DISPLAY_TIME;
          }
          else {
            currentDigit = MINUTE;
          }
          break;
        case 2:
          if (currentDigit == HOUR) currentTimeInfo->tm_hour = currentTimeInfo->tm_hour - 1;
          else if (currentDigit == MINUTE) currentTimeInfo->tm_min = currentTimeInfo->tm_min - 1;
          mktime(currentTimeInfo);
          break;
        case 3:
          if (currentDigit == HOUR) currentTimeInfo->tm_hour = currentTimeInfo->tm_hour + 1;
          else if (currentDigit == MINUTE) currentTimeInfo->tm_min = currentTimeInfo->tm_min + 1;
          mktime(currentTimeInfo);
          break;
      }
      break;
    case ALARM_REACHED:
      updateAlarmTime();
      programState = DISPLAY_TIME;
      ledcWrite(0, 0);
      matrix.setBrightness(defaultBrightness);
      break;
  }
}


void startBeep() {
  beepPlaying = 1;
  beepTime = millis();
  ledcWrite(0, 65535 / 2);
}

void stopBeep() {
  beepPlaying = 0;
  ledcWrite(0, 0);
}

////////////////////////////////////////////////////////////////////////////////
//CLOCK FUNCTIONS
////////////////////////////////////////////////////////////////////////////////

void handleAlarm() {
  int timeSinceAlarm = millis() - alarmStartMillis;

  if (timeSinceAlarm <= fullBrightnessTime) {
    int brightness = 255 * (float(timeSinceAlarm) / float(fullBrightnessTime));

    matrix.setBrightness(brightness);
  }

  if (timeSinceAlarm >= alarmLightPeriod) {
    if (millis() % 500 > 200) {
      ledcWrite(0, 65535 / 2);
    }
    else {
      ledcWrite(0, 0);
    }
  }

  backgroundLayer.fillScreen(sunlightColor);
  backgroundLayer.swapBuffers();
}

//update the alarm so that it occurs at the same time the next day
void updateAlarmTime() {
  currentTimeInfo = localtime(&alarmTime);
  int alarmHour = currentTimeInfo->tm_hour;
  int alarmMinute = currentTimeInfo->tm_min;
  int alarmSecond = currentTimeInfo->tm_sec;

  currentTimeInfo = localtime(&currentTime);
  if (currentTimeInfo->tm_hour < alarmHour || (currentTimeInfo->tm_hour == alarmHour && currentTimeInfo->tm_min < alarmMinute)) {
    currentTimeInfo->tm_mday = currentTimeInfo->tm_mday;
  }
  else {
    currentTimeInfo->tm_mday = currentTimeInfo->tm_mday + 1;
  }
  currentTimeInfo->tm_hour = alarmHour;
  currentTimeInfo->tm_min = alarmMinute;
  currentTimeInfo->tm_sec = alarmSecond;

  alarmTime = mktime(currentTimeInfo);
  currentTimeInfo = localtime(&currentTime);
}

/*void loadTempTime() {
  struct tm *timeInfo;
  timeInfo = localtime(&currentTime);
  currentTimeInfo = localtime(&currentTime);

  tempSecond = timeInfo->tm_sec;
  tempMinute = timeInfo->tm_min;
  tempHour = timeInfo->tm_hour;
  tempDay = timeInfo->tm_mday;
  tempMonth = timeInfo->tm_mon;
  tempYear = timeInfo->tm_year;
  }

  void saveTempTime() {
  struct tm *timeInfo;
  timeInfo = localtime(&currentTime);

  timeInfo->tm_sec = tempSecond;
  timeInfo->tm_min = tempMinute;
  timeInfo->tm_hour = tempHour;
  timeInfo->tm_mday = tempDay;
  timeInfo->tm_mon = tempMonth;
  timeInfo->tm_year = tempYear;

  currentTime = mktime(timeInfo);
  currentTimeInfo = localtime(&currentTime);
  }*/

////////////////////////////////////////////////////////////////////////////////
//MATRIX FUNCTIONS
////////////////////////////////////////////////////////////////////////////////

//display the current time with the date below it on the matrix
void drawTimeDate(tm *timeInfo) {
  char timeText[32];
  char dateText[32];

  strftime(timeText, 32, "%H:%M", timeInfo);
  strftime(dateText, 32, "%m/%d/%y", timeInfo);

  backgroundLayer.fillScreen({0, 0, 0});
  backgroundLayer.setFont(font6x10);
  backgroundLayer.drawString(0, 0, {255, 255, 255}, timeText);
  backgroundLayer.setFont(font3x5);
  backgroundLayer.drawString(0, 10, {255, 255, 255}, dateText);
  backgroundLayer.swapBuffers();
}

void drawMenu() {
  backgroundLayer.fillScreen({0, 0, 0});
  backgroundLayer.setFont(font5x7);
  //backgroundLayer.drawString(0, 0, {255, 255, 255}, menuText[menuSelection]);

  switch (menuSelection) {
    case 0:
      backgroundLayer.drawString(0, 0, {255, 255, 255}, "edit");
      backgroundLayer.drawString(0, 8, {255, 255, 255}, "time");
      break;
    case 1:
      backgroundLayer.drawString(0, 0, {255, 255, 255}, "edit");
      backgroundLayer.drawString(0, 8, {255, 255, 255}, "date");
      break;
    case 2:
      backgroundLayer.drawString(0, 0, {255, 255, 255}, "set");
      backgroundLayer.drawString(0, 8, {255, 255, 255}, "alarm");
      break;
  }

  backgroundLayer.swapBuffers();
}

void drawEditTime(tm *timeInfo) {
  char timeText[32];

  strftime(timeText, 32, "%H:%M", timeInfo);

  backgroundLayer.fillScreen({0, 0, 0});
  backgroundLayer.setFont(font6x10);
  backgroundLayer.drawString(0, 2, {255, 255, 255}, timeText);

  switch (currentDigit) {
    case HOUR:
      backgroundLayer.drawString(0, 3, {255, 255, 255}, "__");
      break;
    case MINUTE:
      backgroundLayer.drawString(0, 3, {255, 255, 255}, "   __");
      break;
  }

  backgroundLayer.swapBuffers();
}

void drawEditDate(tm *timeInfo) {
  char dateText[32];

  strftime(dateText, 32, "%m/%d/%y", timeInfo);

  backgroundLayer.fillScreen({0, 0, 0});
  backgroundLayer.setFont(font3x5);
  backgroundLayer.drawString(0, 5, {255, 255, 255}, dateText);

  switch (currentDigit) {
    case MONTH:
      backgroundLayer.drawString(0, 6, {255, 255, 255}, "__");
      break;
    case DAY:
      backgroundLayer.drawString(0, 6, {255, 255, 255}, "   __");
      break;
    case YEAR:
      backgroundLayer.drawString(0, 6, {255, 255, 255}, "      __");
      break;
  }

  backgroundLayer.swapBuffers();
}

////////////////////////////////////////////////////////////////////////////////
//WIFI/BLUETOOTH FUNCTIONS
////////////////////////////////////////////////////////////////////////////////

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};

//function to be called when data is recived
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();
      //handel the data
      if (rxValue.length() > 0) {
        if (DEBUG)Serial.println(rxValue.c_str());
        switch (bluetoothMode) {
          case 0:
            //looking for a button press
            if (rxValue.length() == 5 && rxValue.substr(0, 2) == "!B") {
              //it is a button press
              //check the checksum
              //char sum=0;
              //for(int a=0;a<4;a++) sum+=rxValue[a];
              //if(sum==rxValue[4]){
              //checksum is valid
              //work only on button press
              if (rxValue[3] == '0') {
                //figure out wich button
                switch (rxValue[2]) {
                  case '1':
                    handleButton(0);
                    break;
                  case '2':
                    handleButton(1);
                    break;
                  case '3':
                    handleButton(2);
                    break;
                  case '4':
                    handleButton(3);
                    break;
                }
              }
              //}

            }
            break;
          case 1:
            wifi_ssid = rxValue.substr(0, rxValue.length() - 1);
            pTxCharacteristic->setValue("Password:");
            pTxCharacteristic->notify();
            bluetoothMode = 2;
            break;
          case 2:
            wifi_password = rxValue.substr(0, rxValue.length() - 1);
            bluetoothMode = 0;
            bool wifiRet = wifiTimeSet();
            if (wifiRet) {
              pTxCharacteristic->setValue("Connection Sucessful\n");
              pTxCharacteristic->notify();
            } else {
              pTxCharacteristic->setValue("Connection Failure\n");
              pTxCharacteristic->notify();
            }
            if (DEBUG)Serial.printf("wifiTimeSet return: %s\n", wifiRet ? "True" : "False");
            break;
        }
        if (DEBUG)Serial.printf("Mode: %d\n", bluetoothMode);
        if (DEBUG)Serial.printf("SSID: %s\n", wifi_ssid.c_str());
        if (DEBUG)Serial.printf("Password: %s\n", wifi_password.c_str());
        if (DEBUG)Serial.print("--------\n");
        /*Serial.println("*********");
          Serial.print("Received Value: ");
          for (int i = 0; i < rxValue.length(); i++)
          Serial.print(rxValue[i]);

          Serial.println();
          Serial.println("*********");*/
      }
    }
};


//will connect and get the time
//true if sucsefull
//false if failure
boolean wifiTimeSet(){
  //still needs to decide on the gmtOffset
  if(DEBUG)Serial.printf("Connecting to %s ", wifi_ssid.c_str());
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
  while (WiFi.status() != WL_CONNECTED) {
      //may need to protect from failures
      if(WiFi.status() == WL_CONNECTION_LOST){
        if(DEBUG)Serial.print(" Connection Failed\n");
        return false;
      }
      delay(500);
      if(DEBUG)Serial.print(".");
  }
  if(DEBUG)Serial.println(" CONNECTED");
  //get the time from the web
  //need to fix daylight savings
  configTime(gmtOffsetSeconds, 3600, "pool.ntp.org","time.nist.gov");
  //check if time was recived
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    if(DEBUG)Serial.println("Failed to obtain time");
    return false;
  }
  //debug display time
  if(DEBUG)Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  return true;
}


void bluetoothSetup() {
  // Create the BLE Device
  BLEDevice::init("ESPtest");

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create a BLE Characteristic
  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY
                      );

  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic * pRxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE
                                          );

  pRxCharacteristic->setCallbacks(new MyCallbacks());

  // Start the service
  pService->start();

  // Start advertising
  pServer->getAdvertising()->start();
}

void bluetoothConnectionCheck() {
  // disconnecting
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); // give the bluetooth stack the chance to get things ready
    pServer->startAdvertising(); // restart advertising
    //if(DEBUG)Serial.println("start advertising");
    oldDeviceConnected = deviceConnected;
  }
  // connecting
  if (deviceConnected && !oldDeviceConnected) {
    // do stuff here on connecting
    oldDeviceConnected = deviceConnected;
  }
}
