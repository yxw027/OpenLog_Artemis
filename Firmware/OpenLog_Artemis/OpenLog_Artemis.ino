/*
  OpenLog Artemis
  By: Nathan Seidle
  SparkFun Electronics
  Date: November 26th, 2019
  License: This code is public domain but you buy me a beer if you use this
  and we meet someday (Beerware license).
  Feel like supporting our work? Buy a board from SparkFun!
  https://www.sparkfun.com/products/15793

  This firmware runs the OpenLog Artemis. A large variety of system settings can be
  adjusted by connecting at 115200bps.

  TODO:
  get time stamps from GPS
  manual set RTC to GPS

  disable local sensor logging, log only serial
  (done) disable terminal output for max analog logging
  (nah) Change filename(s) to record to (data vs serial logging)
  (done) Auto number the datalog and serial log file names
  Enable/disable time stamping of incoming serial
  Toggle LED on serial data recording vs sensor recording
  (done) If VCC is detected as dropping below 3V (diode drop from batt backup) then go into shutdown
  Check out the file creation. Use FILE_WRITE instead of the O_s. Might go faster without append...
  Support multiples of a given sensor. How to support two MCP9600s attached at the same time?
  Allow user to export the current settings to a settings.txt file that the can use to setup other OpenLogs
  Setup a sleep timer, wake up ever 5 seconds, power up Qwiic, take reading, power down I2C bus, sleep.
  Could you store the date from the RTC because it won't change that much?
  (done) Eval how long it takes to boot (SD, log creation, IMU begin, etc)
  Allow user to decrease I2C speed on GPS to increase reliability
  Control Qwiic power from...

  Allow user to control local time stamp with GPS UTC offset

  The Qwiic device settings menus don't change the devices directly. These are set at the exit of the main menu
  when sensors are begun().

  Python/windows program to load new hex files

  What happens when user enables analog on pin 12/tx and keeps serial on pin 13/rx and then changes baud rate? Does analog still work?
  What happens when user enables serial on 13/rx then enables analog read on pin 12/tx? Does analog still work?

  Max rates:
  ~1140Hz for 3 channel analog, max data rate
*/

#include "settings.h"

const byte PIN_STAT_LED = 19;
const byte PIN_POWER_LOSS = 3;
const byte PIN_LOGIC_DEBUG = 11; //TODO remove from production

//Setup Qwiic Port
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
#include "Wire.h"
TwoWire qwiic(1); //Will use pads 8/9
const byte PIN_QWIIC_PWR = 18;
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

//EEPROM for storing settings
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
#include <EEPROM.h>
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

//microSD Interface
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
#include <SPI.h>
#include <SdFat.h> //We use SdFat-Beta from Bill Greiman for increased read/write speed

//x02+ Hardware
const byte PIN_MICROSD_CHIP_SELECT = 10;
const byte PIN_MICROSD_POWER = 23; //x03
//const byte PIN_MICROSD_POWER = 15; //x04

#define SD_CONFIG SdSpiConfig(PIN_MICROSD_CHIP_SELECT, SHARED_SPI, SD_SCK_MHZ(24)) //Max of 24MHz
#define SD_CONFIG_MAX_SPEED SdSpiConfig(PIN_MICROSD_CHIP_SELECT, DEDICATED_SPI, SD_SCK_MHZ(24)) //Max of 24MHz

SdFat sd;
File sensorDataFile; //File that all sensor data is written to
File serialDataFile; //File that all incoming serial data is written to
bool newSerialData = false;
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

//Add RTC interface for Artemis
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
#include "RTC.h" //Include RTC library included with the Aruino_Apollo3 core
APM3_RTC myRTC; //Create instance of RTC class
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

//Create UART instance for OpenLog style serial logging
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
Uart SerialLog(1, 13, 12);  // Declares a Uart object called Serial1 using instance 1 of Apollo3 UART peripherals with RX on pin 13 and TX on pin 12 (note, you specify *pins* not Apollo3 pads. This uses the variant's pin map to determine the Apollo3 pad)
unsigned long lastSeriaLogSyncTime = 0;
const int MAX_IDLE_TIME_MSEC = 500;
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

//Add ICM IMU interface
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
#include "ICM_20948.h"  // Click here to get the library: http://librarymanager/All#SparkFun_ICM_20948_IMU
const byte PIN_IMU_CHIP_SELECT = 44;
const byte PIN_IMU_POWER = 22;
const byte PIN_IMU_INT = 37;
ICM_20948_SPI myICM;
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

//Header files for all possible Qwiic sensors
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
#include <SparkFun_LPS25HB_Arduino_Library.h>  //Click here to get the library: http://librarymanager/All#SparkFun_LPS25HB
LPS25HB pressureSensor;

#include <SparkFun_MCP9600.h>  //Click here to get the library: http://librarymanager/All#SparkFun_MCP9600
MCP9600 thermoSensor;

#include "SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h" // Click here to get the library: http://librarymanager/All#SparkFun_NAU7802
NAU7802 nauScale;

#include "SparkFun_VL53L1X.h" //Click here to get the library: http://librarymanager/All#SparkFun_VL53L1X
SFEVL53L1X distanceSensor_VL53L1X(qwiic);

#include "SparkFun_Ublox_Arduino_Library.h" //http://librarymanager/All#SparkFun_Ublox_GPS
SFE_UBLOX_GPS myGPS;

#include "SparkFun_VCNL4040_Arduino_Library.h" //Click here to get the library: http://librarymanager/All#SparkFun_VCNL4040
VCNL4040 proximitySensor_VCNL4040;

#include <SparkFun_TMP117.h> //Click here to get the library: http://librarymanager/All#SparkFun_TMP117
TMP117 tempSensor_TMP117;

#include "SparkFunCCS811.h" //Click here to get the library: http://librarymanager/All#SparkFun_CCS811
#define CCS811_ADDR 0x5B //Default I2C Address
//#define CCS811_ADDR 0x5A //Alternate I2C Address
CCS811 vocSensor_CCS811(CCS811_ADDR);

#include "SparkFunBME280.h" //Click here to get the library: http://librarymanager/All#SparkFun_BME280
BME280 phtSensor_BME280;

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

//Global variables
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
unsigned long measurementStartTime; //Used to calc the actual update rate.
unsigned long measurementCount = 0; //Used to calc the actual update rate.
String outputData;
unsigned long lastReadTime = 0; //Used to delay until user wants to record a new reading
unsigned long lastDataLogSyncTime = 0; //Used to record to SD every half second
bool helperTextPrinted = false; //Print the column headers only once
unsigned int totalCharactersPrinted = 0; //Limit output rate based on baud rate and number of characters to print
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

unsigned long startTime = 0;

void setup() {
  startTime = micros();

  //If 3.3V rail drops below 3V, system will enter low power mode and maintain RTC
  pinMode(PIN_POWER_LOSS, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_POWER_LOSS), powerDown, FALLING);

  pinMode(PIN_STAT_LED, OUTPUT);
  digitalWrite(PIN_STAT_LED, LOW);

  Serial.begin(115200); //Default for initial debug messages if necessary
  delay(10);
  Serial.println();

  SPI.begin(); //Needed if SD is disabled

  beginSD(); //285 - 293ms

  loadSettings(); //50 - 250ms

  Serial.begin(settings.serialTerminalBaudRate);
  Serial.println("Artemis OpenLog");

  beginQwiic();

  analogReadResolution(14); //Increase from default of 10

  beginDataLogging(); //180ms

  beginSerialLogging(); //20 - 99ms

  beginIMU(); //61ms

  beginSensors(); //159 - 865ms but varies based on number of devices attached

  measurementStartTime = millis();

  if (settings.logMaxRate == true) Serial.println("Logging analog pins at max data rate");

  if (settings.enableTerminalOutput == false && settings.logData == true) Serial.println("Logging to microSD card with no terminal output");

  Serial.printf("Setup time: %.02f ms\n", (micros() - startTime) / 1000.0);
}

void loop() {

  if (Serial.available()) menuMain(); //Present user menu

  if (settings.logSerial == true && online.serialLogging == true)
  {
    if (SerialLog.available())
    {
      char temp[256];
      uint16_t counter = 0;
      while (SerialLog.available())
      {
        temp[counter++] = SerialLog.read();
        if (counter == 512) break;
      }

      serialDataFile.write(temp, counter); //Record the buffer to the card
      lastSeriaLogSyncTime = millis(); //Reset the last sync time to now
      newSerialData = true;

      //Toggle stat LED indicating log recording
    }
    //No characters received?
    else if (newSerialData == true)
    {
      if ((millis() - lastSeriaLogSyncTime) > MAX_IDLE_TIME_MSEC) //If we haven't received any characters recently then sync log file
      {
        newSerialData = false;
        serialDataFile.sync();
        lastSeriaLogSyncTime = millis(); //Reset the last sync time to now
      }
    }
  }

  //Is it time to get new data?
  if (settings.logMaxRate == true || (millis() - lastReadTime) >= (1000UL / settings.recordPerSecond))
  {
    lastReadTime = millis();

    getData(); //Query all enabled sensors for data

    //Print to terminal
    if (settings.enableTerminalOutput == true)
      Serial.print(outputData); //Print to terminal

    //Record to SD
    if (settings.logData == true)
    {
      if (settings.enableSD && online.microSD)
      {
        char temp[512];
        outputData.toCharArray(temp, 512); //Convert string to char array so sdfat can record it
        sensorDataFile.write(temp, strlen(temp)); //Record the buffer to the card

        //Force sync every 500ms
        if (millis() - lastDataLogSyncTime > 500)
        {
          lastDataLogSyncTime = millis();
          digitalWrite(PIN_STAT_LED, HIGH);
          sensorDataFile.sync();
          digitalWrite(PIN_STAT_LED, LOW);
        }
      }
    }

    //    if (digitalRead(PIN_STAT_LED) == HIGH)
    //      digitalWrite(PIN_STAT_LED, LOW);
    //    else
    //      digitalWrite(PIN_STAT_LED, HIGH);
  }
}

void beginQwiic()
{
  pinMode(PIN_QWIIC_PWR, OUTPUT);
  qwiicPowerOn();
  qwiic.begin();
}

void beginSD()
{
  pinMode(PIN_MICROSD_POWER, OUTPUT);
  pinMode(PIN_MICROSD_CHIP_SELECT, OUTPUT);
  digitalWrite(PIN_MICROSD_CHIP_SELECT, HIGH); //Be sure SD is deselected

  if (settings.enableSD == true)
  {
    sdPowerOn();

    //Max power up time is 250ms: https://www.kingston.com/datasheets/SDCIT-specsheet-64gb_en.pdf
    //Max current is 200mA average across 1s, peak 300mA
    delay(10);

    //We can get faster SPI transfer rates if we have only one device enabled on the SPI bus
    //But we have a chicken and egg problem: We need to load settings before we enable SD, but we
    //need the SD to load the settings file. For now, we will disable the logMaxRate option.
    //    if (settings.logMaxRate == true)
    //    {
    //      if (sd.begin(SD_CONFIG_MAX_SPEED) == false) //Very Fast SdFat Beta (dedicated SPI, no IMU)
    //      {
    //        Serial.println("SD init failed. Do you have the correct board selected in Arduino? Is card present? Formatted?");
    //        online.microSD = false;
    //        return;
    //      }
    //    }
    //    else
    //    {
    if (sd.begin(SD_CONFIG) == false) //Slightly Faster SdFat Beta (we don't have dedicated SPI)
    {
      delay(250); //Give SD more time to power up, then try again
      if (sd.begin(SD_CONFIG) == false) //Slightly Faster SdFat Beta (we don't have dedicated SPI)
      {
        Serial.println("SD init failed. Do you have the correct board selected in Arduino? Is card present? Formatted?");
        digitalWrite(PIN_MICROSD_CHIP_SELECT, HIGH); //Be sure SD is deselected
        online.microSD = false;
        return;
      }
    }
    //    }

    if (sd.chdir() == false)
    {
      Serial.println("SD change directory failed");
      //systemError(ERROR_ROOT_INIT); //Change to root directory. All new file creation will be in root.
      online.microSD = false;
      return;
    }

    msg("SD card online");
    online.microSD = true;
  }
  else
  {
    sdPowerOff();

    Serial.println("SD offline/disabled");
    online.microSD = false;
  }
}

void beginIMU()
{
  pinMode(PIN_IMU_POWER, OUTPUT);
  pinMode(PIN_IMU_CHIP_SELECT, OUTPUT);
  digitalWrite(PIN_IMU_CHIP_SELECT, HIGH); //Be sure IMU is deselected

  if (settings.enableIMU == true && settings.logMaxRate == false)
  {
    //Reset ICM by power cycling it
    digitalWrite(PIN_IMU_POWER, LOW);
    delay(10); //10 is fine
    digitalWrite(PIN_IMU_POWER, HIGH);
    delay(25); //Allow ICM to come online. Typical is 11ms. Max is 100ms. https://cdn.sparkfun.com/assets/7/f/e/c/d/DS-000189-ICM-20948-v1.3.pdf

    myICM.begin(PIN_IMU_CHIP_SELECT, SPI, 4000000); //Set IMU SPI rate to 4MHz
    if (myICM.status != ICM_20948_Stat_Ok)
    {
      //Try one more time with longer wait

      //Reset ICM by power cycling it
      digitalWrite(PIN_IMU_POWER, LOW);
      delay(10); //10 is fine
      digitalWrite(PIN_IMU_POWER, HIGH);
      delay(100); //Allow ICM to come online. Typical is 11ms. Max is 100ms.

      myICM.begin(PIN_IMU_CHIP_SELECT, SPI, 4000000); //Set IMU SPI rate to 4MHz
      if (myICM.status != ICM_20948_Stat_Ok)
      {
        digitalWrite(PIN_IMU_CHIP_SELECT, HIGH); //Be sure IMU is deselected
        msg("ICM-20948 failed to init.");
        online.IMU = false;
        return;
      }
    }

    online.IMU = true;
    msg("IMU online");
  }
  else
  {
    //Power down IMU
    digitalWrite(PIN_IMU_POWER, LOW);

    msg("IMU disabled");
    online.IMU = false;
  }
}

void beginDataLogging()
{
  if (online.microSD == true && settings.logData == true)
  {
    // O_CREAT - create the file if it does not exist
    // O_APPEND - seek to the end of the file prior to each write
    // O_WRITE - open for write
    if (sensorDataFile.open(findNextAvailableLog(settings.nextDataLogNumber, "dataLog"), O_CREAT | O_APPEND | O_WRITE) == false)
    {
      Serial.println("Failed to create sensor data file");
      online.dataLogging = false;
      return;
    }

    msg("Data logging online");
    online.dataLogging = true;
  }
  else if (settings.logData == false && online.microSD == true)
  {
    msg("Data logging disabled");
    online.dataLogging = false;
  }
  else if (online.microSD == false)
  {
    Serial.println("Data logging disabled because microSD offline");
    online.serialLogging = false;
  }
  else
  {
    Serial.println("Unknown microSD state");
  }
}

void beginSerialLogging()
{
  if (online.microSD == true && settings.logSerial == true)
  {
    if (serialDataFile.open(findNextAvailableLog(settings.nextSerialLogNumber, "serialLog"), O_CREAT | O_APPEND | O_WRITE) == false)
    {
      Serial.println("Failed to create serial log file");
      //systemError(ERROR_FILE_OPEN);
      online.serialLogging = false;
      return;
    }

    //pinMode(13, INPUT);

    SerialLog.begin(settings.serialLogBaudRate);

    msg("Serial logging online");
    online.serialLogging = true;
  }
  else if (settings.logSerial == false && online.microSD == true)
  {
    msg("Serial logging disabled");
    online.serialLogging = false;
  }
  else if (online.microSD == false)
  {
    Serial.println("Serial logging disabled because microSD offline");
    online.serialLogging = false;
  }
  else
  {
    Serial.println("Unknown microSD state");
  }
}
