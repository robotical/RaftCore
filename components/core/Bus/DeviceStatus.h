/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Device status
//
// Rob Dobson 2024
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once 

#include "limits.h"
#include "RaftUtils.h"
#include "DevicePollingInfo.h"
#include "PollDataAggregator.h"
#include "OfflineDataStore.h"

using DeviceTypeIndexType = uint16_t;
static constexpr DeviceTypeIndexType DEVICE_TYPE_INDEX_INVALID = USHRT_MAX;

class DeviceStatus
{
public:
    static const DeviceTypeIndexType DEVICE_TYPE_INDEX_INVALID = ::DEVICE_TYPE_INDEX_INVALID;

    DeviceStatus()
    {
    }

    void clear()
    {
        deviceTypeIndex = DEVICE_TYPE_INDEX_INVALID;
        deviceIdentPolling.clear();
        dataAggregator.clear();
        clearOfflineBuffer();
        _offlineBufferPaused = false;
        _offlineDrainPaused = false;
    }

    bool isValid() const
    {
        return deviceTypeIndex != DEVICE_TYPE_INDEX_INVALID;
    }

    /// @brief Get pending ident poll info
    /// @param timeNowUs time in us (passed in to aid testing)
    /// @param pollInfo (out) polling info
    /// @return true if there is a pending request
    bool getPendingIdentPollInfo(uint64_t timeNowUs, DevicePollingInfo& pollInfo);

    /// @brief Store poll results
    /// @param nextReqIdx index of next request to store (0 = full poll, 1+ = partial poll)
    /// @param timeNowUs time in us (passed in to aid testing)
    /// @param pollResult poll result data
    /// @param pPollInfo pointer to device polling info (maybe nullptr)
    /// @param pauseAfterSendMs pause after send in ms
    /// @return true if result stored
    bool storePollResults(uint32_t nextReqIdx, uint64_t timeNowUs, const std::vector<uint8_t>& pollResult, 
        const DevicePollingInfo* pPollInfo, uint32_t pauseAfterSendMs);

    // Offline buffer control
    void configureOfflineBuffer(uint32_t maxEntries, uint32_t payloadSize, uint32_t timestampBytes, uint32_t timestampResolutionUs);
    uint32_t getOfflineResponses(std::vector<uint8_t>& devicePollResponseData, uint32_t& responseSize,
                uint32_t maxResponsesToReturn, std::vector<OfflineDataMeta>& metas);
    uint32_t peekOfflineResponses(std::vector<uint8_t>& devicePollResponseData, uint32_t& responseSize,
                uint32_t startIdx, uint32_t maxResponsesToReturn, uint32_t maxBytes,
                std::vector<OfflineDataMeta>& metas);
    OfflineDataStats getOfflineStats() const;
    void clearOfflineBuffer()
    {
        offlineData.clear();
        _offlineSeq = 0;
    }
    void setOfflineBufferPaused(bool paused)
    {
        _offlineBufferPaused = paused;
    }
    bool isOfflineBufferPaused() const
    {
        return _offlineBufferPaused;
    }
    void setOfflineDrainPaused(bool paused)
    {
        _offlineDrainPaused = paused;
    }
    bool isOfflineDrainPaused() const
    {
        return _offlineDrainPaused;
    }
    uint32_t getOfflineAllocBytes() const
    {
        return offlineData.capacityBytes();
    }
    uint32_t getOfflineMaxEntries() const
    {
        return offlineData.getMaxEntries();
    }
    void setOfflineSeq(uint32_t nextSeq)
    {
        _offlineSeq = nextSeq;
    }
    uint32_t getOfflineSeq() const
    {
        return _offlineSeq;
    }

    // Get device type index
    DeviceTypeIndexType getDeviceTypeIndex() const
    {
        return deviceTypeIndex;
    }

    // Get number of poll requests
    uint32_t getNumPollRequests() const
    {
        return deviceIdentPolling.pollReqs.size();
    }

    // Device type index
    DeviceTypeIndexType deviceTypeIndex = DEVICE_TYPE_INDEX_INVALID;

    // Device ident polling - polling related to the device type
    DevicePollingInfo deviceIdentPolling;

    // Data aggregator
    PollDataAggregator dataAggregator;

    // Offline data buffer
    OfflineDataStore offlineData;

    // Debug
    static constexpr const char* MODULE_PREFIX = "RaftI2CDevStat";    

private:
    uint32_t _offlineSeq = 0;
    bool _offlineBufferPaused = false;
    bool _offlineDrainPaused = false;
};
