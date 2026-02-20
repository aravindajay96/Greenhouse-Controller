#include "main.h"

// LCD (I2C address 0x27, 16x2)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// RTC object
RTC_DS3231 rtc;

// SD card module (CS pin)
#define SD_CS 5

// Actuators
#define motor 2
#define fan 16
#define relayA 4
#define LightPin 32

// DHT11 sensor settings
#define DHTPIN 14
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// LDR pin
#define LDRPIN 36
#define sensorC 13

// Variables for sensor display
float temperature = 0.0, humidity = 0.0;
int light  = 0;
bool showDHT = true;

// Motor control variables
bool motorOn = false;
unsigned long motorStartMillis = 0;
const unsigned long motorRunDuration = 30000; // 1/2 minutes

// Last log time
int lastLoggedHour = -1;

File dataFile;

bool wifiConnected = false; // Flag to track WiFi status
bool sendingOnlyMode = false;


////// **Read from SD card, send data to Firebase, and clean up**//////////////

// **Process SD card and send data until the file is empty**
void processSDCard() {
    if (!SD.begin(SD_CS)) {
        Serial.println("SD Card initialization failed!");
        return;
    }

    if (!SD.exists("/datalog.txt")) {
        Serial.println("No datalog.txt file found.");
        sendingOnlyMode = false;
        return;
    }

    File dataFile = SD.open("/datalog.txt", FILE_READ);
    if (!dataFile || dataFile.available() == 0) {
        Serial.println("datalog.txt is empty.");
        dataFile.close();
        SD.remove("/datalog.txt"); // Clean up empty file
        sendingOnlyMode = false;
        return;
    }

    File tempFile = SD.open("/temp.txt", FILE_WRITE);
    if (!tempFile) {
        Serial.println("Failed to open temp.txt!");
        dataFile.close();
        return;
    }

    bool dataSent = false;

    while (dataFile.available()) {
        String line = dataFile.readStringUntil('\n');

        if (wifiConnected) {
            int year, month, day, hour, minute, second;
            float temperature, humidity;
            int light;

            if (sscanf(line.c_str(), "%d/%d/%d %d:%d:%d, T:%fC, H:%f%%, L:%d%%",
                       &year, &month, &day, &hour, &minute, &second,
                       &temperature, &humidity, &light) == 9) {

                sendDataToFirebase(year, month, day, hour, minute, second, temperature, humidity, light);
                dataSent = true;
            } else {
                Serial.println("Data parsing failed. Keeping line for retry.");
                tempFile.println(line);
            }
        } else {
            tempFile.println(line); // Save for later if WiFi is down
        }
    }

    dataFile.close();
    tempFile.close();

    if (dataSent) {
        SD.remove("/datalog.txt");
        SD.rename("/temp.txt", "/datalog.txt");
    } else {
        Serial.println("No data was sent. Keeping original datalog.");
        SD.remove("/temp.txt"); // Don't overwrite the original
    }

    sendingOnlyMode = false;
    Serial.println("Resuming normal operations.");
}

void handleLight() {
    DateTime now = rtc.now();
    int hour = now.hour();

   // if (hour >= 19 && hour < 24 || hour >= 0 && hour <3) {
   if (hour >= 18 && hour < 22) {
        digitalWrite(LightPin, HIGH);
        Serial.println("Light is ON (within 19:00 - 21:00)");
    } else {
        digitalWrite(LightPin, LOW);
        Serial.println("Light is OFF (outside 19:00 - 21:00)");
    }
}

// **Send Data to Firebase**
void sendDataToFirebase(int year, int month, int day, int hour, int minute, int second, 
                        float temperature, float humidity, float light) {
    if (!wifiConnected) return;

    String formattedMonth = (month < 10) ? "0" + String(month) : String(month);
    String formattedDay = (day < 10) ? "0" + String(day) : String(day);
    String formattedHour = (hour < 10) ? "0" + String(hour) : String(hour);
    String formattedminute = (minute < 10) ? "0" + String(minute) : String(minute);
    String formattedSecond = (second < 10) ? "0" + String(second) : String(second);

    HTTPClient http;
    String entryKey = String(year) + String(formattedMonth) + String(formattedDay) + "_" +
                  String(formattedHour) + String(formattedminute) + String(formattedSecond);
    String firebaseURL = String(FIREBASE_HOST) + "/sensorData/" + "/" + entryKey + ".json?auth=" + FIREBASE_SECRET;


    StaticJsonDocument<200> jsonDoc;
    //jsonDoc["timestamp"] = String(year) + "-" + String(month) + "-" + String(day) + " " +
     //                      String(hour) + ":" + String(minute) + ":" + String(second);
    jsonDoc["temperature"] = temperature;
    jsonDoc["humidity"] = humidity;
    jsonDoc["light"] = light;

    String jsonStr;
    serializeJson(jsonDoc, jsonStr);

    http.begin(firebaseURL);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.PUT(jsonStr);  // Use PUT instead of POST  // Use POST to append new data

    Serial.print("Firebase response: ");
    Serial.println(httpResponseCode);

    http.end();
}


// **WiFi Connection Function**
void connectWiFi() {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to WiFi");
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) { // Try 10 times
        Serial.print(".");
        delay(1000);
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected to WiFi!");
        wifiConnected = true;
    } else {
        Serial.println("\nFailed to connect to WiFi!");
        wifiConnected = false;
    }
}


// **WiFi Monitoring Task (Core 0)**
void WiFiTask(void *parameter) {
    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            if (!sendingOnlyMode) {
                Serial.println("WiFi Connected! Stopping all tasks except SD card processing...");
                sendingOnlyMode = true;
            }
        } else {
            if (sendingOnlyMode) {
                Serial.println("WiFi Disconnected! Resuming all tasks...");
                sendingOnlyMode = false;
            }
            Serial.println("WiFi disconnected! Trying to reconnect...");
            connectWiFi(); // Try reconnecting
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}



//////// **Unified Task for Sensors, Motor & Logging on Core 1**/////////////

void unifiedTask(void *parameter) {
    for (;;) {
        if (!sendingOnlyMode) { // Only run if not in sending-only mode
            readDHT11();
            readLDR();
            displayLCD();
            manageMotor();
            handleLight();
            logDataToSD();
            showDHT = !showDHT;
        }
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}


// **Get Date and Time**
String getFormattedDateTime() {
    DateTime now = rtc.now();
    char buffer[20];
    sprintf(buffer, "%02d/%02d %02d:%02d:%02d", now.day(), now.month(), now.hour(), now.minute(), now.second());
    return String(buffer);
}

// **Log Data to SD Card**
void logDataToSD() {
    DateTime now = rtc.now();

    // Log once per hour when hour changes
    if (now.hour() != lastLoggedHour) {
        lastLoggedHour = now.hour(); // Update last logged hour

        File logFile = SD.open("/datalog.txt", FILE_APPEND);
        if (logFile) {
            logFile.printf("%04d/%02d/%02d %02d:%02d:%02d, T:%.2fC, H:%.2f%%, L:%d%%\r\n",
                now.year(), now.month(), now.day(),
                now.hour(), now.minute(), now.second(),
                temperature, humidity, light);

            logFile.close();
            Serial.println("Data logged to SD card.");
        } else {
            Serial.println("Error opening SD card file!");
        }
    }
}

// **Motor Management**
void manageMotor() {
    DateTime now = rtc.now();
    int sensorCState = digitalRead(sensorC);
    int timeCount = now.hour() * 60 + now.minute();  // minutes since midnight

    static int lastActivationBaseTime = -10; // last base activation minute
    static int activationPhase = 0;          // 0 = idle, 1..4 = run number
    static bool motorPending = false;

    int activationInterval = (sensorCState == 1) ? 120 : 60; // 2h or 1h

    // --- NIGHT MODE BLOCK (disable between 21:00 and 07:00) ---
    if (now.hour() >= 21 || now.hour() < 7) {
        // Make sure motor is OFF
        if (motorOn) {
            motorOn = false;
            digitalWrite(motor, LOW);
            Serial.println("Motor forced OFF (night mode)");
        }
        activationPhase = 0;
        motorPending = false;
        return; // skip all logic during night
    }
    // ---------------------------------------------------------

    // Reset at new day
    if (timeCount < lastActivationBaseTime) {
        lastActivationBaseTime = -10;
    }

    // Start new cycle (Run 1)
    if ((timeCount % activationInterval == 0) && (timeCount != lastActivationBaseTime)) {
        lastActivationBaseTime = timeCount;
        activationPhase = 1;
        motorPending = false;
        motorOn = true;
        motorStartMillis = millis();
        digitalWrite(motor, HIGH);
        Serial.println("Motor turned ON (Run 1)");
    }

    // Turn OFF motor after run duration
    if (motorOn && (millis() - motorStartMillis >= motorRunDuration)) {
        motorOn = false;
        digitalWrite(motor, LOW);
        Serial.println("Motor turned OFF");

        if (activationPhase > 0 && activationPhase < 4) {
            motorPending = true;  // ready for next run
        } else {
            activationPhase = 0;  // finished all runs
        }
    }

    // Handle subsequent activations (+5, +10, +15 min)
    if (motorPending && (timeCount == lastActivationBaseTime + activationPhase * 5)) {
        motorOn = true;
        motorStartMillis = millis();
        digitalWrite(motor, HIGH);
        activationPhase++;
        motorPending = false;
        Serial.print("Motor turned ON (Run ");
        Serial.print(activationPhase);
        Serial.println(")");
    }

    // Debug once per minute
    static int lastPrintMinute = -1;
    if (now.minute() != lastPrintMinute) {
        lastPrintMinute = now.minute();
        Serial.print("TimeCount: ");
        Serial.print(timeCount);
        Serial.print("   SensorC: ");
        Serial.print(sensorCState);
        Serial.print("   Phase: ");
        Serial.println(activationPhase);
    }
}

// **Read DHT11 Sensor**
void readDHT11() {
    temperature = dht.readTemperature();
    humidity = dht.readHumidity();
    if (isnan(temperature) || isnan(humidity)) {
        Serial.println("Failed to read from DHT sensor!");
    }
}

// **Read LDR Sensor**
void readLDR() {
    int ldrValue = analogRead(LDRPIN);
    light = map(ldrValue, 0, 1750, 0, 100); // Map raw value to 0-100%
}

// **Display Data on LCD**
void displayLCD() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(getFormattedDateTime());
    lcd.setCursor(0, 1);
    if (showDHT) {
        lcd.printf("T:%.1fC H:%.1f%%", temperature, humidity);
    } else {
        lcd.printf("Light: %d%%", light);
    }
}

// **Setup Function**
void setup() {
    Serial.begin(115200);
    lcd.init();
    lcd.backlight();
    connectWiFi();
    
    if (!rtc.begin()) {
        Serial.println("Couldn't find RTC");
        while (1);
    }

    if (rtc.lostPower()) {
        Serial.println("RTC lost power, setting time!");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    dht.begin();
    
   //Output Pins
    pinMode(motor, OUTPUT);
    digitalWrite(motor, LOW);
    pinMode(fan, OUTPUT);
    digitalWrite(fan, LOW);
    pinMode(LightPin, OUTPUT);
    digitalWrite(LightPin, LOW);
    pinMode(relayA, OUTPUT);
    digitalWrite(relayA, LOW);
    //Input Pins
    pinMode(sensorC, INPUT);
    //pinMode(LDRPIN, INPUT);

    if (!SD.begin(SD_CS)) {
        Serial.println("SD card initialization failed!");
    } else {
        Serial.println("SD card initialized.");
    }

    // **Create Task for Unified Functions on Core 1**
    xTaskCreatePinnedToCore(
        unifiedTask,   // Task function
        "Unified Task",
        4096,
        NULL,
        1,
        NULL,
        1 // Runs on Core 1
    );

    // **Create Task for WiFi Monitoring on Core 0**
    xTaskCreatePinnedToCore(
        WiFiTask,      // Task function
        "WiFi Task",
        2048,
        NULL,
        1,
        NULL,
        0 // Runs on Core 0
    );
}

void loop() {
    if (sendingOnlyMode) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Sending Data...");
        lcd.setCursor(0, 1);
        lcd.print("Please Wait...");
        
        processSDCard(); // Only send SD card data to Firebase
    }

    vTaskDelay(100 / portTICK_PERIOD_MS); // Yield to FreeRTOS
}
