#include "esp_now_transport.h"

#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

namespace {

constexpr uint8_t BROADCAST_MAC[6] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr uint8_t MAX_RECEIVE_HANDLERS = 3;

portMUX_TYPE transportMux = portMUX_INITIALIZER_UNLOCKED;
EspNowTransport::ReceiveHandler receiveHandlers[MAX_RECEIVE_HANDLERS] = {};
bool ready = false;
SemaphoreHandle_t sendMutex = nullptr;
bool audioActive = false;

void dispatchReceived(
    const uint8_t* sourceMac,
    const uint8_t* data,
    size_t length) {
  EspNowTransport::ReceiveHandler handlers[MAX_RECEIVE_HANDLERS] = {};
  portENTER_CRITICAL(&transportMux);
  memcpy(handlers, receiveHandlers, sizeof(handlers));
  portEXIT_CRITICAL(&transportMux);

  for (uint8_t i = 0; i < MAX_RECEIVE_HANDLERS; ++i) {
    if (handlers[i] != nullptr) {
      handlers[i](sourceMac, data, length);
    }
  }
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowReceive(
    const esp_now_recv_info_t* info,
    const uint8_t* data,
    int length) {
  const uint8_t* sourceMac = info == nullptr ? nullptr : info->src_addr;
  dispatchReceived(sourceMac, data, length > 0 ? static_cast<size_t>(length) : 0);
}
#else
void onEspNowReceive(
    const uint8_t* sourceMac,
    const uint8_t* data,
    int length) {
  dispatchReceived(sourceMac, data, length > 0 ? static_cast<size_t>(length) : 0);
}
#endif

}  // namespace

namespace EspNowTransport {

bool begin() {
  portENTER_CRITICAL(&transportMux);
  const bool alreadyReady = ready;
  portEXIT_CRITICAL(&transportMux);
  if (alreadyReady) {
    return true;
  }

  if (sendMutex == nullptr) {
    sendMutex = xSemaphoreCreateMutex();
    if (sendMutex == nullptr) {
      Serial.println("[ESPNOW] send_mutex_create_failed");
      return false;
    }
  }

  const esp_err_t initResult = esp_now_init();
  if (initResult != ESP_OK && initResult != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf(
        "[ESPNOW] init_failed err=%d\n",
        static_cast<int>(initResult));
    return false;
  }

  const esp_err_t callbackResult = esp_now_register_recv_cb(onEspNowReceive);
  if (callbackResult != ESP_OK) {
    Serial.printf(
        "[ESPNOW] recv_callback_failed err=%d\n",
        static_cast<int>(callbackResult));
    return false;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, BROADCAST_MAC, sizeof(BROADCAST_MAC));
  // Channel zero follows the current Wi-Fi STA/home channel. This lets HTTPS
  // and ESP-NOW coexist without retuning the radio away from the access point.
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 4
  peerInfo.ifidx = WIFI_IF_STA;
#endif

  const esp_err_t peerResult = esp_now_add_peer(&peerInfo);
  if (peerResult != ESP_OK && peerResult != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf(
        "[ESPNOW] broadcast_peer_failed err=%d\n",
        static_cast<int>(peerResult));
    return false;
  }

  portENTER_CRITICAL(&transportMux);
  ready = true;
  portEXIT_CRITICAL(&transportMux);

  uint8_t homeChannel = 0;
  wifi_second_chan_t secondChannel = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&homeChannel, &secondChannel);
  Serial.printf(
      "[ESPNOW] ready peer=broadcast channel=current homeChannel=%u\n",
      static_cast<unsigned int>(homeChannel));
  return true;
}

bool isReady() {
  portENTER_CRITICAL(&transportMux);
  const bool snapshot = ready;
  portEXIT_CRITICAL(&transportMux);
  return snapshot;
}

bool registerReceiveHandler(ReceiveHandler handler) {
  if (handler == nullptr || !begin()) {
    return false;
  }

  bool registered = false;
  portENTER_CRITICAL(&transportMux);
  for (uint8_t i = 0; i < MAX_RECEIVE_HANDLERS; ++i) {
    if (receiveHandlers[i] == handler) {
      registered = true;
      break;
    }
    if (receiveHandlers[i] == nullptr) {
      receiveHandlers[i] = handler;
      registered = true;
      break;
    }
  }
  portEXIT_CRITICAL(&transportMux);
  return registered;
}

bool sendBroadcast(
    const void* data,
    size_t length,
    SendClass sendClass) {
  if (data == nullptr || length == 0 || !begin() || sendMutex == nullptr) {
    return false;
  }

  if (sendClass == SendClass::Availability && audioTrafficActive()) {
    return false;
  }

  const TickType_t waitTicks =
      sendClass == SendClass::Audio ? pdMS_TO_TICKS(20) : 0;
  if (xSemaphoreTake(sendMutex, waitTicks) != pdTRUE) {
    return false;
  }

  // Recheck after obtaining the lock so availability cannot start between an
  // audio-active transition and the mutex acquisition.
  if (sendClass == SendClass::Availability && audioTrafficActive()) {
    xSemaphoreGive(sendMutex);
    return false;
  }

  const bool sent = esp_now_send(
                        BROADCAST_MAC,
                        static_cast<const uint8_t*>(data),
                        length) == ESP_OK;
  xSemaphoreGive(sendMutex);
  return sent;
}

void setAudioTrafficActive(bool active) {
  portENTER_CRITICAL(&transportMux);
  audioActive = active;
  portEXIT_CRITICAL(&transportMux);
}

bool audioTrafficActive() {
  portENTER_CRITICAL(&transportMux);
  const bool snapshot = audioActive;
  portEXIT_CRITICAL(&transportMux);
  return snapshot;
}

}  // namespace EspNowTransport
