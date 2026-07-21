#include "runtime_statistics.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <limits.h>
#include <string.h>

#include "app_config.h"

namespace {

portMUX_TYPE statisticsMux = portMUX_INITIALIZER_UNLOCKED;
RuntimeStatistics::Snapshot statistics;
RuntimeStatistics::Snapshot lastPersistedStatistics;

uint32_t voipUploadFailuresBase = 0;
uint64_t voipDiscardedSamplesBase = 0;
uint32_t persistentRevision = 0;
bool persistentDirty = false;
bool forcedSaveRequested = false;
bool persistenceAvailable = false;
uint8_t storedSchemaVersion = 0;
uint32_t lastSuccessfulSaveMs = 0;
uint32_t lastSaveAttemptMs = 0;

constexpr const char* KEY_SCHEMA = "schema";
constexpr const char* KEY_SENT = "sent";
constexpr const char* KEY_TALK = "talk";
constexpr const char* KEY_HEARD = "heard";
constexpr const char* KEY_LAST = "last";
constexpr const char* KEY_VOIP_FAILURES = "vfails";
constexpr const char* KEY_VOIP_DISCARDED = "vdrop";
constexpr const char* KEY_LOCAL_DISCARDED = "ldrop";
constexpr const char* KEY_P2P_MISSED = "pmissed";
constexpr const char* KEY_FALLBACKS = "fallback";
constexpr const char* KEY_VOIP_TALK = "vtalk";
constexpr const char* KEY_P2P_TALK = "ptalk";
constexpr const char* KEY_HEARD_USERS = "users";

constexpr const char* CHANNEL_KEYS[RuntimeStatistics::CHANNEL_COUNT] = {
    "ch01", "ch02", "ch03", "ch04", "ch05",
    "ch06", "ch07", "ch08", "ch09", "ch10"};

uint32_t saturatingAddU32(uint32_t left, uint32_t right) {
  if (UINT32_MAX - left < right) {
    return UINT32_MAX;
  }
  return left + right;
}

uint64_t saturatingAddU64(uint64_t left, uint64_t right) {
  if (UINT64_MAX - left < right) {
    return UINT64_MAX;
  }
  return left + right;
}

void markPersistentDirtyLocked() {
  persistentDirty = true;
  ++persistentRevision;
}

bool putUIntIfChanged(
    Preferences& preferences,
    const char* key,
    uint32_t value,
    uint32_t previous) {
  return value == previous || preferences.putUInt(key, value) == sizeof(value);
}

bool putUShortIfChanged(
    Preferences& preferences,
    const char* key,
    uint16_t value,
    uint16_t previous) {
  return value == previous ||
         preferences.putUShort(key, value) == sizeof(value);
}

bool putULong64IfChanged(
    Preferences& preferences,
    const char* key,
    uint64_t value,
    uint64_t previous) {
  return value == previous ||
         preferences.putULong64(key, value) == sizeof(value);
}

bool loadPersistentStatistics(RuntimeStatistics::Snapshot& loaded) {
  Preferences preferences;
  if (!preferences.begin(StatisticsConfig::NVS_NAMESPACE, false)) {
    return false;
  }

  storedSchemaVersion = preferences.getUChar(KEY_SCHEMA, 0);

  // Version 1 stores independent keys. Missing keys always default to zero,
  // which also makes later additions backward compatible without a reset.
  loaded.sentSessions = preferences.getUInt(KEY_SENT, 0);
  loaded.talkSamples = preferences.getULong64(KEY_TALK, 0);
  loaded.heardSamples = preferences.getULong64(KEY_HEARD, 0);
  loaded.lastTransmissionSamples = preferences.getULong64(KEY_LAST, 0);
  loaded.voipUploadFailures = preferences.getUInt(KEY_VOIP_FAILURES, 0);
  loaded.voipDiscardedSamples =
      preferences.getULong64(KEY_VOIP_DISCARDED, 0);
  loaded.localDiscardedSamples =
      preferences.getULong64(KEY_LOCAL_DISCARDED, 0);
  loaded.p2pPacketsMissed = preferences.getUInt(KEY_P2P_MISSED, 0);
  loaded.fallbackCount = preferences.getUInt(KEY_FALLBACKS, 0);
  loaded.voipTalkSamples = preferences.getULong64(KEY_VOIP_TALK, 0);
  loaded.p2pTalkSamples = preferences.getULong64(KEY_P2P_TALK, 0);
  loaded.heardUsersMask = preferences.getUShort(KEY_HEARD_USERS, 0);
  for (uint8_t index = 0;
       index < RuntimeStatistics::CHANNEL_COUNT;
       ++index) {
    loaded.channelTalkSamples[index] =
        preferences.getULong64(CHANNEL_KEYS[index], 0);
  }

  preferences.end();
  return true;
}

bool savePersistentStatistics(
    const RuntimeStatistics::Snapshot& current,
    const RuntimeStatistics::Snapshot& previous) {
  Preferences preferences;
  if (!preferences.begin(StatisticsConfig::NVS_NAMESPACE, false)) {
    return false;
  }

  bool success = true;
  if (storedSchemaVersion != StatisticsConfig::SCHEMA_VERSION) {
    success = preferences.putUChar(
                  KEY_SCHEMA,
                  StatisticsConfig::SCHEMA_VERSION) == sizeof(uint8_t) &&
              success;
  }

  success = putUIntIfChanged(
                preferences,
                KEY_SENT,
                current.sentSessions,
                previous.sentSessions) &&
            success;
  success = putULong64IfChanged(
                preferences,
                KEY_TALK,
                current.talkSamples,
                previous.talkSamples) &&
            success;
  success = putULong64IfChanged(
                preferences,
                KEY_HEARD,
                current.heardSamples,
                previous.heardSamples) &&
            success;
  success = putULong64IfChanged(
                preferences,
                KEY_LAST,
                current.lastTransmissionSamples,
                previous.lastTransmissionSamples) &&
            success;
  success = putUIntIfChanged(
                preferences,
                KEY_VOIP_FAILURES,
                current.voipUploadFailures,
                previous.voipUploadFailures) &&
            success;
  success = putULong64IfChanged(
                preferences,
                KEY_VOIP_DISCARDED,
                current.voipDiscardedSamples,
                previous.voipDiscardedSamples) &&
            success;
  success = putULong64IfChanged(
                preferences,
                KEY_LOCAL_DISCARDED,
                current.localDiscardedSamples,
                previous.localDiscardedSamples) &&
            success;
  success = putUIntIfChanged(
                preferences,
                KEY_P2P_MISSED,
                current.p2pPacketsMissed,
                previous.p2pPacketsMissed) &&
            success;
  success = putUIntIfChanged(
                preferences,
                KEY_FALLBACKS,
                current.fallbackCount,
                previous.fallbackCount) &&
            success;
  success = putULong64IfChanged(
                preferences,
                KEY_VOIP_TALK,
                current.voipTalkSamples,
                previous.voipTalkSamples) &&
            success;
  success = putULong64IfChanged(
                preferences,
                KEY_P2P_TALK,
                current.p2pTalkSamples,
                previous.p2pTalkSamples) &&
            success;
  success = putUShortIfChanged(
                preferences,
                KEY_HEARD_USERS,
                current.heardUsersMask,
                previous.heardUsersMask) &&
            success;

  for (uint8_t index = 0;
       index < RuntimeStatistics::CHANNEL_COUNT;
       ++index) {
    success = putULong64IfChanged(
                  preferences,
                  CHANNEL_KEYS[index],
                  current.channelTalkSamples[index],
                  previous.channelTalkSamples[index]) &&
              success;
  }

  preferences.end();
  return success;
}

}  // namespace

namespace RuntimeStatistics {

void begin() {
  Snapshot loaded;
  bool loadedFromNvs = false;

  if (StatisticsConfig::PERSISTENCE_ENABLED) {
    loadedFromNvs = loadPersistentStatistics(loaded);
  }

  portENTER_CRITICAL(&statisticsMux);
  statistics = loadedFromNvs ? loaded : Snapshot{};
  lastPersistedStatistics = statistics;
  voipUploadFailuresBase = statistics.voipUploadFailures;
  voipDiscardedSamplesBase = statistics.voipDiscardedSamples;
  persistentRevision = 0;
  persistentDirty = false;
  forcedSaveRequested = false;
  persistenceAvailable =
      StatisticsConfig::PERSISTENCE_ENABLED && loadedFromNvs;
  lastSuccessfulSaveMs = millis();
  lastSaveAttemptMs = 0;
  portEXIT_CRITICAL(&statisticsMux);

  if (StatisticsConfig::LOG_PERSISTENCE) {
    Serial.printf(
        "[STATS_NVS] enabled=%s available=%s schema=%u sent=%lu talkSamples=%llu heardSamples=%llu\n",
        StatisticsConfig::PERSISTENCE_ENABLED ? "true" : "false",
        persistenceAvailable ? "true" : "false",
        static_cast<unsigned int>(storedSchemaVersion),
        static_cast<unsigned long>(statistics.sentSessions),
        static_cast<unsigned long long>(statistics.talkSamples),
        static_cast<unsigned long long>(statistics.heardSamples));
  }
}

void maintainPersistence() {
  if (!StatisticsConfig::PERSISTENCE_ENABLED) {
    return;
  }

  const uint32_t nowMs = millis();
  bool dirty = false;
  bool forced = false;
  bool available = false;
  uint32_t lastSuccess = 0;
  uint32_t lastAttempt = 0;

  portENTER_CRITICAL(&statisticsMux);
  dirty = persistentDirty;
  forced = forcedSaveRequested;
  available = persistenceAvailable;
  lastSuccess = lastSuccessfulSaveMs;
  lastAttempt = lastSaveAttemptMs;
  portEXIT_CRITICAL(&statisticsMux);

  if (!dirty || !available) {
    return;
  }

  const bool intervalElapsed =
      (nowMs - lastSuccess) >= StatisticsConfig::SAVE_INTERVAL_MS;
  if (!forced && !intervalElapsed) {
    return;
  }

  if (lastAttempt != 0 &&
      (nowMs - lastAttempt) < StatisticsConfig::SAVE_FAILURE_RETRY_MS) {
    return;
  }

  Snapshot toSave;
  uint32_t revisionToSave = 0;
  portENTER_CRITICAL(&statisticsMux);
  toSave = statistics;
  revisionToSave = persistentRevision;
  lastSaveAttemptMs = nowMs;
  portEXIT_CRITICAL(&statisticsMux);

  const bool success =
      savePersistentStatistics(toSave, lastPersistedStatistics);

  portENTER_CRITICAL(&statisticsMux);
  if (success) {
    lastPersistedStatistics = toSave;
    storedSchemaVersion = StatisticsConfig::SCHEMA_VERSION;
    lastSuccessfulSaveMs = nowMs;
    lastSaveAttemptMs = 0;
    forcedSaveRequested = false;
    if (persistentRevision == revisionToSave) {
      persistentDirty = false;
    }
  }
  portEXIT_CRITICAL(&statisticsMux);

  if (StatisticsConfig::LOG_PERSISTENCE) {
    Serial.printf(
        "[STATS_NVS] save=%s reason=%s revision=%lu sent=%lu talkSamples=%llu\n",
        success ? "ok" : "failed",
        forced ? "channel_exit" : "interval",
        static_cast<unsigned long>(revisionToSave),
        static_cast<unsigned long>(toSave.sentSessions),
        static_cast<unsigned long long>(toSave.talkSamples));
  }
}

void requestPersistenceSave() {
  if (!StatisticsConfig::PERSISTENCE_ENABLED) {
    return;
  }
  portENTER_CRITICAL(&statisticsMux);
  // Do not write an unchanged snapshot merely because a channel was exited.
  if (persistentDirty) {
    forcedSaveRequested = true;
  }
  portEXIT_CRITICAL(&statisticsMux);
}

void recordCapturedSamples(
    Communication::Transport transport,
    uint8_t channel,
    size_t sampleCount) {
  if (sampleCount == 0) {
    return;
  }

  portENTER_CRITICAL(&statisticsMux);
  statistics.talkSamples += sampleCount;
  if (transport == Communication::Transport::P2p) {
    statistics.p2pTalkSamples += sampleCount;
  } else {
    statistics.voipTalkSamples += sampleCount;
  }
  if (channel >= 1 && channel <= CHANNEL_COUNT) {
    statistics.channelTalkSamples[channel - 1] += sampleCount;
  }
  markPersistentDirtyLocked();
  portEXIT_CRITICAL(&statisticsMux);
}

void recordLocalDiscardedSamples(size_t sampleCount) {
  if (sampleCount == 0) {
    return;
  }
  portENTER_CRITICAL(&statisticsMux);
  statistics.localDiscardedSamples += sampleCount;
  markPersistentDirtyLocked();
  portEXIT_CRITICAL(&statisticsMux);
}

void recordCompletedSession(uint64_t capturedSamples) {
  if (capturedSamples == 0) {
    return;
  }
  portENTER_CRITICAL(&statisticsMux);
  ++statistics.sentSessions;
  statistics.lastTransmissionSamples = capturedSamples;
  markPersistentDirtyLocked();
  portEXIT_CRITICAL(&statisticsMux);
}

void recordPlayedSamples(size_t sampleCount, uint8_t sourceUserNumber) {
  if (sampleCount == 0) {
    return;
  }
  portENTER_CRITICAL(&statisticsMux);
  statistics.heardSamples += sampleCount;
  if (sourceUserNumber >= 1 && sourceUserNumber <= 16) {
    statistics.heardUsersMask |=
        static_cast<uint16_t>(1U << (sourceUserNumber - 1));
  }
  markPersistentDirtyLocked();
  portEXIT_CRITICAL(&statisticsMux);
}

void recordP2pPacketSendResult(bool acceptedForSend) {
  portENTER_CRITICAL(&statisticsMux);
  ++statistics.p2pPacketsSentAttempted;
  if (!acceptedForSend) {
    ++statistics.p2pPacketsSendRejected;
  }
  portEXIT_CRITICAL(&statisticsMux);
}

void recordP2pPacketReceived() {
  portENTER_CRITICAL(&statisticsMux);
  ++statistics.p2pPacketsReceived;
  portEXIT_CRITICAL(&statisticsMux);
}

void recordP2pPacketsMissed(uint32_t missingPacketCount) {
  if (missingPacketCount == 0) {
    return;
  }
  portENTER_CRITICAL(&statisticsMux);
  statistics.p2pPacketsMissed += missingPacketCount;
  statistics.p2pDiagnosticPacketsMissed += missingPacketCount;
  markPersistentDirtyLocked();
  portEXIT_CRITICAL(&statisticsMux);
}

void recordFallback() {
  portENTER_CRITICAL(&statisticsMux);
  ++statistics.fallbackCount;
  markPersistentDirtyLocked();
  portEXIT_CRITICAL(&statisticsMux);
}

void setVoipNetworkCounters(
    uint32_t uploadFailures,
    uint64_t discardedSamples) {
  const uint32_t cumulativeFailures =
      saturatingAddU32(voipUploadFailuresBase, uploadFailures);
  const uint64_t cumulativeDiscarded =
      saturatingAddU64(voipDiscardedSamplesBase, discardedSamples);

  portENTER_CRITICAL(&statisticsMux);
  if (statistics.voipUploadFailures != cumulativeFailures ||
      statistics.voipDiscardedSamples != cumulativeDiscarded) {
    statistics.voipUploadFailures = cumulativeFailures;
    statistics.voipDiscardedSamples = cumulativeDiscarded;
    markPersistentDirtyLocked();
  }
  portEXIT_CRITICAL(&statisticsMux);
}

Snapshot snapshot() {
  Snapshot copy;
  portENTER_CRITICAL(&statisticsMux);
  copy = statistics;
  portEXIT_CRITICAL(&statisticsMux);
  return copy;
}

}  // namespace RuntimeStatistics
