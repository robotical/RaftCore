/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// OfflineDataStoreNVS
// Persisted ring buffer for offline poll results stored in NVS
//
// Rob Dobson 2025
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stdint.h>
#include <vector>
#include <string>
#include "OfflineDataStore.h"

class OfflineDataStoreNVS
{
public:
    OfflineDataStoreNVS();
    ~OfflineDataStoreNVS();

    // Configure NVS storage for a device (creates metadata if absent)
    bool configure(const char* nvsNamespace, uint32_t payloadSize, uint32_t timestampBytes,
                uint32_t timestampResolutionUs, uint32_t maxEntries);

    // Set an effective cap (e.g. RAM capacity) for how many entries to retain
    void setEffectiveMaxEntries(uint32_t maxEntries);

    // Append a batch of records (payloads is concatenated entries)
    bool appendBatch(const std::vector<uint8_t>& payloads, uint32_t payloadSize,
                const std::vector<uint32_t>& adjTsMs, uint32_t firstSeq, uint32_t count, uint32_t& outLastSeq);

    // Import records into a RAM buffer (oldest to newest). Returns true if any entries imported.
    bool importTo(OfflineDataStore& dest, uint32_t importMaxEntries, uint32_t& outNextSeq);

    // Clear all NVS data for this namespace
    void clear();

    bool isReady() const;
    uint32_t getCount() const;
    uint32_t getNextSeq() const;

private:
    struct OfflineNvsMeta
    {
        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t payloadSize = 0;
        uint32_t recordSize = 0;
        uint32_t timestampBytes = 0;
        uint32_t timestampResolutionUs = 0;
        uint32_t maxEntries = 0;
        uint32_t head = 0;
        uint32_t count = 0;
        uint32_t nextSeq = 0;
        uint32_t importSeq = 0;
        uint32_t recordsPerSegment = 0;
        uint32_t segmentBytes = 0;
        uint32_t drops = 0;
    };

    bool open();
    void close();
    bool loadMeta();
    bool saveMeta();
    bool readSegment(uint32_t segIdx, std::vector<uint8_t>& outBuf) const;
    bool writeSegment(uint32_t segIdx, const uint8_t* data, size_t len);
    void makeSegmentKey(uint32_t segIdx, char* key, size_t keySize) const;
    void resetMeta(uint32_t payloadSize, uint32_t timestampBytes,
                uint32_t timestampResolutionUs, uint32_t maxEntries);

private:
    std::string _nvsNamespace;
    bool _ready = false;
    bool _metaValid = false;
    OfflineNvsMeta _meta;
    uint32_t _effectiveMaxEntries = 0;

#ifdef ESP_PLATFORM
    uint32_t _nvsHandle = 0;
#endif
};
