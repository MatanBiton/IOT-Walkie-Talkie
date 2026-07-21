#include "availability_service.h"

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_config.h"
#include "esp_now_transport.h"
#include "p2p_audio.h"
#include "rtdb_request_service.h"
#include "wifi_connection.h"

namespace {

constexpr uint32_t UNKNOWN_AGE_SECONDS = 0xffffffffUL;
constexpr uint16_t PRESENCE_MAGIC = 0xA17A;
constexpr uint8_t PRESENCE_VERSION = 1;
constexpr uint8_t WIRE_DISCOVER = 1;
constexpr uint8_t WIRE_REPLY = 2;

struct PresenceWirePacket {
  uint16_t magic;
  uint8_t version;
  uint8_t type;
  uint32_t scanId;
  uint32_t bootId;
  uint8_t senderUser;
  uint8_t logicalChannel;
  uint8_t state;
  uint8_t transport;
} __attribute__((packed));

static_assert(
    sizeof(PresenceWirePacket) <= 250,
    "Availability packet exceeds ESP-NOW payload limit");

struct InternalUserStatus {
  uint8_t userNumber = 0;
  AvailabilityService::EvidenceState voip =
      AvailabilityService::EvidenceState::Unknown;
  AvailabilityService::EvidenceState p2p =
      AvailabilityService::EvidenceState::Unknown;
  uint32_t voipAgeSeconds = UNKNOWN_AGE_SECONDS;
  uint32_t p2pAgeSeconds = UNKNOWN_AGE_SECONDS;
  uint64_t lastVoipServerMs = 0;
  uint32_t lastP2pSeenMs = 0;
  uint32_t lastP2pScanId = 0;
};

InternalUserStatus users[AvailabilityConfig::USER_COUNT];
portMUX_TYPE availabilityMux = portMUX_INITIALIZER_UNLOCKED;

bool running = false;
uint8_t selfUser = 0;
uint32_t bootId = 0;
uint32_t serviceStartedMs = 0;

uint8_t currentChannel = 0;
bool currentCommunicationBusy = false;
uint8_t currentTransport = 0;
uint8_t currentPresenceState = 0;
bool contextInitialized = false;

bool lastWifiConnected = false;
bool leaseAwaiting = false;
bool leaseRescheduleRequested = false;
bool leaseRescheduleForce = false;
bool leaseDirty = true;
bool lastLeaseOperationSuccess = false;
uint32_t lastLeaseSuccessMs = 0;
uint32_t leaseRetryNotBeforeMs = 0;
char leasePayload[AvailabilityConfig::LEASE_PAYLOAD_BUFFER_BYTES] = {0};

bool usersReadAwaiting = false;
uint32_t lastSnapshotReceivedMs = 0;
bool havePresenceSnapshot = false;
bool snapshotReadFailed = false;
RtdbRequestService::PresenceSnapshot lastPresenceSnapshot;

AvailabilityService::RefreshState currentRefreshState =
    AvailabilityService::RefreshState::Idle;
bool refreshActive = false;
bool refreshWaitingForLease = false;
bool refreshRtdbDone = false;
bool refreshP2pDone = false;
bool refreshHadFailure = false;

bool scanRequested = false;
bool scanActive = false;
uint32_t activeScanId = 0;
uint32_t scanEndsAtMs = 0;
uint32_t scanRequestExpiresAtMs = 0;
uint8_t discoverRepeatsSent = 0;
uint32_t nextDiscoverSendAtMs = 0;
bool anyDiscoverSent = false;

bool replyPending = false;
uint32_t pendingReplyScanId = 0;
uint32_t pendingReplyDueMs = 0;
uint32_t pendingReplyExpiresAtMs = 0;

bool logEnabled() {
  return AvailabilityConfig::LOG_AVAILABILITY;
}

bool deadlineReached(uint32_t deadlineMs) {
  return deadlineMs != 0 &&
         static_cast<int32_t>(millis() - deadlineMs) >= 0;
}

uint8_t userNumberFromId(const char* id) {
  if (id == nullptr || id[0] < '0' || id[0] > '9' ||
      id[1] < '0' || id[1] > '9') {
    return 0;
  }
  const uint8_t value =
      static_cast<uint8_t>((id[0] - '0') * 10 + (id[1] - '0'));
  return value >= 1 && value <= AvailabilityConfig::USER_COUNT ? value : 0;
}

bool currentEpochMs(uint64_t& outEpochMs) {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0 ||
      tv.tv_sec < static_cast<time_t>(
                      AvailabilityConfig::CLOCK_VALID_EPOCH_SECONDS)) {
    outEpochMs = 0;
    return false;
  }
  outEpochMs = static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
               static_cast<uint64_t>(tv.tv_usec / 1000);
  return true;
}

uint8_t normalizedPresenceState(uint8_t channel, bool communicationBusy) {
  if (channel == 0) {
    return 0;  // Home/not joined.
  }
  return communicationBusy ? 2 : 1;  // Transmitting/busy or joined/listening.
}

void initializeUsers() {
  portENTER_CRITICAL(&availabilityMux);
  for (uint8_t index = 0;
       index < AvailabilityConfig::USER_COUNT;
       ++index) {
    users[index] = InternalUserStatus{};
    users[index].userNumber = index + 1;
  }
  portEXIT_CRITICAL(&availabilityMux);
}

bool buildLeasePayload() {
  const int written = snprintf(
      leasePayload,
      sizeof(leasePayload),
      "{\"schema\":%u,\"lastSeenServerMs\":{\".sv\":\"timestamp\"},"
      "\"bootId\":%lu,\"state\":%u,\"channel\":%u,\"transport\":%u}",
      static_cast<unsigned int>(AvailabilityConfig::SCHEMA_VERSION),
      static_cast<unsigned long>(bootId),
      static_cast<unsigned int>(currentPresenceState),
      static_cast<unsigned int>(currentChannel),
      static_cast<unsigned int>(currentTransport));
  return written > 0 && static_cast<size_t>(written) < sizeof(leasePayload);
}

void markSelfLeaseSuccess() {
  if (selfUser == 0) {
    return;
  }
  portENTER_CRITICAL(&availabilityMux);
  InternalUserStatus& self = users[selfUser - 1];
  self.voip = AvailabilityService::EvidenceState::Available;
  self.voipAgeSeconds = 0;
  portEXIT_CRITICAL(&availabilityMux);
}

bool leaseCanRunWithoutInterruptingCommunication() {
  // Never start an ordinary HTTPS presence operation while joined. Even an
  // idle P2P device may begin transmitting while a blocking TLS request is in
  // flight, so joined-state updates remain coalesced until the channel is left.
  // The optional maintenance path is the only explicit exception and pauses
  // VoIP SSE before forcing a bounded lease.
  return WifiConnection::isConnected() &&
         !currentCommunicationBusy &&
         currentChannel == 0;
}

bool submitLatestLease(bool force = false) {
  if (!running) {
    return false;
  }
  if (!force && !leaseCanRunWithoutInterruptingCommunication()) {
    leaseDirty = true;
    return true;
  }
  if (!force && leaseRetryNotBeforeMs != 0 &&
      !deadlineReached(leaseRetryNotBeforeMs)) {
    leaseDirty = true;
    return true;
  }
  if (!buildLeasePayload()) {
    return false;
  }
  if (leaseAwaiting) {
    leaseRescheduleRequested = true;
    leaseRescheduleForce = leaseRescheduleForce || force;
    leaseDirty = true;
    return true;
  }
  if (!RtdbRequestService::schedulePresenceLease(
          leasePayload,
          AvailabilityConfig::LEASE_REQUEST_VALID_MS)) {
    leaseDirty = true;
    return false;
  }
  leaseAwaiting = true;
  leaseDirty = false;
  if (logEnabled()) {
    Serial.printf(
        "[Availability][LEASE_SCHEDULED] state=%u channel=%u transport=%u force=%s\n",
        static_cast<unsigned int>(currentPresenceState),
        static_cast<unsigned int>(currentChannel),
        static_cast<unsigned int>(currentTransport),
        force ? "true" : "false");
  }
  return true;
}

void markVoipSnapshotFailed() {
  snapshotReadFailed = true;
  portENTER_CRITICAL(&availabilityMux);
  for (uint8_t index = 0;
       index < AvailabilityConfig::USER_COUNT;
       ++index) {
    if (users[index].voip == AvailabilityService::EvidenceState::Available) {
      users[index].voip = AvailabilityService::EvidenceState::Stale;
    }
  }
  portEXIT_CRITICAL(&availabilityMux);
}

void updateVoipFreshness() {
  uint64_t nowEpochMs = 0;
  const bool clockReady = currentEpochMs(nowEpochMs);
  const uint32_t nowMs = millis();

  portENTER_CRITICAL(&availabilityMux);
  for (uint8_t index = 0;
       index < AvailabilityConfig::USER_COUNT;
       ++index) {
    InternalUserStatus& user = users[index];

    if (index + 1 == selfUser && lastLeaseSuccessMs != 0) {
      const uint32_t localAgeMs = nowMs - lastLeaseSuccessMs;
      if (localAgeMs <= AvailabilityConfig::VOIP_LEASE_FRESH_MS) {
        user.voip = AvailabilityService::EvidenceState::Available;
        user.voipAgeSeconds = localAgeMs / 1000UL;
        continue;
      }
    }

    if (!havePresenceSnapshot) {
      continue;
    }
    const RtdbRequestService::PresenceRecord& record =
        lastPresenceSnapshot.users[index];
    user.lastVoipServerMs = record.lastSeenServerMs;
    if (!record.present || record.lastSeenServerMs == 0 || !clockReady) {
      user.voip = AvailabilityService::EvidenceState::Unknown;
      user.voipAgeSeconds = UNKNOWN_AGE_SECONDS;
      continue;
    }

    const uint64_t rawAgeMs =
        nowEpochMs >= record.lastSeenServerMs
            ? nowEpochMs - record.lastSeenServerMs
            : 0;
    user.voipAgeSeconds =
        rawAgeMs / 1000ULL > 0xffffffffULL
            ? 0xffffffffUL
            : static_cast<uint32_t>(rawAgeMs / 1000ULL);
    if (snapshotReadFailed) {
      user.voip = AvailabilityService::EvidenceState::Stale;
    } else {
      user.voip = rawAgeMs <= AvailabilityConfig::VOIP_LEASE_FRESH_MS
                      ? AvailabilityService::EvidenceState::Available
                      : AvailabilityService::EvidenceState::Stale;
    }
  }
  portEXIT_CRITICAL(&availabilityMux);
}

void applyPresenceSnapshot(
    const RtdbRequestService::PresenceSnapshot& snapshot) {
  lastPresenceSnapshot = snapshot;
  havePresenceSnapshot = true;
  snapshotReadFailed = false;
  lastSnapshotReceivedMs = millis();
  updateVoipFreshness();
}

void completeRefreshIfReady() {
  if (!refreshActive || !refreshRtdbDone || !refreshP2pDone) {
    return;
  }
  refreshActive = false;
  currentRefreshState = refreshHadFailure
                            ? AvailabilityService::RefreshState::Failed
                            : AvailabilityService::RefreshState::Complete;
  if (logEnabled()) {
    Serial.printf(
        "[Availability][REFRESH_DONE] state=%s snapshotAge=%lu\n",
        refreshHadFailure ? "failed" : "complete",
        static_cast<unsigned long>(
            lastSnapshotReceivedMs == 0
                ? UNKNOWN_AGE_SECONDS
                : (millis() - lastSnapshotReceivedMs) / 1000UL));
  }
}

void scheduleUsersReadAfterLease() {
  if (!refreshActive || !refreshWaitingForLease) {
    return;
  }
  refreshWaitingForLease = false;
  if (RtdbRequestService::schedulePresenceUsersRead(
          AvailabilityConfig::USERS_READ_REQUEST_VALID_MS)) {
    usersReadAwaiting = true;
    return;
  }
  refreshRtdbDone = true;
  refreshHadFailure = true;
  markVoipSnapshotFailed();
  completeRefreshIfReady();
}

void consumeRtdbResults() {
  bool leaseSuccess = false;
  if (RtdbRequestService::takePresenceLeaseResult(leaseSuccess)) {
    leaseAwaiting = false;
    lastLeaseOperationSuccess = leaseSuccess;
    if (leaseSuccess) {
      lastLeaseSuccessMs = millis();
      leaseRetryNotBeforeMs = 0;
      markSelfLeaseSuccess();
    } else {
      leaseDirty = true;
      leaseRetryNotBeforeMs =
          millis() + AvailabilityConfig::LEASE_FAILURE_RETRY_MS;
    }

    if (logEnabled()) {
      Serial.printf(
          "[Availability][LEASE_RESULT] success=%s reschedule=%s\n",
          leaseSuccess ? "true" : "false",
          leaseRescheduleRequested ? "true" : "false");
    }

    if (leaseRescheduleRequested) {
      const bool forceReschedule = leaseRescheduleForce;
      leaseRescheduleRequested = false;
      leaseRescheduleForce = false;
      if (!submitLatestLease(forceReschedule) && refreshActive) {
        refreshWaitingForLease = false;
        refreshRtdbDone = true;
        refreshHadFailure = true;
        completeRefreshIfReady();
      }
    } else {
      if (!leaseSuccess && refreshActive) {
        refreshHadFailure = true;
      }
      scheduleUsersReadAfterLease();
    }
  }

  bool usersSuccess = false;
  RtdbRequestService::PresenceSnapshot snapshot;
  if (RtdbRequestService::takePresenceUsersResult(usersSuccess, snapshot)) {
    usersReadAwaiting = false;
    if (usersSuccess) {
      applyPresenceSnapshot(snapshot);
    } else {
      markVoipSnapshotFailed();
      refreshHadFailure = true;
    }
    if (refreshActive) {
      refreshRtdbDone = true;
    }
    if (logEnabled()) {
      Serial.printf(
          "[Availability][USERS_RESULT] success=%s\n",
          usersSuccess ? "true" : "false");
    }
    completeRefreshIfReady();
  }
}

void markP2pReply(uint8_t userNumber, uint32_t scanId) {
  if (userNumber < 1 || userNumber > AvailabilityConfig::USER_COUNT) {
    return;
  }
  portENTER_CRITICAL(&availabilityMux);
  if (scanActive && scanId == activeScanId) {
    InternalUserStatus& user = users[userNumber - 1];
    user.lastP2pSeenMs = millis();
    user.lastP2pScanId = scanId;
    user.p2p = AvailabilityService::EvidenceState::Available;
    user.p2pAgeSeconds = 0;
  }
  portEXIT_CRITICAL(&availabilityMux);
}

void onEspNowReceive(
    const uint8_t*,
    const uint8_t* data,
    size_t length) {
  if (data == nullptr || length != sizeof(PresenceWirePacket)) {
    return;
  }

  PresenceWirePacket packet = {};
  memcpy(&packet, data, sizeof(packet));
  if (packet.magic != PRESENCE_MAGIC ||
      packet.version != PRESENCE_VERSION ||
      packet.senderUser < 1 ||
      packet.senderUser > AvailabilityConfig::USER_COUNT ||
      packet.senderUser == selfUser) {
    return;
  }

  if (packet.type == WIRE_REPLY) {
    markP2pReply(packet.senderUser, packet.scanId);
    return;
  }
  if (packet.type != WIRE_DISCOVER) {
    return;
  }

  const uint32_t nowMs = millis();
  const uint32_t jitter =
      AvailabilityConfig::P2P_REPLY_RANDOM_JITTER_MS == 0
          ? 0
          : (packet.scanId ^ bootId) %
                AvailabilityConfig::P2P_REPLY_RANDOM_JITTER_MS;
  const uint32_t dueMs =
      nowMs + selfUser * AvailabilityConfig::P2P_REPLY_USER_OFFSET_MS + jitter;

  portENTER_CRITICAL(&availabilityMux);
  if (!replyPending || pendingReplyScanId != packet.scanId) {
    replyPending = true;
    pendingReplyScanId = packet.scanId;
    pendingReplyDueMs = dueMs;
    pendingReplyExpiresAtMs = nowMs + AvailabilityConfig::P2P_SCAN_WINDOW_MS;
  }
  portEXIT_CRITICAL(&availabilityMux);
}

PresenceWirePacket makePresencePacket(uint8_t type, uint32_t scanId) {
  PresenceWirePacket packet = {};
  packet.magic = PRESENCE_MAGIC;
  packet.version = PRESENCE_VERSION;
  packet.type = type;
  packet.scanId = scanId;
  packet.bootId = bootId;
  packet.senderUser = selfUser;
  packet.logicalChannel = currentChannel;
  packet.state = currentPresenceState;
  packet.transport = currentTransport;
  return packet;
}

bool sendPresencePacket(uint8_t type, uint32_t scanId) {
  if (P2pAudio::isAudioBusy()) {
    return false;
  }
  const PresenceWirePacket packet = makePresencePacket(type, scanId);
  return EspNowTransport::sendBroadcast(
      &packet,
      sizeof(packet),
      EspNowTransport::SendClass::Availability);
}

void startP2pScanIfPossible() {
  if (!scanRequested || scanActive) {
    return;
  }
  if (deadlineReached(scanRequestExpiresAtMs)) {
    scanRequested = false;
    refreshP2pDone = true;
    completeRefreshIfReady();
    return;
  }
  if (P2pAudio::isAudioBusy() || !EspNowTransport::isReady()) {
    return;
  }

  scanRequested = false;
  scanActive = true;
  activeScanId = esp_random();
  if (activeScanId == 0) {
    activeScanId = 1;
  }
  scanEndsAtMs = millis() + AvailabilityConfig::P2P_SCAN_WINDOW_MS;
  discoverRepeatsSent = 0;
  nextDiscoverSendAtMs = millis();
  anyDiscoverSent = false;

  portENTER_CRITICAL(&availabilityMux);
  for (uint8_t index = 0;
       index < AvailabilityConfig::USER_COUNT;
       ++index) {
    InternalUserStatus& user = users[index];
    user.p2p = user.lastP2pSeenMs == 0
                   ? AvailabilityService::EvidenceState::Unknown
                   : AvailabilityService::EvidenceState::Stale;
    user.p2pAgeSeconds = user.lastP2pSeenMs == 0
                             ? UNKNOWN_AGE_SECONDS
                             : (millis() - user.lastP2pSeenMs) / 1000UL;
  }
  if (selfUser >= 1 && selfUser <= AvailabilityConfig::USER_COUNT) {
    InternalUserStatus& self = users[selfUser - 1];
    self.p2p = AvailabilityService::EvidenceState::Available;
    self.p2pAgeSeconds = 0;
    self.lastP2pSeenMs = millis();
    self.lastP2pScanId = activeScanId;
  }
  portEXIT_CRITICAL(&availabilityMux);

  if (logEnabled()) {
    Serial.printf(
        "[Availability][P2P_SCAN_START] scan=%lu windowMs=%u\n",
        static_cast<unsigned long>(activeScanId),
        static_cast<unsigned int>(AvailabilityConfig::P2P_SCAN_WINDOW_MS));
  }
}

void processPendingReply() {
  bool pending = false;
  uint32_t scanId = 0;
  uint32_t dueMs = 0;
  uint32_t expiresAtMs = 0;
  portENTER_CRITICAL(&availabilityMux);
  pending = replyPending;
  scanId = pendingReplyScanId;
  dueMs = pendingReplyDueMs;
  expiresAtMs = pendingReplyExpiresAtMs;
  portEXIT_CRITICAL(&availabilityMux);

  if (!pending) {
    return;
  }
  if (deadlineReached(expiresAtMs)) {
    portENTER_CRITICAL(&availabilityMux);
    if (replyPending && pendingReplyScanId == scanId) {
      replyPending = false;
    }
    portEXIT_CRITICAL(&availabilityMux);
    return;
  }
  if (!deadlineReached(dueMs)) {
    return;
  }
  if (P2pAudio::isAudioBusy()) {
    portENTER_CRITICAL(&availabilityMux);
    pendingReplyDueMs =
        millis() + AvailabilityConfig::P2P_REPLY_DEFER_STEP_MS;
    portEXIT_CRITICAL(&availabilityMux);
    return;
  }

  const bool sent = sendPresencePacket(WIRE_REPLY, scanId);
  portENTER_CRITICAL(&availabilityMux);
  if (replyPending && pendingReplyScanId == scanId) {
    if (sent) {
      replyPending = false;
    } else {
      pendingReplyDueMs =
          millis() + AvailabilityConfig::P2P_REPLY_DEFER_STEP_MS;
    }
  }
  portEXIT_CRITICAL(&availabilityMux);
}

void finishP2pScanIfDue() {
  if (!scanActive || !deadlineReached(scanEndsAtMs)) {
    return;
  }

  const uint32_t finishedScan = activeScanId;
  scanActive = false;
  portENTER_CRITICAL(&availabilityMux);
  for (uint8_t index = 0;
       index < AvailabilityConfig::USER_COUNT;
       ++index) {
    InternalUserStatus& user = users[index];
    if (user.lastP2pScanId != finishedScan && index + 1 != selfUser) {
      user.p2p = user.lastP2pSeenMs == 0
                     ? AvailabilityService::EvidenceState::Unknown
                     : AvailabilityService::EvidenceState::Stale;
    }
  }
  portEXIT_CRITICAL(&availabilityMux);

  if (refreshActive) {
    refreshP2pDone = true;
  }
  if (logEnabled()) {
    Serial.printf(
        "[Availability][P2P_SCAN_END] scan=%lu discoverSent=%s\n",
        static_cast<unsigned long>(finishedScan),
        anyDiscoverSent ? "true" : "false");
  }
  completeRefreshIfReady();
}

void pollP2p() {
  processPendingReply();
  startP2pScanIfPossible();

  if (scanActive &&
      discoverRepeatsSent < AvailabilityConfig::P2P_DISCOVER_REPEATS &&
      deadlineReached(nextDiscoverSendAtMs)) {
    const bool sent = sendPresencePacket(WIRE_DISCOVER, activeScanId);
    anyDiscoverSent = anyDiscoverSent || sent;
    ++discoverRepeatsSent;
    nextDiscoverSendAtMs =
        millis() + AvailabilityConfig::P2P_DISCOVER_GAP_MS;
  }

  finishP2pScanIfDue();

  portENTER_CRITICAL(&availabilityMux);
  for (uint8_t index = 0;
       index < AvailabilityConfig::USER_COUNT;
       ++index) {
    InternalUserStatus& user = users[index];
    if (user.lastP2pSeenMs == 0) {
      continue;
    }
    const uint32_t ageMs = millis() - user.lastP2pSeenMs;
    user.p2pAgeSeconds = ageMs / 1000UL;
    if (!scanActive &&
        ageMs > AvailabilityConfig::P2P_RESULT_STALE_MS &&
        user.p2p == AvailabilityService::EvidenceState::Available) {
      user.p2p = AvailabilityService::EvidenceState::Stale;
    }
  }
  portEXIT_CRITICAL(&availabilityMux);
}

void requestP2pScan() {
  scanRequested = true;
  scanRequestExpiresAtMs =
      millis() + AvailabilityConfig::P2P_SCAN_WINDOW_MS + 1500UL;
}

void updateContext(
    uint8_t logicalChannel,
    bool communicationBusy,
    uint8_t transportValue) {
  const uint8_t nextState =
      normalizedPresenceState(logicalChannel, communicationBusy);
  const bool changed = !contextInitialized ||
                       logicalChannel != currentChannel ||
                       communicationBusy != currentCommunicationBusy ||
                       transportValue != currentTransport ||
                       nextState != currentPresenceState;
  currentChannel = logicalChannel;
  currentCommunicationBusy = communicationBusy;
  currentTransport = transportValue;
  currentPresenceState = nextState;
  contextInitialized = true;
  if (changed) {
    leaseDirty = true;
    submitLatestLease(false);
  }
}

}  // namespace

namespace AvailabilityService {

bool begin() {
  if (running) {
    return true;
  }
  selfUser = userNumberFromId(AppConfig::USER_ID);
  if (selfUser == 0) {
    Serial.println("[Availability] invalid USER_ID; expected 01..USER_COUNT");
    return false;
  }
  if (!EspNowTransport::registerReceiveHandler(onEspNowReceive)) {
    Serial.println("[Availability] ESP-NOW handler registration failed");
    return false;
  }

  initializeUsers();
  bootId = esp_random();
  if (bootId == 0) {
    bootId = 1;
  }
  serviceStartedMs = millis();
  lastWifiConnected = WifiConnection::isConnected();
  running = true;

  Serial.printf(
      "[READY] Availability initialized mode=event_driven users=%u bootId=%lu leaseFreshMs=%lu p2pScanMs=%u taskBytes=0\n",
      static_cast<unsigned int>(AvailabilityConfig::USER_COUNT),
      static_cast<unsigned long>(bootId),
      static_cast<unsigned long>(AvailabilityConfig::VOIP_LEASE_FRESH_MS),
      static_cast<unsigned int>(AvailabilityConfig::P2P_SCAN_WINDOW_MS));
  return true;
}

bool isRunning() {
  return running;
}

void poll(
    uint8_t logicalChannel,
    bool communicationBusy,
    uint8_t transportValue) {
  if (!running) {
    return;
  }

  updateContext(logicalChannel, communicationBusy, transportValue);

  const bool wifiConnected = WifiConnection::isConnected();
  if (wifiConnected && !lastWifiConnected) {
    leaseDirty = true;
    leaseRetryNotBeforeMs = 0;
  }
  lastWifiConnected = wifiConnected;
  if (leaseDirty && leaseCanRunWithoutInterruptingCommunication() &&
      (leaseRetryNotBeforeMs == 0 ||
       deadlineReached(leaseRetryNotBeforeMs))) {
    submitLatestLease(false);
  }

  consumeRtdbResults();
  updateVoipFreshness();
  pollP2p();
}

bool requestRefresh() {
  if (!running) {
    return false;
  }
  if (refreshActive) {
    if (logEnabled()) {
      Serial.println("[Availability][REFRESH_COALESCED]");
    }
    return true;
  }

  scanActive = false;
  refreshActive = true;
  currentRefreshState = RefreshState::Refreshing;
  refreshWaitingForLease = true;
  refreshRtdbDone = false;
  refreshP2pDone = false;
  refreshHadFailure = false;
  requestP2pScan();

  if (!submitLatestLease(true)) {
    refreshWaitingForLease = false;
    refreshRtdbDone = true;
    refreshHadFailure = true;
  }
  if (logEnabled()) {
    Serial.println("[Availability][REFRESH_REQUESTED]");
  }
  completeRefreshIfReady();
  return true;
}

void cancelRefresh() {
  refreshActive = false;
  refreshWaitingForLease = false;
  refreshRtdbDone = false;
  refreshP2pDone = false;
  currentRefreshState = RefreshState::Idle;
  scanRequested = false;
  scanActive = false;
  usersReadAwaiting = false;
  RtdbRequestService::cancelPresenceUsersRead();
  if (logEnabled()) {
    Serial.println("[Availability][REFRESH_CANCELLED]");
  }
}

RefreshState refreshState() {
  return currentRefreshState;
}

uint32_t lastSnapshotAgeSeconds() {
  return lastSnapshotReceivedMs == 0
             ? UNKNOWN_AGE_SECONDS
             : (millis() - lastSnapshotReceivedMs) / 1000UL;
}

bool maintenanceLeaseDue(
    uint8_t logicalChannel,
    bool communicationBusy) {
  if (!running || !AvailabilityConfig::ENABLE_SSE_MAINTENANCE_LEASE ||
      logicalChannel == 0 || communicationBusy || refreshActive ||
      leaseAwaiting || usersReadAwaiting) {
    return false;
  }
  const uint32_t baseline =
      lastLeaseSuccessMs == 0 ? serviceStartedMs : lastLeaseSuccessMs;
  return (millis() - baseline) >=
         AvailabilityConfig::LEASE_MAINTENANCE_INTERVAL_MS;
}

bool performMaintenanceLeaseSync(uint32_t timeoutMs) {
  if (!running || timeoutMs == 0) {
    return false;
  }
  lastLeaseOperationSuccess = false;
  if (!submitLatestLease(true)) {
    return false;
  }

  const uint32_t startedAtMs = millis();
  while ((millis() - startedAtMs) < timeoutMs) {
    consumeRtdbResults();
    if (!leaseAwaiting && !leaseRescheduleRequested) {
      return lastLeaseOperationSuccess;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  Serial.printf(
      "[Availability][MAINTENANCE_TIMEOUT] timeoutMs=%lu\n",
      static_cast<unsigned long>(timeoutMs));
  return false;
}

size_t copyUserStatuses(UserStatus* outStatuses, size_t maxStatuses) {
  if (outStatuses == nullptr || maxStatuses == 0) {
    return 0;
  }
  const size_t count =
      maxStatuses < AvailabilityConfig::USER_COUNT
          ? maxStatuses
          : AvailabilityConfig::USER_COUNT;

  portENTER_CRITICAL(&availabilityMux);
  for (size_t index = 0; index < count; ++index) {
    outStatuses[index].userNumber = users[index].userNumber;
    outStatuses[index].voip = users[index].voip;
    outStatuses[index].p2p = users[index].p2p;
    outStatuses[index].voipAgeSeconds = users[index].voipAgeSeconds;
    outStatuses[index].p2pAgeSeconds = users[index].p2pAgeSeconds;
  }
  portEXIT_CRITICAL(&availabilityMux);
  return count;
}

}  // namespace AvailabilityService
