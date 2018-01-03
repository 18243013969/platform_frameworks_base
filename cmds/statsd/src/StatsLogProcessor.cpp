/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define DEBUG true  // STOPSHIP if true
#include "Log.h"
#include "statslog.h"

#include <android-base/file.h>
#include <dirent.h>
#include "StatsLogProcessor.h"
#include "android-base/stringprintf.h"
#include "guardrail/StatsdStats.h"
#include "metrics/CountMetricProducer.h"
#include "external/StatsPullerManager.h"
#include "stats_util.h"
#include "storage/StorageManager.h"

#include <log/log_event_list.h>
#include <utils/Errors.h>

using namespace android;
using android::base::StringPrintf;
using android::util::FIELD_COUNT_REPEATED;
using android::util::FIELD_TYPE_BOOL;
using android::util::FIELD_TYPE_FLOAT;
using android::util::FIELD_TYPE_INT32;
using android::util::FIELD_TYPE_INT64;
using android::util::FIELD_TYPE_MESSAGE;
using android::util::FIELD_TYPE_STRING;
using android::util::ProtoOutputStream;
using std::make_unique;
using std::unique_ptr;
using std::vector;

namespace android {
namespace os {
namespace statsd {

// for ConfigMetricsReportList
const int FIELD_ID_CONFIG_KEY = 1;
const int FIELD_ID_REPORTS = 2;
// for ConfigKey
const int FIELD_ID_UID = 1;
const int FIELD_ID_ID = 2;
// for ConfigMetricsReport
const int FIELD_ID_METRICS = 1;
const int FIELD_ID_UID_MAP = 2;

#define STATS_DATA_DIR "/data/misc/stats-data"

StatsLogProcessor::StatsLogProcessor(const sp<UidMap>& uidMap,
                                     const sp<AnomalyMonitor>& anomalyMonitor,
                                     const long timeBaseSec,
                                     const std::function<void(const ConfigKey&)>& sendBroadcast)
    : mUidMap(uidMap),
      mAnomalyMonitor(anomalyMonitor),
      mSendBroadcast(sendBroadcast),
      mTimeBaseSec(timeBaseSec) {
    // On each initialization of StatsLogProcessor, check stats-data directory to see if there is
    // any left over data to be read.
    StorageManager::sendBroadcast(STATS_DATA_DIR, mSendBroadcast);
    StatsPullerManager statsPullerManager;
    statsPullerManager.SetTimeBaseSec(mTimeBaseSec);
}

StatsLogProcessor::~StatsLogProcessor() {
}

void StatsLogProcessor::onAnomalyAlarmFired(
        const uint64_t timestampNs,
        unordered_set<sp<const AnomalyAlarm>, SpHash<AnomalyAlarm>> anomalySet) {
    // TODO: This is a thread-safety issue. mMetricsManagers could change under our feet.
    // TODO: Solution? Lock everything! :(
    // TODO: Question: Can we replace the other lock (broadcast), or do we need to supplement it?
    for (const auto& itr : mMetricsManagers) {
        itr.second->onAnomalyAlarmFired(timestampNs, anomalySet);
    }
}

// TODO: what if statsd service restarts? How do we know what logs are already processed before?
void StatsLogProcessor::OnLogEvent(const LogEvent& msg) {
    StatsdStats::getInstance().noteAtomLogged(msg.GetTagId(), msg.GetTimestampNs() / NS_PER_SEC);
    // pass the event to metrics managers.
    for (auto& pair : mMetricsManagers) {
        pair.second->onLogEvent(msg);
        flushIfNecessary(msg.GetTimestampNs(), pair.first, *(pair.second));
    }
    // Hard-coded logic to update the isolated uid's in the uid-map.
    // The field numbers need to be currently updated by hand with atoms.proto
    if (msg.GetTagId() == android::util::ISOLATED_UID_CHANGED) {
        status_t err = NO_ERROR, err2 = NO_ERROR, err3 = NO_ERROR;
        bool is_create = msg.GetBool(3, &err);
        auto parent_uid = int(msg.GetLong(1, &err2));
        auto isolated_uid = int(msg.GetLong(2, &err3));
        if (err == NO_ERROR && err2 == NO_ERROR && err3 == NO_ERROR) {
            if (is_create) {
                mUidMap->assignIsolatedUid(isolated_uid, parent_uid);
            } else {
                mUidMap->removeIsolatedUid(isolated_uid, parent_uid);
            }
        }
    }
}

void StatsLogProcessor::OnConfigUpdated(const ConfigKey& key, const StatsdConfig& config) {
    ALOGD("Updated configuration for key %s", key.ToString().c_str());
    sp<MetricsManager> newMetricsManager = new MetricsManager(key, config, mTimeBaseSec, mUidMap);
    auto it = mMetricsManagers.find(key);
    if (it == mMetricsManagers.end() && mMetricsManagers.size() > StatsdStats::kMaxConfigCount) {
        ALOGE("Can't accept more configs!");
        return;
    }

    if (newMetricsManager->isConfigValid()) {
        mUidMap->OnConfigUpdated(key);
        newMetricsManager->setAnomalyMonitor(mAnomalyMonitor);
        if (config.log_source().package().size() > 0) {
            // We have to add listener after the MetricsManager is constructed because it's
            // not safe to create wp or sp from this pointer inside its constructor.
            mUidMap->addListener(newMetricsManager.get());
        }
        mMetricsManagers[key] = newMetricsManager;
        // Why doesn't this work? mMetricsManagers.insert({key, std::move(newMetricsManager)});
        VLOG("StatsdConfig valid");
    } else {
        // If there is any error in the config, don't use it.
        ALOGE("StatsdConfig NOT valid");
    }
}

size_t StatsLogProcessor::GetMetricsSize(const ConfigKey& key) const {
    auto it = mMetricsManagers.find(key);
    if (it == mMetricsManagers.end()) {
        ALOGW("Config source %s does not exist", key.ToString().c_str());
        return 0;
    }
    return it->second->byteSize();
}

void StatsLogProcessor::onDumpReport(const ConfigKey& key, const uint64_t& dumpTimeStampNs, ConfigMetricsReportList* report) {
    auto it = mMetricsManagers.find(key);
    if (it == mMetricsManagers.end()) {
        ALOGW("Config source %s does not exist", key.ToString().c_str());
        return;
    }
    report->mutable_config_key()->set_uid(key.GetUid());
    report->mutable_config_key()->set_id(key.GetId());
    ConfigMetricsReport* configMetricsReport = report->add_reports();
    it->second->onDumpReport(dumpTimeStampNs, configMetricsReport);
    // TODO: dump uid mapping.
}

void StatsLogProcessor::onDumpReport(const ConfigKey& key, vector<uint8_t>* outData) {
    auto it = mMetricsManagers.find(key);
    if (it == mMetricsManagers.end()) {
        ALOGW("Config source %s does not exist", key.ToString().c_str());
        return;
    }

    // This allows another broadcast to be sent within the rate-limit period if we get close to
    // filling the buffer again soon.
    mBroadcastTimesMutex.lock();
    mLastBroadcastTimes.erase(key);
    mBroadcastTimesMutex.unlock();

    ProtoOutputStream proto;

    // Start of ConfigKey.
    long long configKeyToken = proto.start(FIELD_TYPE_MESSAGE | FIELD_ID_CONFIG_KEY);
    proto.write(FIELD_TYPE_INT32 | FIELD_ID_UID, key.GetUid());
    proto.write(FIELD_TYPE_INT64 | FIELD_ID_ID, (long long)key.GetId());
    proto.end(configKeyToken);
    // End of ConfigKey.

    // Start of ConfigMetricsReport (reports).
    long long reportsToken =
            proto.start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_REPORTS);

    // First, fill in ConfigMetricsReport using current data on memory, which
    // starts from filling in StatsLogReport's.
    it->second->onDumpReport(&proto);

    // Fill in UidMap.
    auto uidMap = mUidMap->getOutput(key);
    const int uidMapSize = uidMap.ByteSize();
    char uidMapBuffer[uidMapSize];
    uidMap.SerializeToArray(&uidMapBuffer[0], uidMapSize);
    proto.write(FIELD_TYPE_MESSAGE | FIELD_ID_UID_MAP, uidMapBuffer, uidMapSize);

    // End of ConfigMetricsReport (reports).
    proto.end(reportsToken);

    // Then, check stats-data directory to see there's any file containing
    // ConfigMetricsReport from previous shutdowns to concatenate to reports.
    StorageManager::appendConfigMetricsReport(STATS_DATA_DIR, proto);

    if (outData != nullptr) {
        outData->clear();
        outData->resize(proto.size());
        size_t pos = 0;
        auto iter = proto.data();
        while (iter.readBuffer() != NULL) {
            size_t toRead = iter.currentToRead();
            std::memcpy(&((*outData)[pos]), iter.readBuffer(), toRead);
            pos += toRead;
            iter.rp()->move(toRead);
        }
    }
    StatsdStats::getInstance().noteMetricsReportSent(key);
}

void StatsLogProcessor::OnConfigRemoved(const ConfigKey& key) {
    auto it = mMetricsManagers.find(key);
    if (it != mMetricsManagers.end()) {
        mMetricsManagers.erase(it);
        mUidMap->OnConfigRemoved(key);
    }
    StatsdStats::getInstance().noteConfigRemoved(key);

    std::lock_guard<std::mutex> lock(mBroadcastTimesMutex);
    mLastBroadcastTimes.erase(key);
}

void StatsLogProcessor::flushIfNecessary(uint64_t timestampNs, const ConfigKey& key,
                                         MetricsManager& metricsManager) {
    std::lock_guard<std::mutex> lock(mBroadcastTimesMutex);

    auto lastCheckTime = mLastByteSizeTimes.find(key);
    if (lastCheckTime != mLastByteSizeTimes.end()) {
        if (timestampNs - lastCheckTime->second < StatsdStats::kMinByteSizeCheckPeriodNs) {
            return;
        }
    }

    // We suspect that the byteSize() computation is expensive, so we set a rate limit.
    size_t totalBytes = metricsManager.byteSize();
    mLastByteSizeTimes[key] = timestampNs;
    if (totalBytes >
        StatsdStats::kMaxMetricsBytesPerConfig) {  // Too late. We need to start clearing data.
        // TODO(b/70571383): By 12/15/2017 add API to drop data directly
        ProtoOutputStream proto;
        metricsManager.onDumpReport(&proto);
        StatsdStats::getInstance().noteDataDropped(key);
        VLOG("StatsD had to toss out metrics for %s", key.ToString().c_str());
    } else if (totalBytes > .9 * StatsdStats::kMaxMetricsBytesPerConfig) {
        // Send broadcast so that receivers can pull data.
        auto lastBroadcastTime = mLastBroadcastTimes.find(key);
        if (lastBroadcastTime != mLastBroadcastTimes.end()) {
            if (timestampNs - lastBroadcastTime->second < StatsdStats::kMinBroadcastPeriodNs) {
                VLOG("StatsD would've sent a broadcast but the rate limit stopped us.");
                return;
            }
        }
        mLastBroadcastTimes[key] = timestampNs;
        VLOG("StatsD requesting broadcast for %s", key.ToString().c_str());
        mSendBroadcast(key);
        StatsdStats::getInstance().noteBroadcastSent(key);
    }
}

void StatsLogProcessor::WriteDataToDisk() {
    mkdir(STATS_DATA_DIR, S_IRWXU);
    for (auto& pair : mMetricsManagers) {
        const ConfigKey& key = pair.first;
        vector<uint8_t> data;
        onDumpReport(key, &data);
        // TODO: Add a guardrail to prevent accumulation of file on disk.
        string file_name = StringPrintf("%s/%d-%lld-%ld", STATS_DATA_DIR, key.GetUid(),
                                        (long long)key.GetId(), time(nullptr));
        StorageManager::writeFile(file_name.c_str(), &data[0], data.size());
    }
}

}  // namespace statsd
}  // namespace os
}  // namespace android
