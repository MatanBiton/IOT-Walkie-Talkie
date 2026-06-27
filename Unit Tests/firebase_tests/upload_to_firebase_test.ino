#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"
#include <ArduinoJson.h>

// =====================================================
// CONFIG
// =====================================================

const char* WIFI_SSID = "GalaxyA31948B";
const char* WIFI_PASSWORD = "enxu9794";

const char* FIREBASE_DB =
"https://walki-talkie-37ed3-default-rtdb.europe-west1.firebasedatabase.app/";

const char* FIREBASE_API_KEY = "AIzaSyBd_mLyDFaZesLOGV7K0pxPD_M3LD2fQ6U";
const char* FIREBASE_EMAIL = "elironaviron@gmail.com";
const char* FIREBASE_PASSWORD = "Arduino123";

#define ROOM_ID "room1"
#define DEVICE_ID "esp32-audio-01"

// =====================================================
// AUDIO
// =====================================================

#define SAMPLE_RATE 8000
#define CHUNK_MS 250
#define CHUNK_SAMPLES ((SAMPLE_RATE * CHUNK_MS) / 1000)


// =====================================================
// BUTTON
// =====================================================
#define BTN_PIN 14   // change if needed
bool recording = false;
#define LED_PIN 4

bool btnStable = HIGH;
bool lastRawBtn = HIGH;
unsigned long lastBtnChange = 0;

// =====================================================
// STATE
// =====================================================

uint32_t streamId = 0;
uint32_t seq = 0;

String idToken = "";
unsigned long tokenExpire = 0;

unsigned long lastAudioSend = 0;

// =====================================================
// WIFI
// =====================================================

void connectWiFi() {
    Serial.println("\n[WiFi] Connecting...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int tries = 0;

    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
        if (++tries > 30) {
            Serial.println("\n[WiFi] FAILED → reboot");
            ESP.restart();
        }
    }

    Serial.println("\n[WiFi] CONNECTED");
    Serial.println(WiFi.localIP());

    delay(2000); // IMPORTANT stability delay (fixes TLS crashes)
    WiFi.setSleep(false);
}

// =====================================================
// SAFE JSON FIELD EXTRACTION (NO CRASH SUBSTRING)
// =====================================================

String extractField(String json, String key) {

    DynamicJsonDocument doc(8192);

    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.print("[JSON ERROR] ");
        Serial.println(err.c_str());
        return "";
    }

    if (!doc.containsKey(key)) {
        Serial.println("[JSON] key not found: " + key);
        return "";
    }

    return doc[key].as<String>();
}

// =====================================================
// FIREBASE LOGIN (CRASH SAFE)
// =====================================================

bool firebaseLogin() {

    if (idToken.length() > 0 && millis() < tokenExpire) {
        return true;
    }

    Serial.println("\n[Firebase] login attempt...");

    WiFiClientSecure client;
    client.setInsecure();

    static HTTPClient https;

    String url =
        String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=")
        + FIREBASE_API_KEY;

    if (!https.begin(client, url)) {
        Serial.println("[Firebase] begin failed");
        return false;
    }

    https.addHeader("Content-Type", "application/json");
    https.addHeader("Connection", "keep-alive");

    String body = "{";
    body += "\"email\":\"" + String(FIREBASE_EMAIL) + "\",";
    body += "\"password\":\"" + String(FIREBASE_PASSWORD) + "\",";
    body += "\"returnSecureToken\":true";
    body += "}";

    int code = https.POST(body);

    if (code != 200) {
        Serial.printf("[Firebase] HTTP FAIL: %d\n", code);
        String err = https.getString();
        Serial.println(err);
        https.end();
        return false;
    }

    String res = https.getString();
    https.end();

    Serial.println("[Firebase] response received");


    // SAFE extraction (NO CRASH)
    String token = extractField(res, "idToken");

    if (token.length() < 50) {
        Serial.println("[Firebase] token invalid or missing");
        Serial.println(token);
        return false;
    }


    idToken = token;
    tokenExpire = millis() + 3300 * 1000;

    Serial.println("[Firebase] login success");
    return true;
}

// =====================================================
// BASE64
// =====================================================

char* b64(const uint8_t* data, size_t len) {

    size_t outLen;
    size_t cap = 4 * ((len + 2) / 3) + 1;

    char* out = (char*)malloc(cap);
    if (!out) return nullptr;

    mbedtls_base64_encode((unsigned char*)out, cap, &outLen, data, len);
    out[outLen] = 0;

    return out;
}

// =====================================================
// MIC (placeholder)
// =====================================================

void readMic(int16_t* out) {
    for (int i = 0; i < CHUNK_SAMPLES; i++) {
        out[i] = random(-200, 200);
    }
}

// =====================================================
// SEND CHUNK
// =====================================================

bool sendChunk(uint32_t streamId, uint32_t seq, int16_t* data) {

    size_t rawBytes = CHUNK_SAMPLES * 2;
    char* encoded = b64((uint8_t*)data, rawBytes);

    if (!encoded) return false;

    String json = "{";
    json += "\"streamId\":" + String(streamId) + ",";
    json += "\"seq\":" + String(seq) + ",";
    json += "\"device\":\"" + String(DEVICE_ID) + "\",";
    json += "\"data\":\"" + String(encoded) + "\"";
    json += "}";

    free(encoded);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    String url =
        String(FIREBASE_DB) +
        "/rooms/" + ROOM_ID +
        "/sessions/" + String(streamId) +
        "/chunks/" + String(seq) +
        ".json?auth=" + idToken;

    unsigned long t0 = millis();

    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

    int code = http.PUT(json);

    http.end();

    Serial.printf("[TX] seq=%lu HTTP=%d time=%lums\n", seq, code, millis() - t0);

    return (code == 200 || code == 204);
}

// =====================================================
// SESSION
// =====================================================

void startSession() {

    streamId = millis();
    seq = 0;

    Serial.println("\n========================");
    Serial.println("[SESSION START]");
    Serial.printf("streamId=%lu\n", streamId);
    Serial.println("========================");
}

// =====================================================
// SAFE LOGIN RETRY WRAPPER (IMPORTANT FIX)
// =====================================================

bool safeLogin() {

    for (int i = 0; i < 5; i++) {

        if (firebaseLogin()) return true;

        Serial.printf("[Firebase] retry %d\n", i + 1);
        delay(1000 * (i + 1));
    }

    return false;
}

// =====================================================
// SETUP
// =====================================================

void setup() {
    Serial.begin(115200);
    pinMode(BTN_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    connectWiFi();

    if (!safeLogin()) {
        Serial.println("[FATAL] Firebase login failed permanently");
        while (true) delay(1000);
    }

    startSession();
}

// =====================================================
// LOOP
// =====================================================

void loop() {

    // -------------------------
    // BUTTON HANDLING (ALWAYS FAST)
    // -------------------------
    bool raw = digitalRead(BTN_PIN);

    // debounce tracking
    if (raw != lastRawBtn) {
        lastBtnChange = millis();
        lastRawBtn = raw;
    }

    // stable state update
    if (millis() - lastBtnChange > 40) {

        if (raw != btnStable) {
            btnStable = raw;

            // PRESS (HIGH -> LOW)
            if (btnStable == LOW) {
                recording = true;
                Serial.println("[AUDIO] START");
            }

            // RELEASE (LOW -> HIGH)
            else {
                recording = false;
                Serial.println("[AUDIO] STOP");
            }
        }
    }

    // LED always follows state
    digitalWrite(LED_PIN, recording ? HIGH : LOW);

    // -------------------------
    // FIREBASE CHECK (LIGHTWEIGHT)
    // -------------------------
    if (idToken.length() == 0 || millis() > tokenExpire) {
        if (!safeLogin()) {
            delay(1000);
            return;
        }
    }

    // -------------------------
    // AUDIO SENDING (NON-BLOCKING)
    // -------------------------
    if (recording && millis() - lastAudioSend >= CHUNK_MS) {

        lastAudioSend = millis();

        int16_t buffer[CHUNK_SAMPLES];
        readMic(buffer);
        sendChunk(streamId, seq, buffer);
        seq++;
    }
}