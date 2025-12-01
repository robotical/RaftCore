/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// OfflineDataStore
// Fixed-size ring buffer for poll results with metadata for offline delivery
//
// Rob Dobson 2025
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <vector>
#include <stdint.h>
#include <cstring>
#include "RaftUtils.h"
#include "RaftThreading.h"

// Metadata for a stored poll result
struct OfflineDataMeta
{
    uint32_t seq = 0;
    uint32_t ts = 0;
    uint64_t tsBaseMs = 0;
};

// Stats for the offline buffer
struct OfflineDataStats
{
    uint32_t depth = 0;
    uint32_t drops = 0;
    uint32_t maxEntries = 0;
    uint32_t payloadSize = 0;
    uint32_t metaSize = 0;
    uint32_t tsWrapCount = 0;
    uint32_t firstSeq = 0;
    uint32_t timestampBytes = 0;
    uint32_t timestampResolutionUs = 0;
    uint64_t oldestCaptureMs = 0;

    uint32_t bytesInUse() const
    {
        uint32_t metaBytes = metaSize ? metaSize : sizeof(uint32_t);
        return depth * (payloadSize + metaBytes);
    }
};

class OfflineDataStore
{
public:
    static constexpr uint32_t META_STORAGE_BYTES = sizeof(uint32_t);

    OfflineDataStore()
    {
        _accessMutex = xSemaphoreCreateMutex();
    }

    // Initialise the buffer
    void init(uint32_t maxEntries, uint32_t payloadSize, uint32_t timestampBytes, uint32_t timestampResolutionUs)
    {
        if (xSemaphoreTake(_accessMutex, portMAX_DELAY) != pdTRUE)
            return;

        // Release any existing storage before allocating a new buffer to avoid double-use of RAM
        if (!_ringBuffer.empty())
        {
            std::vector<uint8_t>().swap(_ringBuffer);
        }
        if (!_adjTimestampMs.empty())
        {
            std::vector<uint32_t>().swap(_adjTimestampMs);
        }

        _maxEntries = maxEntries;
        _payloadSize = payloadSize;
        _timestampBytes = timestampBytes;
        _timestampResolutionUs = timestampResolutionUs;
        _timestampWrapMs = (1ULL << (_timestampBytes * 8)) * (_timestampResolutionUs / 1000);
        _ringBuffer.assign(_maxEntries * _payloadSize, 0);
        _adjTimestampMs.assign(_maxEntries, 0);
        _headIdx = 0;
        _count = 0;
        _drops = 0;
        _tsWrapCount = 0;
        _lastTimestampValid = false;
        _timestampBaseMs = 0;
        _lastTimestampVal = 0;
        _nextSeq = 0;

        xSemaphoreGive(_accessMutex);
    }

    // Clear contents
    void clear()
    {
        if (xSemaphoreTake(_accessMutex, portMAX_DELAY) != pdTRUE)
            return;
        _headIdx = 0;
        _count = 0;
        _drops = 0;
        _tsWrapCount = 0;
        _lastTimestampValid = false;
        _timestampBaseMs = 0;
        _lastTimestampVal = 0;
        _nextSeq = 0;
        xSemaphoreGive(_accessMutex);
    }

    bool isConfigured() const
    {
        return _maxEntries > 0 && _payloadSize > 0;
    }

    // Put a new poll result into the ring
    bool put(uint64_t timeNowUs, uint32_t seq, const std::vector<uint8_t>& data)
    {
        if (!isConfigured() || (data.size() != _payloadSize))
            return false;
        if (xSemaphoreTake(_accessMutex, portMAX_DELAY) != pdTRUE)
            return false;

        // Extract timestamp from the payload (big-endian)
        uint16_t tsVal = 0;
        const uint8_t* pData = data.data();
        const uint8_t* pEnd = data.data() + data.size();
        if (_timestampBytes == 1)
            tsVal = data[0];
        else if (_timestampBytes == 2)
            tsVal = Raft::getBEUInt16AndInc(pData, pEnd);
        else if (_timestampBytes == 4)
            tsVal = (uint16_t)Raft::getBEUInt32AndInc(pData, pEnd);

        uint64_t timeNowMs = timeNowUs / 1000;
        if (!_lastTimestampValid)
        {
            uint32_t tsResolutionMs = _timestampResolutionUs / 1000;
            _timestampBaseMs = (tsResolutionMs > 0 && (timeNowMs > (uint64_t)tsVal * tsResolutionMs)) ?
                        timeNowMs - (uint64_t)tsVal * tsResolutionMs : 0;
        }

        // Track wraps to provide a monotonic base
        if (_lastTimestampValid && (tsVal < _lastTimestampVal))
        {
            _timestampBaseMs += _timestampWrapMs;
            _tsWrapCount++;
        }
        _lastTimestampVal = tsVal;
        _lastTimestampValid = true;

        // Store data
        uint32_t writeIdx = _headIdx * _payloadSize;
        memcpy(_ringBuffer.data() + writeIdx, data.data(), _payloadSize);
        uint32_t tsResolutionMs = _timestampResolutionUs / 1000;
        uint64_t adjustedTsMs = _timestampBaseMs + (uint64_t)tsVal * tsResolutionMs;
        _adjTimestampMs[_headIdx] = (uint32_t)adjustedTsMs;

        // Update ring indices
        if (_count < _maxEntries)
        {
            _count++;
        }
        else
        {
            _drops++;
        }
        _headIdx = (_headIdx + 1) % _maxEntries;
        _nextSeq = seq + 1;

        xSemaphoreGive(_accessMutex);
        return true;
    }

    // Get up to maxResponses entries (0 = all)
    uint32_t get(std::vector<uint8_t>& data, uint32_t& responseSize, uint32_t maxResponsesToReturn,
                std::vector<OfflineDataMeta>& metas, bool consumeEntries = true,
                uint32_t startIdx = 0, uint32_t maxBytes = 0)
    {
        data.clear();
        metas.clear();

        if (!isConfigured() || (xSemaphoreTake(_accessMutex, portMAX_DELAY) != pdTRUE))
            return 0;

        if ((consumeEntries) && (startIdx > 0))
            startIdx = 0;

        if (_count == 0 || startIdx >= _count)
        {
            xSemaphoreGive(_accessMutex);
            return 0;
        }

        uint32_t available = _count - startIdx;
        uint32_t numResponses = (maxResponsesToReturn == 0 || available < maxResponsesToReturn) ? available : maxResponsesToReturn;

        if (maxBytes > 0)
        {
            uint32_t bytesPerEntry = _payloadSize + META_STORAGE_BYTES;
            uint32_t maxFromBytes = bytesPerEntry ? maxBytes / bytesPerEntry : 0;
            if (maxFromBytes == 0)
            {
                xSemaphoreGive(_accessMutex);
                return 0;
            }
            if (maxFromBytes < numResponses)
                numResponses = maxFromBytes;
        }

        responseSize = _payloadSize;
        if (numResponses == 0)
        {
            xSemaphoreGive(_accessMutex);
            return 0;
        }

        uint32_t tailIdx = (_headIdx + _maxEntries - _count + startIdx) % _maxEntries;
        data.resize(numResponses * _payloadSize);
        metas.reserve(numResponses);
        uint32_t seqStart = (_nextSeq > _count) ? (_nextSeq - _count + startIdx) : startIdx;
        uint32_t tsResolutionMs = _timestampResolutionUs / 1000;

        uint8_t* pOut = data.data();
        for (uint32_t i = 0; i < numResponses; i++)
        {
            memcpy(pOut, _ringBuffer.data() + (tailIdx * _payloadSize), _payloadSize);
            OfflineDataMeta meta;
            meta.seq = seqStart + i;
            const uint8_t* pData = pOut;
            const uint8_t* pEnd = pOut + _payloadSize;
            if (_timestampBytes == 1)
                meta.ts = pData[0];
            else if (_timestampBytes == 2)
                meta.ts = Raft::getBEUInt16AndInc(pData, pEnd);
            else if (_timestampBytes == 4)
                meta.ts = (uint16_t)Raft::getBEUInt32AndInc(pData, pEnd);
            uint64_t adjustedTsMs = _adjTimestampMs[tailIdx];
            uint64_t tsComponentMs = tsResolutionMs ? (uint64_t)meta.ts * tsResolutionMs : 0;
            meta.tsBaseMs = (adjustedTsMs >= tsComponentMs) ? (adjustedTsMs - tsComponentMs) : 0;
            metas.push_back(meta);

            pOut += _payloadSize;
            tailIdx = (tailIdx + 1) % _maxEntries;
        }

        if (consumeEntries)
            _count -= numResponses;

        xSemaphoreGive(_accessMutex);
        return numResponses;
    }

    bool consume(uint32_t numResponses)
    {
        if (!isConfigured() || (xSemaphoreTake(_accessMutex, portMAX_DELAY) != pdTRUE))
            return false;
        if (numResponses > _count)
            numResponses = _count;
        _count -= numResponses;
        xSemaphoreGive(_accessMutex);
        return true;
    }

    OfflineDataStats getStats() const
    {
        OfflineDataStats stats;
        if (!isConfigured() || (xSemaphoreTake(_accessMutex, portMAX_DELAY) != pdTRUE))
            return stats;

        stats.depth = _count;
        stats.drops = _drops;
        stats.maxEntries = _maxEntries;
        stats.payloadSize = _payloadSize;
        stats.metaSize = META_STORAGE_BYTES;
        stats.tsWrapCount = _tsWrapCount;
        stats.timestampBytes = _timestampBytes;
        stats.timestampResolutionUs = _timestampResolutionUs;
        stats.firstSeq = (_nextSeq > _count) ? (_nextSeq - _count) : 0;
        if (_count > 0)
        {
            uint32_t tailIdx = (_headIdx + _maxEntries - _count) % _maxEntries;
            stats.oldestCaptureMs = _adjTimestampMs[tailIdx];
        }
        xSemaphoreGive(_accessMutex);
        return stats;
    }

    uint32_t capacityBytes() const
    {
        return _maxEntries * (_payloadSize + META_STORAGE_BYTES);
    }

    uint32_t getMaxEntries() const
    {
        return _maxEntries;
    }

    uint32_t getPayloadSize() const
    {
        return _payloadSize;
    }

private:
    std::vector<uint8_t> _ringBuffer;
    std::vector<uint32_t> _adjTimestampMs;
    uint32_t _headIdx = 0;
    uint32_t _count = 0;
    uint32_t _maxEntries = 0;
    uint32_t _payloadSize = 0;
    uint32_t _timestampBytes = 0;
    uint32_t _timestampResolutionUs = 0;
    uint64_t _timestampWrapMs = 0;
    uint64_t _timestampBaseMs = 0;
    uint16_t _lastTimestampVal = 0;
    bool _lastTimestampValid = false;
    uint32_t _drops = 0;
    uint32_t _tsWrapCount = 0;
    uint32_t _nextSeq = 0;

    // Access mutex
    SemaphoreHandle_t _accessMutex = nullptr;
};
