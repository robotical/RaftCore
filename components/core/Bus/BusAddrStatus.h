/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Bus Address Status
//
// Rob Dobson 2024
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "RaftDeviceConsts.h"
#include "DeviceStatus.h"

enum class DeviceOnlineState
{
    INITIAL,
    ONLINE,
    OFFLINE
};

// Bus address status
class BusAddrStatus
{
public:
    BusAddrStatus() = default;
    BusAddrStatus(BusElemAddrType address, DeviceOnlineState onlineState, bool isChange, bool isNewlyIdentified)
        : address(address), isChange(isChange), isOnline(onlineState == DeviceOnlineState::ONLINE),
          wasOnceOnline(onlineState == DeviceOnlineState::ONLINE), isNewlyIdentified(isNewlyIdentified),
          onlineState(onlineState)
    {
    }

    // Address and slot
    BusElemAddrType address = 0;

    // Max failures before declaring a bus element offline
    static const uint32_t ADDR_RESP_COUNT_FAIL_MAX_DEFAULT = 3;

    // Max successes before declaring a bus element online
    static const uint32_t ADDR_RESP_COUNT_OK_MAX_DEFAULT = 2;

    // Online/offline count
    int8_t count = 0;

    // State
    bool isChange : 1 = false;
    bool isOnline : 1 = false;
    bool wasOnceOnline : 1 = false;
    bool slotResolved : 1 = false;
    bool isNewlyIdentified : 1 = false;
    bool flagForDeletion : 1 = false;
    DeviceOnlineState onlineState = DeviceOnlineState::INITIAL;

    // Access barring
    uint32_t barStartMs = 0;
    uint16_t barDurationMs = 0;

    // Min between data change callbacks
    uint32_t minTimeBetweenReportsMs = 0;
    uint32_t lastDataChangeReportTimeMs = 0;

    // Device status
    DeviceStatus deviceStatus;

    // Device data change callback and info
    RaftDeviceDataChangeCB dataChangeCB = nullptr;
    const void* pCallbackInfo = nullptr;

    // Handle responding
    bool handleResponding(bool isResponding, bool &flagSpuriousRecord, 
            uint32_t okMax = ADDR_RESP_COUNT_OK_MAX_DEFAULT, 
            uint32_t failMax = ADDR_RESP_COUNT_FAIL_MAX_DEFAULT);

    static const char* getOnlineStateStr(DeviceOnlineState state)
    {
        switch (state)
        {
            case DeviceOnlineState::INITIAL: return "INITIAL";
            case DeviceOnlineState::ONLINE: return "ONLINE";
            case DeviceOnlineState::OFFLINE: return "OFFLINE";
            default: return "UNKNOWN";
        }
    }
    
    // Register for data change
    void registerForDataChange(RaftDeviceDataChangeCB dataChangeCB, uint32_t minTimeBetweenReportsMs, const void* pCallbackInfo)
    {
        this->dataChangeCB = dataChangeCB;
        this->pCallbackInfo = pCallbackInfo;
        this->minTimeBetweenReportsMs = minTimeBetweenReportsMs;
    }

    // Get device data change callback
    RaftDeviceDataChangeCB getDataChangeCB() const
    {
        return dataChangeCB;
    }

    // Get device data change callback info
    const void* getCallbackInfo() const
    {
        return pCallbackInfo;
    }

    // Get JSON for device status
    String getJson() const;
};
