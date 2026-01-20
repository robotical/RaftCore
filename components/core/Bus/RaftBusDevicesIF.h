/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Bus Devices Interface
//
// Rob Dobson 2024
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stdint.h>
#include <vector>
#include "RaftBusConsts.h"
#include "RaftArduino.h"
#include "RaftDeviceConsts.h"
#include "DevicePollingInfo.h"
#include "OfflineDataStore.h"
#include <set>
#include <string>
#include <map>

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Device decode state
/// @class RaftBusDeviceDecodeState
class RaftBusDeviceDecodeState
{
public:
    uint64_t lastReportTimestampUs = 0;
    uint64_t reportTimestampOffsetUs = 0;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Bus Devices Interface
/// @class RaftBusDevicesIF
class RaftBusDevicesIF
{
public:

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get list of device addresses attached to the bus
    /// @param pAddrList pointer to array to receive addresses
    /// @param onlyAddressesWithIdentPollResponses true to only return addresses with ident poll responses
    virtual void getDeviceAddresses(std::vector<BusElemAddrType>& addresses, bool onlyAddressesWithIdentPollResponses) const = 0;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get device type information by address
    /// @param address address of device to get information for
    /// @param includePlugAndPlayInfo true to include plug and play information
    /// @return JSON string
    virtual String getDevTypeInfoJsonByAddr(BusElemAddrType address, bool includePlugAndPlayInfo) const = 0;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get device type information by device type name
    /// @param deviceType - device type name
    /// @param includePlugAndPlayInfo - true to include plug and play information
    /// @return JSON string
    virtual String getDevTypeInfoJsonByTypeName(const String& deviceType, bool includePlugAndPlayInfo) const = 0;

    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get device type info JSON by device type index
    /// @param deviceTypeIdx device type index
    /// @param includePlugAndPlayInfo include plug and play info
    /// @return JSON string
    virtual String getDevTypeInfoJsonByTypeIdx(uint16_t deviceTypeIdx, bool includePlugAndPlayInfo) const = 0;

    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get queued device data in JSON format
    /// @return JSON string
    virtual String getQueuedDeviceDataJson(uint32_t maxResponsesToReturn = 0, uint32_t* pRemaining = nullptr) const = 0;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get queued device data in binary format
    /// @param connMode connection mode (inc bus number)
    /// @return Binary data vector
    virtual std::vector<uint8_t> getQueuedDeviceDataBinary(uint32_t connMode, uint32_t maxResponsesToReturn = 0,
                uint32_t* pRemaining = nullptr) const = 0;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get decoded poll responses
    /// @param address address of device to get data from
    /// @param pStructOut pointer to structure (or array of structures) to receive decoded data
    /// @param structOutSize size of structure (in bytes) to receive decoded data
    /// @param maxRecCount maximum number of records to decode
    /// @param decodeState decode state for this device
    /// @return number of records decoded
    /// @note the pStructOut should generally point to structures of the correct type for the device data and the
    ///       decodeState should be maintained between calls for the same device
    virtual uint32_t getDecodedPollResponses(BusElemAddrType address, 
                    void* pStructOut, uint32_t structOutSize, 
                    uint16_t maxRecCount, RaftBusDeviceDecodeState& decodeState) const = 0;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Store poll results
    /// @param timeNowUs time in us (passed in to aid testing)
    /// @param address address
    /// @param pollResultData poll result data
    /// @param pPollInfo pointer to device polling info (maybe nullptr)
    /// @return true if result stored
    virtual bool handlePollResult(uint64_t timeNowUs, BusElemAddrType address, 
                            const std::vector<uint8_t>& pollResultData, const DevicePollingInfo* pPollInfo)
    {
        return false;
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Register for device data notifications
    /// @param addrAndSlot address and slot
    /// @param dataChangeCB Callback for data change
    /// @param minTimeBetweenReportsMs Minimum time between reports (ms)
    /// @param pCallbackInfo Callback info (passed to the callback)
    virtual void registerForDeviceData(BusElemAddrType address, RaftDeviceDataChangeCB dataChangeCB, 
                uint32_t minTimeBetweenReportsMs, const void* pCallbackInfo)
    {
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get debug JSON
    /// @return JSON string
    virtual String getDebugJSON(bool includeBraces) const
    {
        if (!includeBraces)
            return "";
        return "{}";
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get offline stats
    /// @param address device address
    virtual OfflineDataStats getOfflineStats(BusElemAddrType address) const
    {
        (void)address;
        return OfflineDataStats();
    }

    // Optional offline control interfaces (default no-op)
    virtual void setOfflineMaxPerPublishOverride(uint32_t maxPerPublish) {(void)maxPerPublish;}
    virtual void setOfflineDrainSelection(const std::vector<BusElemAddrType>& addresses, const std::vector<std::string>& typeNames,
                bool drainOnlySelected) {(void)addresses; (void)typeNames; (void)drainOnlySelected;}
    virtual void setOfflineBufferPaused(const std::vector<BusElemAddrType>& addresses, bool paused) {(void)addresses; (void)paused;}
    virtual void setOfflineDrainPaused(const std::vector<BusElemAddrType>& addresses, bool paused) {(void)addresses; (void)paused;}
    virtual void setOfflineDrainLinkPaused(bool paused) {(void)paused;}
    virtual void setOfflineAutoResume(bool enabled, const std::vector<BusElemAddrType>& addresses, uint32_t rateOverrideMs)
    {
        (void)enabled;
        (void)addresses;
        (void)rateOverrideMs;
    }
    virtual void resetOfflineBuffers(const std::vector<BusElemAddrType>& addresses) {(void)addresses;}
    virtual void getOfflineControlSnapshot(std::set<BusElemAddrType>& bufferPaused, std::set<BusElemAddrType>& drainPaused,
                std::set<BusElemAddrType>& drainSelectedAddrs, std::set<std::string>& drainSelectedTypes,
                bool& drainOnlySelected, uint32_t& maxPerPublishOverride,
                bool& globalBufferPaused, bool& globalDrainPaused,
                std::map<BusElemAddrType, uint32_t>& rateOverridesUs) const
    {
        bufferPaused.clear();
        drainPaused.clear();
        drainSelectedAddrs.clear();
        drainSelectedTypes.clear();
        drainOnlySelected = false;
        maxPerPublishOverride = 0;
        globalBufferPaused = false;
        globalDrainPaused = false;
        rateOverridesUs.clear();
    }
    virtual bool getDeviceTypeName(BusElemAddrType address, std::string& typeName) const
    {
        (void)address;
        typeName.clear();
        return false;
    }
    virtual String peekOfflineDataJson(const std::vector<BusElemAddrType>& addresses,
                uint32_t startIdx, uint32_t maxResponsesToReturn, uint32_t maxBytes,
                uint32_t& totalRemaining)
    {
        (void)addresses;
        (void)startIdx;
        (void)maxResponsesToReturn;
        (void)maxBytes;
        totalRemaining = 0;
        return "{}";
    }
    virtual bool applyOfflineRateOverride(const std::vector<BusElemAddrType>& addresses, uint32_t pollRateMs)
    {
        (void)addresses;
        (void)pollRateMs;
        return false;
    }
    virtual bool clearOfflineRateOverride(const std::vector<BusElemAddrType>& addresses)
    {
        (void)addresses;
        return false;
    }
    virtual bool rebalanceOfflineBuffers(const std::vector<BusElemAddrType>& addresses)
    {
        (void)addresses;
        return false;
    }
    struct EstAllocInfo
    {
        uint32_t allocBytes = 0;
        uint32_t bytesPerEntry = 0;
        uint32_t payloadSize = 0;
        uint32_t metaSize = 0;
    };

    virtual bool estimateOfflineAllocations(const std::vector<BusElemAddrType>& addresses,
                std::map<BusElemAddrType, EstAllocInfo>& allocBytesOut) const
    {
        (void)addresses;
        allocBytesOut.clear();
        return false;
    }
};
