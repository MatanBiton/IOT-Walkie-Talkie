#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// ================= WIFI =================
const char* WIFI_SSID = "GalaxyA31948B";
const char* WIFI_PASSWORD = "enxu9794";

// ================= FIREBASE =================
const char* FIREBASE_DB =
"https://walki-talkie-37ed3-default-rtdb.europe-west1.firebasedatabase.app/";

const char* FIREBASE_API_KEY = "AIzaSyBd_mLyDFaZesLOGV7K0pxPD_M3LD2fQ6U";
const char* FIREBASE_EMAIL = "elironaviron@gmail.com";
const char* FIREBASE_PASSWORD = "Arduino123";

// ================= PATH =================
#define ROOM_ID "room1"
#define SESSION_ID "4416"

// ================= AUDIO =================
#define SAMPLE_RATE 8000

#define I2S_BCLK 27
#define I2S_WS   26
#define I2S_SD   22

#define SPEAKER_GAIN  1

// ================= STATE =================
String idToken = "";
unsigned long tokenExpire = 0;
int lastSeq = -1;

// =====================================================
// BASE64 DECODE
// =====================================================
int base64_decode_audio(const char* input, uint8_t* output) {
    size_t outLen;
    mbedtls_base64_decode(output, 4096, &outLen,
                          (const unsigned char*)input,
                          strlen(input));
    return outLen;
}

// =====================================================
// I2S SETUP
// =====================================================
void setupI2S() {

    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false
    };

    i2s_pin_config_t pins = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_SD,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pins);
}

// =====================================================
// WIFI
// =====================================================
void connectWiFi() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }

    Serial.println("\nWiFi connected");
    WiFi.setSleep(false);
}

// =====================================================
// FIREBASE LOGIN
// =====================================================
bool firebaseLogin() {

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;

    String url =
        String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=")
        + FIREBASE_API_KEY;

    if (!https.begin(client, url)) return false;

    https.addHeader("Content-Type", "application/json");

    String body =
        String("{\"email\":\"") + FIREBASE_EMAIL +
        "\",\"password\":\"" + FIREBASE_PASSWORD +
        "\",\"returnSecureToken\":true}";

    int code = https.POST(body);

    if (code != 200) {
        Serial.println(https.getString());
        https.end();
        return false;
    }

    String res = https.getString();
    https.end();

    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, res)) return false;

    idToken = doc["idToken"].as<String>();
    tokenExpire = millis() + 3300 * 1000;

    Serial.println("[Firebase] login OK");
    return true;
}

// =====================================================
// SAFE LOGIN
// =====================================================
bool safeLogin() {
    for (int i = 0; i < 5; i++) {
        if (firebaseLogin()) return true;
        delay(1000);
    }
    return false;
}

// =====================================================
// FETCH CHUNKS
// =====================================================
String fetchChunks() {

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    String url =
        String(FIREBASE_DB) +
        "rooms/" + ROOM_ID +
        "/sessions/" + SESSION_ID +
        "/chunks.json?auth=" + idToken;

    if (!http.begin(client, url)) {
        Serial.println("[Firebase] begin failed");
        return "";
    }

    http.setReuse(false);

    int code = http.GET();

    Serial.printf("[Firebase] HTTP code: %d\n", code);

    String payload = http.getString();

    Serial.printf("[Firebase] payload length: %d\n", payload.length());
    Serial.println(payload);

    http.end();

    delay(200);

    return payload;
}
// =====================================================
// PLAY AUDIO
// =====================================================
void playAudio(const char* b64) {

    uint8_t decoded[4096];

    int len = base64_decode_audio(b64, decoded);

    size_t written;
    i2s_write(I2S_NUM_0, decoded, len, &written, portMAX_DELAY);
}

// =====================================================
// POLL FIREBASE
// =====================================================
void pollFirebase() {

    String res = fetchChunks();
    if (res == "" || res == "null" || res == "{}") {
        Serial.println("[Firebase] empty response");
        return;
    }

    Serial.println("[RAW]");
    Serial.println(res);

    DynamicJsonDocument doc(12000);
    DeserializationError err = deserializeJson(doc, res);

    if (err) {
        Serial.print("[JSON ERROR] ");
        Serial.println(err.c_str());
        return;
    }

    JsonObject root = doc.as<JsonObject>();


    for (JsonPair kv : root) {

        JsonObject obj = kv.value();

        int seq = obj["seq"] | -1;
        const char* data = obj["data"] | nullptr;

        if (!data) continue;

        Serial.printf("[CHUNK] seq=%d\n", seq);

        if (seq <= lastSeq) continue;

        lastSeq = seq;
        playAudio(data);
    }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
    Serial.begin(115200);

    connectWiFi();
    setupI2S();

    if (!safeLogin()) {
        Serial.println("[FATAL] Firebase login failed");
        while (true) delay(1000);
    }

    Serial.println("Receiver ready");
}

// =====================================================
// LOOP
// =====================================================
void loop() {

    if (idToken.length() == 0 || millis() > tokenExpire) {
        safeLogin();
    }

    pollFirebase();

    delay(150);
}