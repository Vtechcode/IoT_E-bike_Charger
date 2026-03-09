/*
 * E-Bike Charger ESP32 Firmware
 * 
 * This firmware connects your ESP32-based e-bike charger to Firebase Realtime Database
 * for remote monitoring and control via the mobile app.
 * 
 * Hardware Configuration:
 * - ESP32 DevKit or similar
 * - ACS712 Current Sensor connected to ADC pin (GPIO 34)
 * - Optocoupler control pin (GPIO 25) - controls MOSFET via LM7812
 * - Power supply: 5V from buck converter (from 23V DC rectified)
 * 
 * Dependencies:
 * - Firebase ESP32 Client library by mobizt
 * - WiFi library (built-in)
 * 
 * Install via Arduino IDE Library Manager:
 * - Search for "Firebase ESP32 Client" and install
 */

#include <WiFi.h>
#include <Firebase_ESP_Client.h>

// Provide the token generation process info
#include "addons/TokenHelper.h"
// Provide the RTDB payload printing info and other helper functions
#include "addons/RTDBHelper.h"

// ==================== CONFIGURATION ====================
// WiFi credentials - UPDATE THESE
#define WIFI_SSID "dekut"
#define WIFI_PASSWORD "dekut@ict2023"

// Firebase configuration - UPDATE THESE
#define FIREBASE_HOST "ev-charger-test2-default-rtdb.firebaseio.com/"  // Without https://
#define FIREBASE_AUTH "232bf28bf9cf5208939c2815650af5e79dddc7b4"  // Leave empty for public database, or add your database secret
#define FIREBASE_API_KEY "AIzaSyCHPUZFMsumFDEUJ-SX2SX7Mb5lWnPUx14"

// Charger configuration - UPDATE THIS
#define CHARGER_ID "charger_01"  // Must match the ID in the mobile app

// Hardware pin configuration
#define ACS712_PIN 34        // ADC pin for ACS712 current sensor
#define CONTROL_PIN 23       // GPIO pin for optocoupler control (to MOSFET gate)
#define LED_PIN 2            // Built-in LED for status indication

// ACS712 configuration (30A version)
// For 5A version: sensitivity = 0.185
// For 20A version: sensitivity = 0.100
// For 30A version: sensitivity = 0.066
#define ACS712_SENSITIVITY 0.066  // V/A for 30A version
#define ACS712_ZERO_POINT 2.5     // Zero current voltage (VCC/2)
#define ADC_RESOLUTION 4095.0     // 12-bit ADC
#define ADC_VOLTAGE 3.3           // ESP32 ADC reference voltage

// Update intervals (milliseconds)
#define READING_INTERVAL 2000     // How often to read and send current
#define CONTROL_CHECK_INTERVAL 500 // How often to check for control commands

// ==================== GLOBAL VARIABLES ====================
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long lastReadingTime = 0;
unsigned long lastControlCheckTime = 0;
bool chargerState = false;
bool firebaseConnected = false;

// Firebase paths
String controlPath = String("/chargers/") + CHARGER_ID + "/control/state";
String readingsPath = String("/chargers/") + CHARGER_ID + "/readings";

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== E-Bike Charger ESP32 ===");
  Serial.println("Initializing...");
  
  // Initialize pins
  pinMode(CONTROL_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(CONTROL_PIN, LOW);  // Start with charger OFF
  digitalWrite(LED_PIN, LOW);
  
  // Connect to WiFi
  connectWiFi();
  
  // Initialize Firebase
  initFirebase();
  
  // Set initial state in Firebase
  setInitialState();
  
  Serial.println("Setup complete!");
  Serial.println("Charger ID: " + String(CHARGER_ID));
}

// ==================== MAIN LOOP ====================
void loop() {
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Reconnecting...");
    connectWiFi();
  }
  
  // Check Firebase connection
  if (!Firebase.ready()) {
    Serial.println("Firebase not ready. Reinitializing...");
    initFirebase();
    return;
  }
  
  unsigned long currentTime = millis();
  
  // Check for control commands from Firebase
  if (currentTime - lastControlCheckTime >= CONTROL_CHECK_INTERVAL) {
    lastControlCheckTime = currentTime;
    checkControlState();
  }
  
  // Read and send current measurement
  if (currentTime - lastReadingTime >= READING_INTERVAL) {
    lastReadingTime = currentTime;
    sendCurrentReading();
  }
}

// ==================== WIFI FUNCTIONS ====================
void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));  // Blink LED while connecting
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    digitalWrite(LED_PIN, HIGH);  // LED on when connected
  } else {
    Serial.println("\nWiFi connection failed!");
    digitalWrite(LED_PIN, LOW);
  }
}

// ==================== FIREBASE FUNCTIONS ====================
void initFirebase() {
  Serial.println("Initializing Firebase...");
  
  // Configure Firebase
  config.database_url = FIREBASE_HOST;
  
  // For public database (no authentication)
  if (strlen(FIREBASE_AUTH) == 0) {
    Serial.println("Using anonymous access (public database)");
    Firebase.signUp(&config, &auth, "", "");
  } else {
    config.signer.tokens.legacy_token = FIREBASE_AUTH;
  }
  
  // Set token status callback
  config.token_status_callback = tokenStatusCallback;
  
  // Initialize Firebase
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  // Wait for Firebase to be ready
  int attempts = 0;
  while (!Firebase.ready() && attempts < 10) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (Firebase.ready()) {
    Serial.println("\nFirebase connected!");
    firebaseConnected = true;
  } else {
    Serial.println("\nFirebase connection failed!");
    firebaseConnected = false;
  }
}

void setInitialState() {
  if (!firebaseConnected) return;
  
  // Set charger as online
  String onlinePath = readingsPath + "/online";
  if (Firebase.RTDB.setBool(&fbdo, onlinePath.c_str(), true)) {
    Serial.println("Set online status: true");
  }
  
  // Initialize control state to false (OFF)
  if (Firebase.RTDB.setBool(&fbdo, controlPath.c_str(), false)) {
    Serial.println("Set initial control state: OFF");
  }
  
  // Set initial current reading
  sendCurrentReading();
}

void checkControlState() {
  if (!firebaseConnected) return;
  
  if (Firebase.RTDB.getBool(&fbdo, controlPath.c_str())) {
    bool newState = fbdo.boolData();
    
    if (newState != chargerState) {
      chargerState = newState;
      digitalWrite(CONTROL_PIN, chargerState ? HIGH : LOW);
      
      Serial.print("Charger state changed to: ");
      Serial.println(chargerState ? "ON" : "OFF");
      
      // Blink LED to indicate state change
      for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, LOW);
        delay(100);
        digitalWrite(LED_PIN, HIGH);
        delay(100);
      }
    }
  } else {
    Serial.print("Failed to get control state: ");
    Serial.println(fbdo.errorReason());
  }
}

void sendCurrentReading() {
  if (!firebaseConnected) return;
  
  // Read current from ACS712
  float current = readCurrent();
  
  // Create JSON object for readings
  FirebaseJson json;
  json.set("current", current);
  json.set("timestamp/.sv", "timestamp");  // Server timestamp
  json.set("online", true);
  
  // Send to Firebase
  if (Firebase.RTDB.setJSON(&fbdo, readingsPath.c_str(), &json)) {
    Serial.print("Current: ");
    Serial.print(current, 2);
    Serial.println(" A");
  } else {
    Serial.print("Failed to send reading: ");
    Serial.println(fbdo.errorReason());
  }
}

// ==================== SENSOR FUNCTIONS ====================
float readCurrent() {
  // Take multiple readings for averaging
  const int numReadings = 20;
  long total = 0;
  
  for (int i = 0; i < numReadings; i++) {
    total += analogRead(ACS712_PIN);
    delay(1);
  }
  
  float avgReading = total / (float)numReadings;
  
  // Convert ADC reading to voltage
  float voltage = (avgReading / ADC_RESOLUTION) * ADC_VOLTAGE;
  
  // Convert voltage to current
  // ACS712 outputs VCC/2 (2.5V) at zero current
  // Current = (Voltage - ZeroPoint) / Sensitivity
  float current = (voltage - ACS712_ZERO_POINT) / ACS712_SENSITIVITY;
  
  // Only return positive values (we're measuring DC)
  if (current < 0) current = 0;
  
  return current;
}

// ==================== UTILITY FUNCTIONS ====================
