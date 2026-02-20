#ifndef MAIN_H
#define MAIN_H

// Include Libraries
#include <Wire.h>
#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include "DHT.h"
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// WiFi & Firebase Configuration
#define WIFI_SSID "Redmi A3"
#define WIFI_PASS "12445678"
#define FIREBASE_HOST "https://agrogreenhouse0-38d6a-default-rtdb.asia-southeast1.firebasedatabase.app"  // Without "/"
#define FIREBASE_SECRET "erxVUiPGn8XooEjUfoYzWYIoupd1u560sijnqJ2e"



String getFormattedDateTime();
void logDataToSD();
void manageMotor();
void readDHT11();
void readLDR();
void displayLCD();
void unifiedTask(void *parameter);
void handleLight();

void connectWiFi();
void sendDataToFirebase(int year, int month, int day, int hour, int minute, int second, 
    float temperature, float humidity, float light);
void processSDCard();


#endif