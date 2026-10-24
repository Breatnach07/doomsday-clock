#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "SSD1306Wire.h"

// --- Hardware Definitions ---
#define BUTTON_PIN 2 
SSD1306Wire display(0x3c, 12, 14); 

// --- Network & Server ---
const char* ssid = "INSERT WIFI SSID";        
const char* password = "INSERT WIFI PASSWORD";  
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

// --- Event Class Structure ---
class Event {
public:
  String name;
  unsigned long timestamp; 
  String type;
  bool isActive;
  bool isMain;

  Event() : name(""), timestamp(0), type(""), isActive(false), isMain(false) {}

  void setEvent(String n, unsigned long t, String ty, bool main) {
    name = n;
    timestamp = t;
    type = ty;
    isMain = main;
    isActive = true;
  }

  void destroy() {
    isActive = false;
    isMain = false;
  }

  long getSecondsRemaining(unsigned long currentUnixTime) {
    if (timestamp > currentUnixTime) {
      return timestamp - currentUnixTime;
    }
    return 0; 
  }
};

// --- Global State ---
#define MAX_EVENTS 10
Event events[MAX_EVENTS];
int currentEventIndex = 0;

// Display State Machine Variables
bool displayOn = true;
unsigned long lastCycleTime = 0;
bool justReturnedToMain = false;

// IP Display State
bool showingIP = false;
unsigned long ipDisplayStartTime = 0;

// Button Debounce & Double Click Variables
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;
unsigned long lastClickTime = 0;
const unsigned long doubleClickWindow = 400; 

// --- Helper Functions ---
int getMainEventIndex() {
  for (int i = 0; i < MAX_EVENTS; i++) {
    if (events[i].isActive && events[i].isMain) return i;
  }
  for (int i = 0; i < MAX_EVENTS; i++) {
    if (events[i].isActive) return i;
  }
  return -1; 
}

void advanceEventIndex() {
  if (getMainEventIndex() == -1) return; // Prevent infinite loop if empty
  
  int startIndex = currentEventIndex;
  do {
    currentEventIndex = (currentEventIndex + 1) % MAX_EVENTS;
    if (events[currentEventIndex].isActive) break;
  } while (currentEventIndex != startIndex);
}

// --- File System Storage ---
void saveEvents() {
  StaticJsonDocument<2048> doc;
  JsonArray array = doc.to<JsonArray>();

  for (int i = 0; i < MAX_EVENTS; i++) {
    if (events[i].isActive) {
      JsonObject obj = array.createNestedObject();
      obj["name"] = events[i].name;
      obj["timestamp"] = events[i].timestamp;
      obj["type"] = events[i].type;
      obj["isMain"] = events[i].isMain;
    }
  }

  File file = LittleFS.open("/events.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

void loadEvents() {
  if (!LittleFS.exists("/events.json")) return;
  
  File file = LittleFS.open("/events.json", "r");
  if (!file) return;

  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) return;

  JsonArray array = doc.as<JsonArray>();
  int i = 0;
  for (JsonObject obj : array) {
    if (i >= MAX_EVENTS) break;
    events[i].setEvent(
      obj["name"].as<String>(),
      obj["timestamp"].as<unsigned long>(),
      obj["type"].as<String>(),
      obj["isMain"].as<bool>()
    );
    i++;
  }
  
  currentEventIndex = getMainEventIndex();
  if (currentEventIndex == -1) currentEventIndex = 0;
}

// --- Web Server Endpoints ---
void handleUpdateEvents() {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  for (int i = 0; i < MAX_EVENTS; i++) {
    if (!events[i].isActive) {
      events[i].setEvent(
        doc["name"].as<String>(),
        doc["timestamp"].as<unsigned long>(),
        doc["type"].as<String>(),
        doc["isMain"].as<bool>()
      );
      
      if (events[i].isMain) {
        for(int j=0; j<MAX_EVENTS; j++) {
          if (j != i) events[j].isMain = false;
        }
        currentEventIndex = i; 
      }
      
      saveEvents(); // Persist to flash
      server.send(200, "application/json", "{\"status\":\"success\"}");
      return;
    }
  }
  server.send(507, "text/plain", "Event array full");
}

void handleGetEvents() {
  StaticJsonDocument<2048> doc;
  JsonArray array = doc.to<JsonArray>();

  for (int i = 0; i < MAX_EVENTS; i++) {
    if (events[i].isActive) {
      JsonObject obj = array.createNestedObject();
      obj["id"] = i;
      obj["name"] = events[i].name;
      obj["timestamp"] = events[i].timestamp;
      obj["type"] = events[i].type;
      obj["isMain"] = events[i].isMain;
    }
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleDeleteEvent() {
  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "Missing event id");
    return;
  }
  
  int id = server.arg("id").toInt();
  if (id >= 0 && id < MAX_EVENTS) {
    events[id].destroy();
    
    if (currentEventIndex == id) {
      currentEventIndex = getMainEventIndex();
      if (currentEventIndex == -1) currentEventIndex = 0; 
    }
    
    saveEvents(); // Persist to flash
    server.send(200, "application/json", "{\"status\":\"deleted\"}");
  } else {
    server.send(400, "text/plain", "Invalid id");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  display.init();
  display.flipScreenVertically();
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  
  // Initialize File System
  if (!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed");
  } else {
    loadEvents(); // Restore events from flash
  }

  display.drawString(64, 20, "Connecting...");
  display.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  Serial.println();
  Serial.print("WiFi connected. IP address: ");
  Serial.println(WiFi.localIP());

  display.clear();
  display.drawString(64, 15, "IP Address:");
  display.drawString(64, 35, WiFi.localIP().toString());
  display.display();
  delay(10000); 

  timeClient.begin();
  
  server.on("/api/events", HTTP_POST, handleUpdateEvents);
  server.on("/api/events", HTTP_GET, handleGetEvents);
  server.on("/api/events", HTTP_DELETE, handleDeleteEvent);
  server.begin();

  lastCycleTime = millis();
}

void loop() {
  server.handleClient();
  timeClient.update();

  unsigned long currentMillis = millis();
  unsigned long currentUnixTime = timeClient.getEpochTime();

  // --- Button Handling (Edge Detection & Double Click) ---
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastDebounceTime = currentMillis;
  }

  if ((currentMillis - lastDebounceTime) > debounceDelay) {
    static bool validatedButtonState = HIGH;
    
    if (reading != validatedButtonState) {
      validatedButtonState = reading;
      
      if (validatedButtonState == LOW) { // Validated Button Press
        if (currentMillis - lastClickTime < doubleClickWindow) {
          // Double Click Detected
          showingIP = true;
          ipDisplayStartTime = currentMillis;
          displayOn = true;
          display.displayOn();
        } else {
          // Single Click Detected
          if (!showingIP) {
            if (!displayOn) {
              displayOn = true;
              display.displayOn();
              currentEventIndex = getMainEventIndex();
              if (currentEventIndex == -1) currentEventIndex = 0;
              lastCycleTime = currentMillis;
              justReturnedToMain = true;
            } else {
              advanceEventIndex();
              lastCycleTime = currentMillis;
              justReturnedToMain = false;
            }
          }
        }
        lastClickTime = currentMillis;
      }
    }
  }
  lastButtonState = reading;

  // --- IP Override Display ---
  if (showingIP) {
    if (currentMillis - ipDisplayStartTime > 10000) {
      showingIP = false;
      lastCycleTime = currentMillis; 
      display.clear();
    } else {
      display.clear();
      display.setFont(ArialMT_Plain_16);
      display.drawString(64, 15, "IP Address:");
      display.drawString(64, 35, WiFi.localIP().toString());
      display.display();
      return; 
    }
  }

  // --- State Machine & UI Cycling ---
  if (displayOn && !showingIP) {
    int activeMainIndex = getMainEventIndex();
    
    if (activeMainIndex == -1) {
      display.clear();
      display.drawString(64, 20, "No Events");
      display.display();
      return;
    }

    bool isCurrentMain = (currentEventIndex == activeMainIndex);
    unsigned long interval = isCurrentMain ? 10000 : 5000;

    if (currentMillis - lastCycleTime > interval) {
      if (isCurrentMain && justReturnedToMain) {
        displayOn = false;
        display.displayOff();
      } else {
        advanceEventIndex();
        lastCycleTime = currentMillis;
        justReturnedToMain = (currentEventIndex == activeMainIndex);
      }
    }

    if (displayOn) {
      display.clear();
      display.setFont(ArialMT_Plain_16);
      display.drawString(64, 0, events[currentEventIndex].name);

      long secsLeft = events[currentEventIndex].getSecondsRemaining(currentUnixTime);
      int days = secsLeft / 86400;
      int hours = (secsLeft % 86400) / 3600;
      
      String timeStr = String(days) + "d " + String(hours) + "h";
      display.setFont(ArialMT_Plain_24);
      display.drawString(64, 30, timeStr);
      
      display.display();
    }
  }
}
