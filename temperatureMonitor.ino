/*
  ESP32 FreeRTOS + DHT11 -> Firestore with Queue + Wi-Fi recovery + TimeSync
  - Uploads every 1 hour
  - Sleeps from 9 PM to 8 AM automatically
  - Uses FreeRTOS tasks for Wi-Fi, upload, network monitor, and time sync
  - Queue stores unsent readings
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <ESP32Time.h>
#include "esp_sleep.h"
#include "freertos/queue.h"

// ---------- User config ----------
const char* ssid = "<YOUR_WIFI_SSID>";
const char* password = "<YOUR_WIFI_PASSWORD>";
const char* apiKey = "<YOUR_FIREBASE_API_KEY>";
const char* projectId = "<YOUR_FIREBASE_PROJECT_ID>";
const char* collectionPath = "<YOUR_COLLECTION_PATH>";

#define DHTPIN 23
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
ESP32Time rtc;

// ---------- FreeRTOS handles ----------
TaskHandle_t wifiTaskHandle = nullptr;
TaskHandle_t uploaderTaskHandle = nullptr;
TaskHandle_t netMonitorTaskHandle = nullptr;
TaskHandle_t timeSyncTaskHandle = nullptr;

// ---------- Queue ----------
#define QUEUE_LENGTH 10
#define QUEUE_ITEM_SIZE sizeof(String)
QueueHandle_t dataQueue;

// ---------- Constants ----------
const uint64_t HOUR_US = 3600ULL * 1000000ULL;
const uint64_t SHORT_SLEEP_US = 5ULL * 60ULL * 1000000ULL;
const uint64_t RETRY_10MIN_US = 10ULL * 60ULL * 1000000ULL;

// ---------- Function prototypes ----------
String getFormattedTime();
String getDaySuffix(int day);
String getDayOfWeek(int dayOfWeek);
String getMonthString(int month);
void goToDeepSleepFor(uint64_t us);
bool dataQueueIsEmpty();
bool validateTime();

// ---------- Wi-Fi Task ----------
void wifiTask(void* pvParameters) {
  Serial.println("WiFi: connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 60000) {
    Serial.print(".");
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("WiFi connect failed.");
  }

  // Notify time sync task
  xTaskNotifyGive(timeSyncTaskHandle);
  vTaskSuspend(NULL);
}

// ---------- Time Sync Task ----------
void timeSyncTask(void* pvParameters) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    struct tm timeinfo;
    bool synced = false;
    int attempts = 0;

    while (!synced && attempts < 5) {
      if (getLocalTime(&timeinfo)) {
        rtc.setTimeStruct(timeinfo);
        if (rtc.getYear() >= 2024) {
          Serial.println("Time sync valid: " + getFormattedTime());
          synced = true;
        } else {
          Serial.println("Time year invalid, retrying NTP sync...");
        }
      } else {
        Serial.println("Failed NTP sync, retrying...");
      }
      attempts++;
      if (!synced) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    if (!synced) {
      Serial.println("NTP sync failed after retries, will try later");
    }

    // Notify uploader to continue
    xTaskNotifyGive(uploaderTaskHandle);
  }
}

// ---------- Uploader Task ----------
void uploaderTask(void* pvParameters) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Validate time before sending
    if (!validateTime()) {
      Serial.println("Invalid time, triggering WiFi reconnect for proper NTP...");
      xTaskNotifyGive(wifiTaskHandle);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    int hourNow = rtc.getHour(true);
    if (hourNow >= 21 || hourNow < 8) {
      int sleepHours = (hourNow >= 21) ? (24 - hourNow + 8) : (8 - hourNow);
      goToDeepSleepFor((uint64_t)sleepHours * HOUR_US);
    }

    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    if (isnan(temperature) || isnan(humidity)) {
      Serial.println("DHT read failed, sleeping 5 min...");
      goToDeepSleepFor(SHORT_SLEEP_US);
    }

    String humidity_quality;
    if (humidity <= 20) humidity_quality = "Very Dry";
    else if (humidity <= 35) humidity_quality = "Dry";
    else if (humidity <= 50) humidity_quality = "Normal";
    else if (humidity <= 65) humidity_quality = "Slightly Humid";
    else if (humidity <= 75) humidity_quality = "Humid";
    else if (humidity <= 85) humidity_quality = "Very Humid";
    else humidity_quality = "Saturated";

    DynamicJsonDocument doc(1024);
    doc["fields"]["temperature"]["doubleValue"] = temperature;
    doc["fields"]["humidity"]["doubleValue"] = humidity;
    doc["fields"]["humidity_quality"]["stringValue"] = humidity_quality;
    doc["fields"]["timestamp"]["stringValue"] = getFormattedTime();
    doc["fields"]["device_id"]["stringValue"] = "esp32_node_01";
    doc["fields"]["rssi"]["integerValue"] = WiFi.RSSI();

    String jsonData;
    serializeJson(doc, jsonData);

    if (xQueueSend(dataQueue, &jsonData, pdMS_TO_TICKS(10)) != pdPASS) {
      Serial.println("Queue full, discarding oldest value");
      String dummy;
      xQueueReceive(dataQueue, &dummy, 0);
      xQueueSend(dataQueue, &jsonData, 0);
    }

    xTaskNotifyGive(netMonitorTaskHandle);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ---------- Network Monitor Task ----------
void netMonitorTask(void* pvParameters) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    int hourNow = rtc.getHour(true);
    uint64_t retry_us = (hourNow == 8) ? HOUR_US : RETRY_10MIN_US;

    while (!dataQueueIsEmpty()) {
      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("Wi-Fi down, retry in %.0f minutes\n", (double)retry_us/60000000.0);
        goToDeepSleepFor(retry_us);
      }

      String jsonData;
      xQueueReceive(dataQueue, &jsonData, 0);

      String url = "https://firestore.googleapis.com/v1/projects/";
      url += projectId;
      url += "/databases/(default)/documents/";
      url += collectionPath;
      url += "?key=";
      url += apiKey;

      HTTPClient http;
      http.begin(url);
      http.addHeader("Content-Type", "application/json");
      int httpCode = http.POST(jsonData);
      String payload = http.getString();
      http.end();

      if (httpCode == 200 || httpCode == 201) {
        Serial.println("Upload success from queue");
      } else {
        Serial.println("Upload failed, requeueing");
        xQueueSend(dataQueue, &jsonData, 0);
        goToDeepSleepFor(retry_us);
      }
    }

    goToDeepSleepFor(HOUR_US);
  }
}

// ---------- Helpers ----------
bool dataQueueIsEmpty() { return uxQueueMessagesWaiting(dataQueue) == 0; }

bool validateTime() { return rtc.getYear() >= 2024; }

void goToDeepSleepFor(uint64_t us) {
  Serial.printf("Deep sleep for %.2f minutes\n", (double)us / 60000000.0);
  esp_sleep_enable_timer_wakeup(us);
  Serial.flush();
  esp_deep_sleep_start();
}

// ---------- Arduino Setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  dht.begin();
  dataQueue = xQueueCreate(QUEUE_LENGTH, QUEUE_ITEM_SIZE);

  Serial.printf("Wakeup reason: %d\n", esp_sleep_get_wakeup_cause());

  xTaskCreatePinnedToCore(uploaderTask, "uploaderTask", 8192, NULL, 2, &uploaderTaskHandle, 1);
  delay(10);
  xTaskCreatePinnedToCore(wifiTask, "wifiTask", 4096, NULL, 1, &wifiTaskHandle, 1);
  delay(10);
  xTaskCreatePinnedToCore(netMonitorTask, "netMonitorTask", 8192, NULL, 1, &netMonitorTaskHandle, 1);
  delay(10);
  xTaskCreatePinnedToCore(timeSyncTask, "timeSyncTask", 4096, NULL, 3, &timeSyncTaskHandle, 1);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }

// ---------- Time Helpers ----------
String getFormattedTime() {
  int hour24 = rtc.getHour(true);
  int hour12 = hour24 % 12;
  if (hour12 == 0) hour12 = 12;
  String meridiem = (hour24 >= 12) ? "PM" : "AM";
  String dayOfWeek = getDayOfWeek(rtc.getDayofWeek());
  String month = getMonthString(rtc.getMonth());
  int day = rtc.getDay();
  String suffix = getDaySuffix(day);
  String minuteStr = (rtc.getMinute() < 10) ? "0" + String(rtc.getMinute()) : String(rtc.getMinute());
  return String(hour12) + ":" + minuteStr + " " + meridiem + "  " +
         dayOfWeek + ", " + String(day) + suffix + " " + month + " " + String(rtc.getYear());
}

String getDaySuffix(int day) { if (day >= 11 && day <= 13) return "th"; switch(day%10){case 1:return"st";case2:return"nd";case3:return"rd";default:return"th";} }
String getDayOfWeek(int dayOfWeek) { const char* days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}; return (dayOfWeek>=0 && dayOfWeek<7)?days[dayOfWeek]:"Invalid"; }
String getMonthString(int month) { const char* months[]={"January","February","March","April","May","June","July","August","September","October","November","December"}; return (month>=0 && month<12)?months[month]:"Invalid"; }
