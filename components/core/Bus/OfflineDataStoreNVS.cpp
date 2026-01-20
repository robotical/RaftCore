/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// OfflineDataStoreNVS
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "OfflineDataStoreNVS.h"
#include "Logger.h"
#include "RaftUtils.h"
#include <algorithm>

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "nvs_flash.h"
#endif

static constexpr const char* MODULE_PREFIX = "OfflineDataStoreNVS";
static constexpr uint32_t OFFLINE_NVS_META_MAGIC = 0x4F424E56; // "OBNV"
static constexpr uint32_t OFFLINE_NVS_META_VERSION = 2;
static constexpr uint32_t OFFLINE_NVS_SEGMENT_BYTES = 4000; // NVS chunk max size (one page)
static constexpr const char* OFFLINE_NVS_META_KEY = "meta";

OfflineDataStoreNVS::OfflineDataStoreNVS()
{
}

OfflineDataStoreNVS::~OfflineDataStoreNVS()
{
    close();
}

bool OfflineDataStoreNVS::configure(const char* nvsNamespace, uint32_t payloadSize, uint32_t timestampBytes,
            uint32_t timestampResolutionUs, uint32_t maxEntries)
{
#ifndef ESP_PLATFORM
    (void)nvsNamespace;
    (void)payloadSize;
    (void)timestampBytes;
    (void)timestampResolutionUs;
    (void)maxEntries;
    _ready = false;
    _metaValid = false;
    return false;
#else
    if (!nvsNamespace || payloadSize == 0 || maxEntries == 0)
        return false;

    if (_nvsNamespace != nvsNamespace)
    {
        close();
        _nvsNamespace = nvsNamespace;
    }
    if (!open())
        return false;

    if (!loadMeta())
    {
        resetMeta(payloadSize, timestampBytes, timestampResolutionUs, maxEntries);
        if (_meta.recordsPerSegment == 0)
            return false;
        if (!saveMeta())
            return false;
    }
    else
    {
        bool metaCompatible = (_meta.magic == OFFLINE_NVS_META_MAGIC) &&
                (_meta.version == OFFLINE_NVS_META_VERSION) &&
                (_meta.payloadSize == payloadSize) &&
                (_meta.timestampBytes == timestampBytes) &&
                (_meta.timestampResolutionUs == timestampResolutionUs) &&
                (_meta.recordSize == (payloadSize + sizeof(uint32_t))) &&
                (_meta.recordsPerSegment > 0) &&
                (_meta.segmentBytes > 0);
        if (!metaCompatible)
        {
            LOG_W(MODULE_PREFIX, "configure meta mismatch ns %s payload %u tsBytes %u tsResUs %u",
                    _nvsNamespace.c_str(), (unsigned)payloadSize, (unsigned)timestampBytes,
                    (unsigned)timestampResolutionUs);
            clear();
            resetMeta(payloadSize, timestampBytes, timestampResolutionUs, maxEntries);
            if (_meta.recordsPerSegment == 0)
                return false;
            if (!saveMeta())
                return false;
        }
    }

    _ready = true;
    _metaValid = true;
    // Default effective max entries to the stored max entries
    _effectiveMaxEntries = _meta.maxEntries;
    return true;
#endif
}

void OfflineDataStoreNVS::setEffectiveMaxEntries(uint32_t maxEntries)
{
    if (!_metaValid)
        return;
    if (maxEntries == 0 || maxEntries > _meta.maxEntries)
        _effectiveMaxEntries = _meta.maxEntries;
    else
        _effectiveMaxEntries = maxEntries;
    if (_meta.count > _effectiveMaxEntries)
    {
        _meta.drops += (_meta.count - _effectiveMaxEntries);
        _meta.count = _effectiveMaxEntries;
        saveMeta();
    }
}

bool OfflineDataStoreNVS::appendBatch(const std::vector<uint8_t>& payloads, uint32_t payloadSize,
            const std::vector<uint32_t>& adjTsMs, uint32_t firstSeq, uint32_t count, uint32_t& outLastSeq)
{
    outLastSeq = 0;
#ifndef ESP_PLATFORM
    (void)payloads;
    (void)payloadSize;
    (void)adjTsMs;
    (void)firstSeq;
    (void)count;
    return false;
#else
    if (!_ready || !_metaValid)
        return false;
    if (payloadSize != _meta.payloadSize)
        return false;
    if (count == 0 || adjTsMs.size() < count)
        return false;
    if (payloads.size() < count * payloadSize)
        return false;

    uint32_t effectiveMaxEntries = _effectiveMaxEntries > 0 ? _effectiveMaxEntries : _meta.maxEntries;
    if (effectiveMaxEntries == 0)
        return false;

    if (_meta.count == 0)
    {
        _meta.nextSeq = firstSeq;
    }
    else if (firstSeq > _meta.nextSeq)
    {
        LOG_W(MODULE_PREFIX, "appendBatch seq gap ns %s nextSeq %u firstSeq %u (reset)",
                _nvsNamespace.c_str(), (unsigned)_meta.nextSeq, (unsigned)firstSeq);
        clear();
        resetMeta(payloadSize, _meta.timestampBytes, _meta.timestampResolutionUs, _meta.maxEntries);
        _metaValid = true;
        _ready = true;
        if (_meta.recordsPerSegment == 0)
            return false;
        if (!saveMeta())
            return false;
        _meta.nextSeq = firstSeq;
    }

    uint32_t skip = 0;
    if (firstSeq < _meta.nextSeq)
    {
        uint32_t diff = _meta.nextSeq - firstSeq;
        if (diff >= count)
        {
            outLastSeq = _meta.nextSeq > 0 ? (_meta.nextSeq - 1) : 0;
            return true; // All entries already stored
        }
        skip = diff;
    }

    std::vector<uint8_t> segBuf;
    uint32_t currentSegIdx = UINT32_MAX;
    bool segDirty = false;
    auto flushSeg = [&](void) -> bool {
        if (currentSegIdx == UINT32_MAX || !segDirty)
            return true;
        bool ok = writeSegment(currentSegIdx, segBuf.data(), segBuf.size());
        segDirty = false;
        return ok;
    };

    uint32_t recordSize = _meta.recordSize;
    uint32_t segmentBytes = _meta.recordsPerSegment * recordSize;

    segBuf.resize(segmentBytes);

    for (uint32_t ii = skip; ii < count; ii++)
    {
        uint32_t seq = firstSeq + ii;
        uint32_t writeIdx = _meta.head;
        uint32_t segIdx = writeIdx / _meta.recordsPerSegment;
        uint32_t segOffset = (writeIdx % _meta.recordsPerSegment) * recordSize;

        if (segIdx != currentSegIdx)
        {
            if (!flushSeg())
                return false;
            currentSegIdx = segIdx;
            if (!readSegment(currentSegIdx, segBuf))
                std::fill(segBuf.begin(), segBuf.end(), 0);
        }

        // Write adjusted timestamp (LE) then payload
        Raft::setLEUInt32(segBuf.data(), segOffset, adjTsMs[ii]);
        memcpy(segBuf.data() + segOffset + sizeof(uint32_t),
                payloads.data() + ii * payloadSize, payloadSize);
        segDirty = true;

        // Update ring state
        _meta.head = (_meta.head + 1) % _meta.maxEntries;
        if (_meta.count < effectiveMaxEntries)
            _meta.count++;
        else
            _meta.drops++;
        _meta.nextSeq = seq + 1;
        outLastSeq = seq;
    }

    if (!flushSeg())
        return false;
    return saveMeta();
#endif
}

bool OfflineDataStoreNVS::importTo(OfflineDataStore& dest, uint32_t importMaxEntries, uint32_t& outNextSeq)
{
    outNextSeq = 0;
#ifndef ESP_PLATFORM
    (void)dest;
    (void)importMaxEntries;
    return false;
#else
    if (!_ready || !_metaValid)
        return false;
    outNextSeq = _meta.nextSeq;
    if (_meta.count == 0)
        return false;

    uint32_t firstSeqInStore = (_meta.nextSeq >= _meta.count) ? (_meta.nextSeq - _meta.count) : 0;
    uint32_t startSeq = _meta.importSeq + 1;
    if (startSeq < firstSeqInStore)
        startSeq = firstSeqInStore;
    if (startSeq >= _meta.nextSeq)
        return true;

    uint32_t maxEntries = dest.getMaxEntries();
    if (importMaxEntries > 0 && importMaxEntries < maxEntries)
        maxEntries = importMaxEntries;
    if (maxEntries == 0)
        return false;

    uint32_t available = _meta.nextSeq - startSeq;
    uint32_t importCount = (available < maxEntries) ? available : maxEntries;
    if (importCount == 0)
        return false;

    uint32_t recordSize = _meta.recordSize;
    uint32_t segmentBytes = _meta.recordsPerSegment * recordSize;
    std::vector<uint8_t> segBuf;
    segBuf.resize(segmentBytes);

    uint32_t tail = (_meta.head + _meta.maxEntries - _meta.count) % _meta.maxEntries;
    uint32_t startIdx = (tail + (startSeq - firstSeqInStore)) % _meta.maxEntries;
    uint32_t firstSeq = startSeq;

    std::vector<uint8_t> record;
    record.resize(_meta.payloadSize);
    uint32_t currentSegIdx = UINT32_MAX;

    for (uint32_t ii = 0; ii < importCount; ii++)
    {
        uint32_t recordIdx = (startIdx + ii) % _meta.maxEntries;
        uint32_t segIdx = recordIdx / _meta.recordsPerSegment;
        uint32_t segOffset = (recordIdx % _meta.recordsPerSegment) * recordSize;

        if (segIdx != currentSegIdx)
        {
            currentSegIdx = segIdx;
            if (!readSegment(currentSegIdx, segBuf))
                return false;
        }

        const uint8_t* pRec = segBuf.data() + segOffset;
        const uint8_t* pRecEnd = pRec + recordSize;
        uint32_t adjTsMs = Raft::getLEUInt32AndInc(pRec, pRecEnd);
        memcpy(record.data(), pRec, _meta.payloadSize);
        uint64_t timeNowUs = (ii == 0) ? ((uint64_t)adjTsMs * 1000ULL) : 0;
        dest.put(timeNowUs, firstSeq + ii, record);
        if ((ii % 512) == 0)
            delay(1);
    }
    _meta.importSeq = firstSeq + importCount - 1;
    saveMeta();
    return true;
#endif
}

void OfflineDataStoreNVS::clear()
{
#ifdef ESP_PLATFORM
    if (!open())
        return;
    nvs_erase_all(_nvsHandle);
    nvs_commit(_nvsHandle);
#endif
    _metaValid = false;
    _ready = false;
}

bool OfflineDataStoreNVS::isReady() const
{
    return _ready && _metaValid;
}

uint32_t OfflineDataStoreNVS::getCount() const
{
    return _metaValid ? _meta.count : 0;
}

uint32_t OfflineDataStoreNVS::getNextSeq() const
{
    return _metaValid ? _meta.nextSeq : 0;
}

bool OfflineDataStoreNVS::open()
{
#ifndef ESP_PLATFORM
    return false;
#else
    if (_nvsHandle != 0)
        return true;
    if (_nvsNamespace.empty())
        return false;
    esp_err_t err = nvs_open(_nvsNamespace.c_str(), NVS_READWRITE, reinterpret_cast<nvs_handle_t*>(&_nvsHandle));
    if (err != ESP_OK)
    {
        LOG_W(MODULE_PREFIX, "open nvs namespace %s err %d", _nvsNamespace.c_str(), err);
        _nvsHandle = 0;
        return false;
    }
    return true;
#endif
}

void OfflineDataStoreNVS::close()
{
#ifdef ESP_PLATFORM
    if (_nvsHandle != 0)
    {
        nvs_close((nvs_handle_t)_nvsHandle);
        _nvsHandle = 0;
    }
#endif
}

bool OfflineDataStoreNVS::loadMeta()
{
#ifndef ESP_PLATFORM
    return false;
#else
    if (!open())
        return false;
    size_t len = sizeof(_meta);
    esp_err_t err = nvs_get_blob((nvs_handle_t)_nvsHandle, OFFLINE_NVS_META_KEY, &_meta, &len);
    if (err != ESP_OK || len != sizeof(_meta))
    {
        _metaValid = false;
        return false;
    }
    _metaValid = (_meta.magic == OFFLINE_NVS_META_MAGIC) && (_meta.version == OFFLINE_NVS_META_VERSION);
    return _metaValid;
#endif
}

bool OfflineDataStoreNVS::saveMeta()
{
#ifndef ESP_PLATFORM
    return false;
#else
    if (!open())
        return false;
    esp_err_t err = nvs_set_blob((nvs_handle_t)_nvsHandle, OFFLINE_NVS_META_KEY, &_meta, sizeof(_meta));
    if (err != ESP_OK)
    {
        LOG_W(MODULE_PREFIX, "saveMeta fail ns %s err %d", _nvsNamespace.c_str(), err);
        return false;
    }
    err = nvs_commit((nvs_handle_t)_nvsHandle);
    if (err != ESP_OK)
    {
        LOG_W(MODULE_PREFIX, "saveMeta commit fail ns %s err %d", _nvsNamespace.c_str(), err);
        return false;
    }
    return true;
#endif
}

bool OfflineDataStoreNVS::readSegment(uint32_t segIdx, std::vector<uint8_t>& outBuf) const
{
#ifndef ESP_PLATFORM
    (void)segIdx;
    (void)outBuf;
    return false;
#else
    if (_nvsHandle == 0)
        return false;
    char key[16];
    makeSegmentKey(segIdx, key, sizeof(key));
    size_t len = outBuf.size();
    esp_err_t err = nvs_get_blob((nvs_handle_t)_nvsHandle, key, outBuf.data(), &len);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return false;
    if (err != ESP_OK || len != outBuf.size())
    {
        LOG_W(MODULE_PREFIX, "readSegment fail ns %s key %s err %d len %u",
                _nvsNamespace.c_str(), key, err, (unsigned)len);
        return false;
    }
    return true;
#endif
}

bool OfflineDataStoreNVS::writeSegment(uint32_t segIdx, const uint8_t* data, size_t len)
{
#ifndef ESP_PLATFORM
    (void)segIdx;
    (void)data;
    (void)len;
    return false;
#else
    if (!open())
        return false;
    char key[16];
    makeSegmentKey(segIdx, key, sizeof(key));
    esp_err_t err = nvs_set_blob((nvs_handle_t)_nvsHandle, key, data, len);
    if (err != ESP_OK)
    {
        LOG_W(MODULE_PREFIX, "writeSegment fail ns %s key %s err %d", _nvsNamespace.c_str(), key, err);
        return false;
    }
    return true;
#endif
}

void OfflineDataStoreNVS::makeSegmentKey(uint32_t segIdx, char* key, size_t keySize) const
{
    if (!key || keySize == 0)
        return;
    snprintf(key, keySize, "s%05u", (unsigned)segIdx);
    key[keySize - 1] = 0;
}

void OfflineDataStoreNVS::resetMeta(uint32_t payloadSize, uint32_t timestampBytes,
            uint32_t timestampResolutionUs, uint32_t maxEntries)
{
    _meta.magic = OFFLINE_NVS_META_MAGIC;
    _meta.version = OFFLINE_NVS_META_VERSION;
    _meta.payloadSize = payloadSize;
    _meta.recordSize = payloadSize + sizeof(uint32_t);
    _meta.timestampBytes = timestampBytes;
    _meta.timestampResolutionUs = timestampResolutionUs;
    _meta.maxEntries = maxEntries;
    _meta.head = 0;
    _meta.count = 0;
    _meta.nextSeq = 0;
    _meta.importSeq = 0;
    _meta.segmentBytes = OFFLINE_NVS_SEGMENT_BYTES;
    _meta.recordsPerSegment = _meta.recordSize ? (_meta.segmentBytes / _meta.recordSize) : 0;
    _meta.drops = 0;
    if (_meta.recordsPerSegment == 0)
    {
        LOG_W(MODULE_PREFIX, "resetMeta invalid recordSize %u segmentBytes %u",
                (unsigned)_meta.recordSize, (unsigned)_meta.segmentBytes);
    }
}
