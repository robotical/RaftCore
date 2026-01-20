/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Device status
//
// Rob Dobson 2024
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "DeviceStatus.h"

// #define DEBUG_DEVICE_STATUS

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get pending ident poll requests 
/// @param timeNowUs time in us (passed in to aid testing)
/// @param pollInfo (out) polling info
/// @return true if there is a pending request
bool DeviceStatus::getPendingIdentPollInfo(uint64_t timeNowUs, DevicePollingInfo& pollInfo)
{
    // Check if this is the very first poll
    if (deviceIdentPolling.lastPollTimeUs == 0)
    {
        deviceIdentPolling.lastPollTimeUs = timeNowUs;
    }

    // Check if time to do full or partial poll (partial poll is when a pause after send has been requested)
    bool isStartOfPoll = (deviceIdentPolling.partialPollNextReqIdx == 0);
    uint32_t callIntervalUs = isStartOfPoll ? deviceIdentPolling.pollIntervalUs : deviceIdentPolling.partialPollPauseAfterSendMs * 1000;
    if (Raft::isTimeout(timeNowUs, deviceIdentPolling.lastPollTimeUs, callIntervalUs))
    {
        // Clear the poll result data if this is the start of the poll
        if (isStartOfPoll)
            pollInfo._pollDataResult.clear();

        // Update timestamp
        deviceIdentPolling.lastPollTimeUs = timeNowUs;

        // Check poll requests isn't empty
        if (deviceIdentPolling.pollReqs.size() == 0)
            return false;

        // Copy polling info
        pollInfo = deviceIdentPolling;

        // Return true
        return true;
    }

    // Nothing pending
    return false;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Store poll results
/// @param nextReqIdx index of next request to store (0 = full poll, 1+ = partial poll)
/// @param timeNowUs time in us (passed in to aid testing)
/// @param pollResult poll result data
/// @param pPollInfo pointer to device polling info (maybe nullptr)
/// @param pauseAfterSendMs pause after send in ms
/// @return true if result stored
bool DeviceStatus::storePollResults(uint32_t nextReqIdx, uint64_t timeNowUs, const std::vector<uint8_t>& pollResult, const DevicePollingInfo* pPollInfo, uint32_t pauseAfterSendMs)
{
    // Check if this is a full or partial poll
    if (nextReqIdx != 0)
    {
        // Partial poll - store the partial poll result
        deviceIdentPolling.recordPartialPollResult(nextReqIdx, timeNowUs, pollResult, pauseAfterSendMs);
    }
    else
    {
        // Get the any partial poll results
        std::vector<uint8_t> partialPollResult;
        if (deviceIdentPolling.getPartialPollResultsAndClear(partialPollResult))
        {
            // Add the new poll result to the partial poll result
            partialPollResult.insert(partialPollResult.end(), pollResult.begin(), pollResult.end());

            // Add complete poll result to aggregator
            bool aggOk = dataAggregator.put(timeNowUs, partialPollResult);
            uint32_t seq = _offlineSeq++;
            if (offlineData.isConfigured() && !_offlineBufferPaused)
                offlineData.put(timeNowUs, seq, partialPollResult);
            // LOG_I(MODULE_PREFIX, "storePollResults addrSeq %u size %u offline %s aggOk %d",
            //             (unsigned)seq, (unsigned)partialPollResult.size(),
            //             offlineData.isConfigured() ? (_offlineBufferPaused ? "paused" : "active") : "notcfg", aggOk);
            return aggOk;
    }

    // Poll complete without partial poll - add result to aggregator
    bool aggOk = dataAggregator.put(timeNowUs, pollResult);
    uint32_t seq = _offlineSeq++;
    if (offlineData.isConfigured() && !_offlineBufferPaused)
        offlineData.put(timeNowUs, seq, pollResult);
    // LOG_I(MODULE_PREFIX, "storePollResults addrSeq %u size %u offline %s aggOk %d",
    //             (unsigned)seq, (unsigned)pollResult.size(),
    //             offlineData.isConfigured() ? (_offlineBufferPaused ? "paused" : "active") : "notcfg", aggOk);
    return aggOk;
}
    return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Configure offline buffer
/// @param maxEntries maximum entries to retain
/// @param payloadSize size of each poll result (bytes)
/// @param timestampBytes number of bytes used for timestamp in poll result
/// @param timestampResolutionUs timestamp resolution (us)
void DeviceStatus::configureOfflineBuffer(uint32_t maxEntries, uint32_t payloadSize, uint32_t timestampBytes, uint32_t timestampResolutionUs)
{
    // Keep the current paused state so reconfiguration doesn't accidentally resume buffering
    bool wasPaused = _offlineBufferPaused;
    offlineData.init(maxEntries, payloadSize, timestampBytes, timestampResolutionUs);
    _offlineBufferPaused = wasPaused;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get offline responses
/// @param devicePollResponseData (out) buffer containing concatenated responses
/// @param responseSize (out) size of a single response
/// @param maxResponsesToReturn max responses (0 for all)
/// @param metas (out) metadata per response
/// @return number of responses returned
uint32_t DeviceStatus::getOfflineResponses(std::vector<uint8_t>& devicePollResponseData, uint32_t& responseSize,
            uint32_t maxResponsesToReturn, std::vector<OfflineDataMeta>& metas)
{
    if (_offlineDrainPaused || !offlineData.isConfigured())
        return 0;
    return offlineData.get(devicePollResponseData, responseSize, maxResponsesToReturn, metas);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Peek at offline responses without consuming them
uint32_t DeviceStatus::peekOfflineResponses(std::vector<uint8_t>& devicePollResponseData, uint32_t& responseSize,
            uint32_t startIdx, uint32_t maxResponsesToReturn, uint32_t maxBytes,
            std::vector<OfflineDataMeta>& metas)
{
    if (!offlineData.isConfigured())
        return 0;
    return offlineData.get(devicePollResponseData, responseSize, maxResponsesToReturn, metas, false, startIdx, maxBytes);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get offline stats
OfflineDataStats DeviceStatus::getOfflineStats() const
{
    return offlineData.getStats();
}
