////////////////////////////////////////////////////////////////////////////////
//
// DeviceManager.cpp
//
////////////////////////////////////////////////////////////////////////////////

#include <functional>
#include <algorithm>
#include <set>
#include <map>
#include <string>
#include <cstdlib>
#include <cctype>
#include "DeviceManager.h"
#include "DeviceTypeRecordDynamic.h"
#include "DeviceTypeRecords.h"
#include "RaftBusSystem.h"
#include "RaftBusDevice.h"
#include "DeviceFactory.h"
#include "SysManager.h"
#include "RestAPIEndpointManager.h"
#include "DemoDevice.h"
#include "RaftJsonPrefixed.h"
#include "OfflineDataStore.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

// Warnings
#define WARN_ON_DEVICE_CLASS_NOT_FOUND
#define WARN_ON_DEVICE_INSTANTIATION_FAILED
#define WARN_ON_SETUP_DEVICE_FAILED

// Debug
#define DEBUG_BUS_OPERATION_STATUS_OK_CB
#define DEBUG_NEW_DEVICE_FOUND_CB
// #define DEBUG_DEVICE_SETUP
// #define DEBUG_DEVICE_FACTORY
// #define DEBUG_LIST_DEVICES
// #define DEBUG_JSON_DEVICE_DATA
using EstAllocInfo = RaftBusDevicesIF::EstAllocInfo;
// #define DEBUG_BINARY_DEVICE_DATA
// #define DEBUG_JSON_DEVICE_HASH
// #define DEBUG_DEVMAN_API
// #define DEBUG_BUS_ELEMENT_STATUS
// #define DEBUG_GET_DEVICE
// #define DEBUG_JSON_DEVICE_HASH_DETAIL
// #define DEBUG_MAKE_BUS_REQUEST_VERBOSE
// #define DEBUG_API_CMDRAW

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Constructor
DeviceManager::DeviceManager(const char *pModuleName, RaftJsonIF& sysConfig)
    : RaftSysMod(pModuleName, sysConfig)
{
    // Access semaphore
    _accessMutex = xSemaphoreCreateMutex();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Destructor
DeviceManager::~DeviceManager()
{
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Setup function
void DeviceManager::setup()
{
    // Setup buses
    raftBusSystem.setup("Buses", modConfig(),
            std::bind(&DeviceManager::busElemStatusCB, this, std::placeholders::_1, std::placeholders::_2),
            std::bind(&DeviceManager::busOperationStatusCB, this, std::placeholders::_1, std::placeholders::_2)
    );

    // Offline publish defaults
    RaftJsonPrefixed offlineCfg(modConfig(), "offlineBuffer");
    _maxPublishSamplesPerDevice = offlineCfg.getLong("maxPerPublish", 32);

    // Setup device classes (these are the keys into the device factory)
    setupDevices("Devices", modConfig());
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Post setup function
/// @note This handles post-setup for statically added devices (dynamic devices are handled separately)
void DeviceManager::postSetup()
{
    // Register JSON data source (message generator and state detector functions)
    getSysManager()->registerDataSource("Publish", "devjson", 
        [this](const char* messageName, CommsChannelMsg& msg) {
            uint32_t remaining = 0;
            String statusStr = getDevicesDataJSON(_maxPublishSamplesPerDevice, &remaining);
            msg.setFromBuffer((uint8_t*)statusStr.c_str(), statusStr.length());
            msg.setBacklogHint(remaining > 0, remaining);
            return statusStr.length() > 0;
        },
        [this](const char* messageName, std::vector<uint8_t>& stateHash) {
            return getDevicesHash(stateHash);
        }
    );    

    // Register binary data source (new)
    getSysManager()->registerDataSource("Publish", "devbin", 
        [this](const char* messageName, CommsChannelMsg& msg) {
            uint32_t remaining = 0;
            std::vector<uint8_t> binaryData = getDevicesDataBinary(_maxPublishSamplesPerDevice, &remaining);
            msg.setFromBuffer(binaryData.data(), binaryData.size());
            msg.setBacklogHint(remaining > 0, remaining);
            return binaryData.size() > 0;
        },
        [this](const char* messageName, std::vector<uint8_t>& stateHash) {
            return getDevicesHash(stateHash);
        }
    );

    // Get a frozen copy of the device list (null-pointers excluded)
    RaftDevice* pDeviceListCopy[DEVICE_LIST_MAX_SIZE];
    uint32_t numDevices = getDeviceListFrozen(pDeviceListCopy, DEVICE_LIST_MAX_SIZE);

    // Loop through the devices
    for (uint32_t devIdx = 0; devIdx < numDevices; devIdx++)
    {
        pDeviceListCopy[devIdx]->postSetup();
    }

    // Register for device data change callbacks
#ifdef DEBUG_DEVICE_SETUP
    uint32_t numDevCBsRegistered = 
#endif
    registerForDeviceDataChangeCBs();

    // Register for device events
    for (uint32_t devIdx = 0; devIdx < numDevices; devIdx++)
    {
        pDeviceListCopy[devIdx]->registerForDeviceStatusChange(
            std::bind(&DeviceManager::deviceEventCB, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)
        );
    }

    // Debug
#ifdef DEBUG_DEVICE_SETUP
    LOG_I(MODULE_PREFIX, "postSetup %d devices registered %d CBs", numDevices, numDevCBsRegistered);
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Loop function
void DeviceManager::loop()
{
    // Service the buses
    raftBusSystem.loop();

    // Get a frozen copy of the device list
    RaftDevice* pDeviceListCopy[DEVICE_LIST_MAX_SIZE];
    uint32_t numDevices = getDeviceListFrozen(pDeviceListCopy, DEVICE_LIST_MAX_SIZE);

    // Loop through the devices
    for (uint32_t devIdx = 0; devIdx < numDevices; devIdx++)
    {
        // Handle device loop
        pDeviceListCopy[devIdx]->loop();
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Bus operation status callback
/// @param bus a reference to the bus which has changed status
/// @param busOperationStatus - indicates bus ok/failing
void DeviceManager::busOperationStatusCB(RaftBus& bus, BusOperationStatus busOperationStatus)
{
    // Debug
#ifdef DEBUG_BUS_OPERATION_STATUS_OK_CB
    LOG_I(MODULE_PREFIX, "busOperationStatusInfo %s %s", bus.getBusName().c_str(), 
        RaftBus::busOperationStatusToString(busOperationStatus));
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Bus element status callback
/// @param bus a reference to the bus which has changed status
/// @param statusChanges - an array of status changes (online/offline) for bus elements
void DeviceManager::busElemStatusCB(RaftBus& bus, const std::vector<BusElemAddrAndStatus>& statusChanges)
{
    // Handle the status changes
    for (const auto& el : statusChanges)
    {
        // Find the device
        String deviceId = bus.formUniqueId(el.address);
        RaftDevice* pDevice = getDeviceByID(deviceId.c_str());
        bool newlyCreated = false;
        if (!pDevice)
        {
            // Check if device newly created
            if (el.isNewlyIdentified)
            {
                // Generate config JSON for the device
                String devConfig = "{\"name\":" + deviceId + "}";

                // Create the device
                pDevice = new RaftBusDevice(bus.getBusName().c_str(), el.address, "RaftBusDevice", devConfig.c_str());
                pDevice->setDeviceTypeIndex(el.deviceTypeIndex);
                newlyCreated = true;

                // Add to the list of instantiated devices & setup
                if (xSemaphoreTake(_accessMutex, pdMS_TO_TICKS(5)) == pdTRUE)
                {
                    // Add to the list of instantiated devices
                    _deviceList.push_back(pDevice);
                    xSemaphoreGive(_accessMutex);

                    // Setup device
                    pDevice->setup();
                    pDevice->postSetup();

                    // Debug
#ifdef DEBUG_NEW_DEVICE_FOUND_CB
                    LOG_I(MODULE_PREFIX, "busElemStatusCB new device %s name %s class %s pubType %s", 
                                    deviceId.c_str(), 
                                    pDevice->getDeviceName().c_str(), 
                                    pDevice->getDeviceClassName().c_str(),
                                    pDevice->getPublishDeviceType().c_str());
#endif
                }
                else
                {
                    // Delete the device to avoid memory leak
                    delete pDevice;
                    pDevice = nullptr;

                    // Debug
                    LOG_E(MODULE_PREFIX, "busElemStatusCB failed to add device %s", deviceId.c_str());
                }
            }
        }

        // Handle status update
        if (pDevice)
        {
            // Handle device status change
            pDevice->handleStatusChange(el.isChangeToOnline, el.isChangeToOffline, el.isNewlyIdentified, el.deviceTypeIndex);

            // Handle device status change callbacks
            callDeviceStatusChangeCBs(pDevice, el, newlyCreated);

            // If newly created, register for device data notifications for this specific device
            if (newlyCreated)
            {
                registerForDeviceDataChangeCBs(pDevice->getDeviceName().c_str());
            }

            if (!el.isChangeToOnline){
                LOG_I(MODULE_PREFIX, "Device is offline. Deleting - %s", deviceId.c_str());
                if (xSemaphoreTake(_accessMutex, pdMS_TO_TICKS(5)) == pdTRUE)
                {
                    // Delete from the list. It will be re-identified if reconnected. If continuity is required it should be handled at the frontend
                    _deviceList.remove(pDevice);
                    xSemaphoreGive(_accessMutex);
                }
            }
        }
        
        // Debug
#ifdef DEBUG_BUS_ELEMENT_STATUS
        LOG_I(MODULE_PREFIX, "busElemStatusInfo ID %s %s%s%s%s",
                        deviceId.c_str(), 
                        el.isChangeToOnline ? "Online" : ("Offline" + String(el.isChangeToOffline ? " (was online)" : "")).c_str(),
                        el.isNewlyIdentified ? (" DevTypeIdx " + String(el.deviceTypeIndex)).c_str() : "",
                        newlyCreated ? " NewlyCreated" : "",
                        pDevice ? "" : " NOT IDENTIFIED YET");
#endif
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Setup devices
/// @param pConfigPrefix prefix for the device configuration
/// @param devManConfig configuration for the device manager
void DeviceManager::setupDevices(const char* pConfigPrefix, RaftJsonIF& devManConfig)
{
    // Get devices config
    std::vector<String> deviceConfigs;
    devManConfig.getArrayElems(pConfigPrefix, deviceConfigs);
    for (RaftJson devConf : deviceConfigs)
    {
        // Check if enable is explicitly set to false
        if (!devConf.getBool("enable", true))
            continue;

        // Get class of device
        String devClass = devConf.getString("class", "");

        // Find the device class in the factory
        const DeviceFactory::RaftDeviceClassDef* pDeviceClassDef = deviceFactory.findDeviceClass(devClass.c_str());
        if (!pDeviceClassDef)
        {
#ifdef WARN_ON_DEVICE_CLASS_NOT_FOUND
            LOG_W(MODULE_PREFIX, "setupDevices %s class %s not found", pConfigPrefix, devClass.c_str());
#endif
            continue;
        }

        // Create the device
        auto pDevice = pDeviceClassDef->pCreateFn(devClass.c_str(), devConf.c_str());
        if (!pDevice)
        {
#ifdef WARN_ON_DEVICE_INSTANTIATION_FAILED
            LOG_E(MODULE_PREFIX, "setupDevices %s class %s create failed devConf %s", 
                        pConfigPrefix, devClass.c_str(), devConf.c_str());
#endif
            continue;
        }

        // Add to the list of instantiated devices
        _deviceList.push_back(pDevice);

        // Debug
#ifdef DEBUG_DEVICE_FACTORY
        {
            LOG_I(MODULE_PREFIX, "setup class %s devConf %s", 
                        devClass.c_str(), devConf.c_str());
        }
#endif

    }

    // Now call setup on instantiated devices
    for (auto* pDevice : _deviceList)
    {
        if (pDevice)
        {
#ifdef DEBUG_DEVICE_SETUP            
            LOG_I(MODULE_PREFIX, "setup pDevice %p name %s", pDevice, pDevice->getDeviceName());
#endif
            // Setup device
            pDevice->setup();

            // See if the device has a device type record
            DeviceTypeRecordDynamic devTypeRec;
            if (pDevice->getDeviceTypeRecord(devTypeRec))
            {
                // Add the device type record to the device type records
                uint16_t deviceTypeIndex = 0;
                deviceTypeRecords.addExtendedDeviceTypeRecord(devTypeRec, deviceTypeIndex);
                pDevice->setDeviceTypeIndex(deviceTypeIndex);
            }
        }
    }

    // Give each SysMod the opportunity to add endpoints and comms channels and to keep a
    // pointer to the CommsCoreIF that can be used to send messages
    for (auto* pDevice : _deviceList)
    {
        if (pDevice)
        {
            if (getRestAPIEndpointManager())
                pDevice->addRestAPIEndpoints(*getRestAPIEndpointManager());
            if (getCommsCore())
                pDevice->addCommsChannels(*getCommsCore());
        }            
    }

#ifdef DEBUG_LIST_DEVICES
    uint32_t deviceIdx = 0;
    for (auto* pDevice : _deviceList)
    {
        LOG_I(MODULE_PREFIX, "Device %d: %s", deviceIdx++, 
                pDevice ? pDevice->getDeviceName() : "UNKNOWN");
            
    }
    if (_deviceList.size() == 0)
        LOG_I(MODULE_PREFIX, "No devices found");
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Setup a single device
/// @param pDeviceClass class of the device to setup
/// @param devConfig configuration for the device
/// @return RaftDevice* pointer to the created device or nullptr if failed
RaftDevice* DeviceManager::setupDevice(const char* pDeviceClass, RaftJsonIF& devConfig)
{
    // Check valid
    if (!pDeviceClass)
    {
#ifdef WARN_ON_SETUP_DEVICE_FAILED
        LOG_E(MODULE_PREFIX, "setupDevice invalid parameters pDeviceClass %s", 
                pDeviceClass ? pDeviceClass : "NULL");
#endif
        return nullptr;
    }

    // Find the device class in the factory
    const DeviceFactory::RaftDeviceClassDef* pDeviceClassDef = deviceFactory.findDeviceClass(pDeviceClass);
    if (!pDeviceClassDef)
    {
#ifdef WARN_ON_SETUP_DEVICE_FAILED
        LOG_W(MODULE_PREFIX, "setupDevice class %s not found", pDeviceClass);
#endif
        return nullptr;
    }
    // Create the device
    auto pDevice = pDeviceClassDef->pCreateFn(pDeviceClass, devConfig.c_str());
    if (!pDevice)
    {
#ifdef WARN_ON_SETUP_DEVICE_FAILED
        LOG_E(MODULE_PREFIX, "setupDevice class %s create failed devConf %s", 
                pDeviceClass, devConfig.c_str());
#endif
        return nullptr;
    }
    // Add to the list of instantiated devices
    if (xSemaphoreTake(_accessMutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        // Add to the list of instantiated devices
        _deviceList.push_back(pDevice);
        xSemaphoreGive(_accessMutex);
        // Setup device
        pDevice->setup();
        pDevice->postSetup();
#ifdef DEBUG_DEVICE_SETUP
        LOG_I(MODULE_PREFIX, "setupDevice %s devConf %s", 
                pDeviceClass, devConfig.c_str());
#endif
    }
    else
    {
#ifdef WARN_ON_SETUP_DEVICE_FAILED
        LOG_E(MODULE_PREFIX, "setupDevice failed to add device %s", pDeviceClass);
#endif
        // Delete the device to avoid memory leak
        delete pDevice;
        pDevice = nullptr;
    }

    // If the device has a device type record, add it to the device type records
    DeviceTypeRecordDynamic devTypeRec;
    if (pDevice && pDevice->getDeviceTypeRecord(devTypeRec))
    {
        // Add the device type record to the device type records
        uint16_t deviceTypeIndex = 0;
        deviceTypeRecords.addExtendedDeviceTypeRecord(devTypeRec, deviceTypeIndex);
        pDevice->setDeviceTypeIndex(deviceTypeIndex);
    }

    // If the device was successfully created, register for device data notifications
    if (pDevice)
    {
        // Register for device data notifications
        registerForDeviceDataChangeCBs(pDevice->getDeviceName().c_str());
    }

    // Debug
#ifdef DEBUG_DEVICE_SETUP
    if (pDevice)
    {
        LOG_I(MODULE_PREFIX, "setupDevice %s name %s class %s pubType %s", 
                        pDeviceClass, 
                        pDevice->getDeviceName().c_str(),
                        pDevice->getDeviceClassName().c_str(),
                        pDevice->getPublishDeviceType().c_str());
    }
#endif

    // Return success
    return pDevice;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get devices' data as JSON
/// @return JSON string
String DeviceManager::getDevicesDataJSON(uint32_t maxResponsesToReturn, uint32_t* pRemaining) const
{
    // JSON strings
    String jsonStrBus, jsonStrDev;
    uint32_t remainingTotal = 0;

    // Check all buses for data
    for (RaftBus* pBus : raftBusSystem.getBusList())
    {
        if (!pBus)
            continue;
        // Get device interface
        RaftBusDevicesIF* pDevicesIF = pBus->getBusDevicesIF();
        if (!pDevicesIF)
            continue; 
        uint32_t busRemaining = 0;
        String jsonRespStr = pDevicesIF->getQueuedDeviceDataJson(maxResponsesToReturn, &busRemaining);
        remainingTotal += busRemaining;

        // Check for empty string or empty JSON object
        if (jsonRespStr.length() > 2)
        {
            jsonStrBus += (jsonStrBus.length() == 0 ? "\"" : ",\"") + pBus->getBusName() + "\":" + jsonRespStr;
        }
    }

    // Get a frozen copy of the device list (null-pointers excluded)
    RaftDevice* pDeviceListCopy[DEVICE_LIST_MAX_SIZE];
    uint32_t numDevices = getDeviceListFrozen(pDeviceListCopy, DEVICE_LIST_MAX_SIZE);

    // Loop through the devices
    for (uint32_t devIdx = 0; devIdx < numDevices; devIdx++)
    {
        RaftDevice* pDevice = pDeviceListCopy[devIdx];
        String jsonRespStr = pDevice->getStatusJSON();

        // Check for empty string or empty JSON object
        if (jsonRespStr.length() > 2)
        {
            jsonStrDev += (jsonStrDev.length() == 0 ? "\"" : ",\"") + pDevice->getPublishDeviceType() + "\":" + jsonRespStr;
        }
    }

#ifdef DEBUG_JSON_DEVICE_DATA
    LOG_I(MODULE_PREFIX, "getDevicesDataJSON BUS %s DEV %s ", jsonStrBus.c_str(), jsonStrDev.c_str());
#endif

    if (jsonStrBus.length() == 0 && jsonStrDev.length() == 0)
    {
        // No data available
        return "";
    }

    if (pRemaining)
        *pRemaining = remainingTotal;

    return "{" + (jsonStrBus.length() == 0 ? (jsonStrDev.length() == 0 ? "" : jsonStrDev) : (jsonStrDev.length() == 0 ? jsonStrBus : jsonStrBus + "," + jsonStrDev)) + "}";
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get devices' data as binary
/// @return Binary data vector
std::vector<uint8_t> DeviceManager::getDevicesDataBinary(uint32_t maxResponsesToReturn, uint32_t* pRemaining) const
{
    std::vector<uint8_t> binaryData;
    binaryData.reserve(500);
    uint32_t remainingTotal = 0;

    // Add bus data
    uint16_t connModeBusNum = DEVICE_CONN_MODE_FIRST_BUS;
    for (RaftBus* pBus : raftBusSystem.getBusList())
    {
        if (!pBus)
            continue;
        RaftBusDevicesIF* pDevicesIF = pBus->getBusDevicesIF();
        if (!pDevicesIF)
            continue;

        // Add the bus data
        uint32_t busRemaining = 0;
        std::vector<uint8_t> busBinaryData = pDevicesIF->getQueuedDeviceDataBinary(connModeBusNum, maxResponsesToReturn, &busRemaining);
        remainingTotal += busRemaining;
        binaryData.insert(binaryData.end(), busBinaryData.begin(), busBinaryData.end());

        // Next bus
        connModeBusNum++;
    }

    // Get a frozen copy of the device list (null-pointers excluded)
    RaftDevice* pDeviceListCopy[DEVICE_LIST_MAX_SIZE];
    uint32_t numDevices = getDeviceListFrozen(pDeviceListCopy, DEVICE_LIST_MAX_SIZE);

    // Loop through the devices
    for (uint32_t devIdx = 0; devIdx < numDevices; devIdx++)
    {
        RaftDevice* pDevice = pDeviceListCopy[devIdx];
        std::vector<uint8_t> deviceBinaryData = pDevice->getStatusBinary();
        binaryData.insert(binaryData.end(), deviceBinaryData.begin(), deviceBinaryData.end());

#ifdef DEBUG_BINARY_DEVICE_DATA
        LOG_I(MODULE_PREFIX, "getDevicesDataBinary DEV %s hex %s", 
                pDevice->getDeviceName().c_str(), Raft::getHexStr(deviceBinaryData.data(), deviceBinaryData.size()).c_str());
#endif        
    }

    if (pRemaining)
        *pRemaining = remainingTotal;

    return binaryData;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Check for change of devices' data
/// @param stateHash hash of the currently available data
void DeviceManager::getDevicesHash(std::vector<uint8_t>& stateHash) const
{
    // Clear hash to two bytes
    stateHash.clear();
    stateHash.push_back(0);
    stateHash.push_back(0);

    // Check all buses for data
    for (RaftBus* pBus : raftBusSystem.getBusList())
    {
        // Check bus
        if (pBus)
        {
            // Check bus status
            uint32_t identPollLastMs = pBus->getDeviceInfoTimestampMs(true, true);
            stateHash[0] ^= (identPollLastMs & 0xff);
            stateHash[1] ^= ((identPollLastMs >> 8) & 0xff);

#ifdef DEBUG_JSON_DEVICE_HASH_DETAIL
            LOG_I(MODULE_PREFIX, "getDevicesHash %s ms %d %02x%02x", 
                    pBus->getBusName().c_str(), (int)identPollLastMs, stateHash[0], stateHash[1]);
#endif
        }
    }

    // Get a frozen copy of the device list (null-pointers excluded)
    RaftDevice* pDeviceListCopy[DEVICE_LIST_MAX_SIZE];
    uint32_t numDevices = getDeviceListFrozen(pDeviceListCopy, DEVICE_LIST_MAX_SIZE);

    // Loop through the devices
    for (uint32_t devIdx = 0; devIdx < numDevices; devIdx++)
    {
        // Check device status
        RaftDevice* pDevice = pDeviceListCopy[devIdx];
        uint32_t identPollLastMs = pDevice->getDeviceInfoTimestampMs(true, true);
        stateHash[0] ^= (identPollLastMs & 0xff);
        stateHash[1] ^= ((identPollLastMs >> 8) & 0xff);

#ifdef DEBUG_JSON_DEVICE_HASH_DETAIL
        LOG_I(MODULE_PREFIX, "getDevicesHash %s ms %d %02x%02x", 
                pDevice->getDeviceName().c_str(), (int)identPollLastMs, stateHash[0], stateHash[1]);
#endif
    }

    // Debug
#ifdef DEBUG_JSON_DEVICE_HASH
    LOG_I(MODULE_PREFIX, "getDevicesHash => %02x%02x", stateHash[0], stateHash[1]);
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Pause/resume draining offline buffers based on link availability
/// @param paused true to pause draining while link is down
void DeviceManager::setOfflineDrainLinkPaused(bool paused)
{
    if (_offlineDrainLinkPaused == paused)
        return;
    _offlineDrainLinkPaused = paused;

    for (RaftBus* pBus : raftBusSystem.getBusList())
    {
        if (!pBus)
            continue;
        RaftBusDevicesIF* pDevicesIF = pBus->getBusDevicesIF();
        if (!pDevicesIF)
            continue;
        pDevicesIF->setOfflineDrainLinkPaused(paused);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get a device by name
/// @param pDeviceName Name of the device
/// @return RaftDevice* Pointer to the device or nullptr if not found
RaftDevice* DeviceManager::getDevice(const char* pDeviceName) const
{
    // Obtain access to the device list
    if (xSemaphoreTake(_accessMutex, pdMS_TO_TICKS(5)) != pdTRUE)
        return nullptr;

    // Loop through the devices
    for (auto* pDevice : _deviceList)
    {
#ifdef DEBUG_GET_DEVICE
        LOG_I(MODULE_PREFIX, "getDevice %s checking %s", pDeviceName, pDevice ? pDevice->getDeviceName() : "UNKNOWN");
#endif
        if (pDevice && pDevice->getDeviceName() == pDeviceName)
        {
            xSemaphoreGive(_accessMutex);
            return pDevice;
        }
    }

    // Return semaphore
    xSemaphoreGive(_accessMutex);
    return nullptr;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get JSON status string
/// @return JSON string
String DeviceManager::getDebugJSON() const
{
    // JSON strings
    String jsonStrBus, jsonStrDev;

    // Check all buses for data
    for (RaftBus* pBus : raftBusSystem.getBusList())
    {
        if (!pBus)
            continue;
        // Get device interface
        RaftBusDevicesIF* pDevicesIF = pBus->getBusDevicesIF();
        if (!pDevicesIF)
            continue;
        String jsonRespStr = pDevicesIF->getDebugJSON(true);

        // Check for empty string or empty JSON object
        if (jsonRespStr.length() > 2)
        {
            jsonStrBus += (jsonStrBus.length() == 0 ? "\"" : ",\"") + pBus->getBusName() + "\":" + jsonRespStr;
        }
    }

    // Get a frozen copy of the device list (null-pointers excluded)
    RaftDevice* pDeviceListCopy[DEVICE_LIST_MAX_SIZE];
    uint32_t numDevices = getDeviceListFrozen(pDeviceListCopy, DEVICE_LIST_MAX_SIZE);

    // Loop through the devices
    for (uint32_t devIdx = 0; devIdx < numDevices; devIdx++)
    {
        RaftDevice* pDevice = pDeviceListCopy[devIdx];
        String jsonRespStr = pDevice->getDebugJSON(true);

        // Check for empty string or empty JSON object
        if (jsonRespStr.length() > 2)
        {
            jsonStrDev += (jsonStrDev.length() == 0 ? "\"" : ",\"") + pDevice->getPublishDeviceType() + "\":" + jsonRespStr;
        }
    }

    return "{" + (jsonStrBus.length() == 0 ? (jsonStrDev.length() == 0 ? "" : jsonStrDev) : (jsonStrDev.length() == 0 ? jsonStrBus : jsonStrBus + "," + jsonStrDev)) + "}";
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Endpoints
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void DeviceManager::addRestAPIEndpoints(RestAPIEndpointManager &endpointManager)
{
    // REST API endpoints
    endpointManager.addEndpoint("devman", RestAPIEndpoint::ENDPOINT_CALLBACK, RestAPIEndpoint::ENDPOINT_GET,
                            std::bind(&DeviceManager::apiDevMan, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3),
                            " devman/typeinfo?bus=<busName>&type=<typeName> - Get type info,"
                            " devman/cmdraw?bus=<busName>&addr=<addr>&hexWr=<hexWriteData>&numToRd=<numBytesToRead>&msgKey=<msgKey> - Send raw command to device,"
                            " devman/demo?type=<deviceType>&rate=<sampleRateMs>&duration=<durationMs>&offlineIntvS=<N>&offlineDurS=<M> - Start demo device");
    LOG_I(MODULE_PREFIX, "addRestAPIEndpoints added devman");
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// REST API DevMan
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RaftRetCode DeviceManager::apiDevMan(const String &reqStr, String &respStr, const APISourceInfo& sourceInfo)
{
    // Get device info
    std::vector<String> params;
    std::vector<RaftJson::NameValuePair> nameValues;
    RestAPIEndpointManager::getParamsAndNameValues(reqStr.c_str(), params, nameValues);
    RaftJson jsonParams = RaftJson::getJSONFromNVPairs(nameValues, true); 

    // Get command
    String cmdName = reqStr;
    if (params.size() > 1)
        cmdName = params[1];

    // Check command
    if (cmdName.equalsIgnoreCase("typeinfo"))
    {
        // Get bus name
        String busName = jsonParams.getString("bus", "");
        if (busName.length() == 0)
            return Raft::setJsonErrorResult(reqStr.c_str(), respStr, "failBusMissing");

        // Get device name
        String devTypeName = jsonParams.getString("type", "");
        if (devTypeName.length() == 0)
            return Raft::setJsonErrorResult(reqStr.c_str(), respStr, "failTypeMissing");

        // Check if the bus name is valid and, if so, use the bus devices interface to get the device info
        String devInfo;
        RaftBus* pBus = raftBusSystem.getBusByName(busName);
        if (!pBus)
        {
            // Try to get by bus number if the busName start with a number
            if ((busName.length() > 0) && isdigit(busName[0]))
            {
                int busNum = busName.toInt();
                int busIdx = 1;
                for (auto& bus : raftBusSystem.getBusList())
                {
                    if (busIdx++ == busNum)
                    {
                        pBus = bus;
                        break;
                    }
                }
            }
        }
        if (pBus)
        {
            // Get devices interface
            RaftBusDevicesIF* pDevicesIF = pBus->getBusDevicesIF();
            if (!pDevicesIF)
                return Raft::setJsonErrorResult(reqStr.c_str(), respStr, "failTypeNotFound");

            // Check if the first digit of the device type name is a number
            if ((devTypeName.length() > 0) && isdigit(devTypeName[0]))
            {
                // Get device info by number
                devInfo = pDevicesIF->getDevTypeInfoJsonByTypeIdx(devTypeName.toInt(), false);
            }
            if (devInfo.length() == 0)
            {
                // Get device info by name if possible
                devInfo = pDevicesIF->getDevTypeInfoJsonByTypeName(devTypeName, false);
            }
        }
        else
        {
            // Use the global device type info to get the device info
            if ((devTypeName.length() > 0) && isdigit(devTypeName[0]))
            {
                // Get device info by number
                devInfo = deviceTypeRecords.getDevTypeInfoJsonByTypeIdx(devTypeName.toInt(), false);
            }
            if (devInfo.length() == 0)
            {
                // Get device info by name if possible
                devInfo = deviceTypeRecords.getDevTypeInfoJsonByTypeName(devTypeName, false);
            }
        }

        // Check valid
        if ((devInfo.length() == 0) || (devInfo == "{}"))
        {
#ifdef DEBUG_DEVMAN_API
            LOG_I(MODULE_PREFIX, "apiHWDevice bus %s type %s DEVICE NOT FOUND", busName.c_str(), devTypeName.c_str());
#endif
            return Raft::setJsonErrorResult(reqStr.c_str(), respStr, "failTypeNotFound");
        }

#ifdef DEBUG_DEVMAN_API
        LOG_I(MODULE_PREFIX, "apiHWDevice bus %s busFound %s type %s devInfo %s", 
                busName.c_str(), 
                pBus ? "Y" : "N",
                devTypeName.c_str(), 
                devInfo.c_str());
#endif

        // Set result
        return Raft::setJsonBoolResult(reqStr.c_str(), respStr, true, ("\"devinfo\":" + devInfo).c_str());
    }

    // Check for offline buffer stats and control
    if (cmdName.equalsIgnoreCase("offlinebuf"))
    {
        auto parseAddrList = [](const String& addrCsv, std::vector<BusElemAddrType>& outAddrs)
        {
            int startPos = 0;
            while (startPos < addrCsv.length())
            {
                int commaPos = addrCsv.indexOf(',', startPos);
                String token = (commaPos == -1) ? addrCsv.substring(startPos) : addrCsv.substring(startPos, commaPos);
                token.trim();
                if (token.length() > 0)
                    outAddrs.push_back(strtoul(token.c_str(), nullptr, 0));
                if (commaPos == -1)
                    break;
                startPos = commaPos + 1;
            }
        };
        auto parseTypeList = [](const String& typeCsv, std::vector<std::string>& outTypes)
        {
            int startPos = 0;
            while (startPos < typeCsv.length())
            {
                int commaPos = typeCsv.indexOf(',', startPos);
                String token = (commaPos == -1) ? typeCsv.substring(startPos) : typeCsv.substring(startPos, commaPos);
                token.trim();
                if (token.length() > 0)
                    outTypes.push_back(std::string(token.c_str()));
                if (commaPos == -1)
                    break;
                startPos = commaPos + 1;
            }
        };
        auto addrSetToJson = [](const std::set<BusElemAddrType>& addrs) -> String
        {
            String out = "[";
            bool first = true;
            for (auto addr : addrs)
            {
                out += first ? "" : ",";
                out += "\"0x" + String(addr, 16) + "\"";
                first = false;
            }
            out += "]";
            return out;
        };
        auto strSetToJson = [](const std::set<std::string>& values) -> String
        {
            String out = "[";
            bool first = true;
            for (const auto& val : values)
            {
                out += first ? "" : ",";
                out += "\"" + String(val.c_str()) + "\"";
                first = false;
            }
            out += "]";
            return out;
        };
        auto rateOverridesToJson = [](const std::map<BusElemAddrType, uint32_t>& overridesUs) -> String
        {
            if (overridesUs.empty())
                return "";
            String out = "{";
            bool first = true;
            for (const auto& kv : overridesUs)
            {
                out += first ? "" : ",";
                out += "\"0x" + String(kv.first, 16) + "\":" + String(kv.second / 1000);
                first = false;
            }
            out += "}";
            return out;
        };
        auto toLower = [](const std::string& inStr)
        {
            std::string out(inStr);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return std::tolower(c); });
            return out;
        };

        String busName = jsonParams.getString("bus", "");
        String action = jsonParams.getString("action", "status");
        String addrCsv = jsonParams.getString("addr", "");
        String typeCsv = jsonParams.getString("type", "");
        // Tolerate URL-encoded commas in CSV params (e.g. "VL53L4CD%2CLSM6DS")
        addrCsv.replace("%2C", ",");
        addrCsv.replace("%2c", ",");
        typeCsv.replace("%2C", ",");
        typeCsv.replace("%2c", ",");
        int32_t rateOverrideMsIn = jsonParams.getLong("rateMs", 0);
        uint32_t rateOverrideMs = rateOverrideMsIn > 0 ? (uint32_t)rateOverrideMsIn : 0;
        int32_t startParam = jsonParams.getLong("start", 0);
        uint32_t startIdx = startParam > 0 ? (uint32_t)startParam : 0;
        int32_t countParam = jsonParams.getLong("count", 0);
        uint32_t maxResponses = countParam > 0 ? (uint32_t)countParam : 0;
        int32_t maxBytesParam = jsonParams.getLong("maxBytes", 0);
        uint32_t maxBytes = maxBytesParam > 0 ? (uint32_t)maxBytesParam : 0;
        bool clearOnStop = jsonParams.getBool("clear", false);
        bool nonDestructiveFetch = jsonParams.getBool("nonDestructive", true);
        bool simulateOnly = jsonParams.getBool("simulate", false);

        bool doStart = action.equalsIgnoreCase("start") || action.equalsIgnoreCase("resume");
        bool doStop = action.equalsIgnoreCase("stop") || action.equalsIgnoreCase("pause");
        bool doReset = action.equalsIgnoreCase("reset") || action.equalsIgnoreCase("clear");
        bool doPeek = action.equalsIgnoreCase("peek") || (action.equalsIgnoreCase("fetch") && nonDestructiveFetch);

        std::vector<BusElemAddrType> requestedAddrs;
        parseAddrList(addrCsv, requestedAddrs);
        std::vector<std::string> requestedTypes;
        parseTypeList(typeCsv, requestedTypes);
        std::set<std::string> requestedTypesLower;
        for (const auto& typeName : requestedTypes)
            requestedTypesLower.insert(toLower(typeName));

        LOG_I(MODULE_PREFIX, "offlinebuf action %s bus %s addrCsv %s typeCsv %s rateMs %u start %u count %u maxBytes %u",
            action.c_str(), busName.c_str(), addrCsv.c_str(), typeCsv.c_str(),
            (unsigned)rateOverrideMs, (unsigned)startIdx, (unsigned)maxResponses, (unsigned)maxBytes);

        bool busMatched = false;
        String statsJson;
        String controlJson;
        String peekJson;
        uint32_t peekRemainingTotal = 0;
        uint64_t offlineBytesTotal = 0;
        String estimateJson;

        for (RaftBus* pBus : raftBusSystem.getBusList())
        {
            if (!pBus || ((busName.length() > 0) && !pBus->getBusName().equalsIgnoreCase(busName)))
                continue;
            busMatched = true;
            RaftBusDevicesIF* pDevicesIF = pBus->getBusDevicesIF();
            if (!pDevicesIF)
                continue;

            std::vector<BusElemAddrType> allAddrs;
            pDevicesIF->getDeviceAddresses(allAddrs, false);
            if (allAddrs.empty())
                continue;

            std::set<BusElemAddrType> requestedAddrSet(requestedAddrs.begin(), requestedAddrs.end());
            std::vector<BusElemAddrType> targetAddrs;
            for (auto addr : allAddrs)
            {
                bool addrMatch = !requestedAddrSet.empty() && (requestedAddrSet.count(addr) > 0);
                bool typeMatch = false;
                if (!requestedTypesLower.empty())
                {
                    std::string typeName;
                    if (pDevicesIF->getDeviceTypeName(addr, typeName))
                        typeMatch = requestedTypesLower.count(toLower(typeName)) > 0;
                }
                if (requestedAddrSet.empty() && requestedTypesLower.empty())
                {
                    targetAddrs.push_back(addr);
                }
                else if (addrMatch || typeMatch)
                {
                    targetAddrs.push_back(addr);
                }
            }
            if (targetAddrs.empty())
            {
                // If a specific addr/type was requested but nothing matched, skip this bus
                if (!requestedAddrSet.empty() || !requestedTypesLower.empty())
                    continue;
                // Otherwise, default to all addresses on the bus
                targetAddrs = allAddrs;
            }

            if (simulateOnly)
            {
                std::map<BusElemAddrType, EstAllocInfo> estBytes;
                if (pDevicesIF->estimateOfflineAllocations(targetAddrs, estBytes) && !estBytes.empty())
                {
                    String busEstimate;
                    for (const auto& kv : estBytes)
                    {
                        if (busEstimate.length() > 0)
                            busEstimate += ",";
                        busEstimate += "\"0x";
                        busEstimate += String(kv.first, 16);
                        busEstimate += "\":{";
                        busEstimate += "\"bytes\":";
                        busEstimate += String(kv.second.allocBytes);
                        busEstimate += ",\"bpe\":";
                        busEstimate += String(kv.second.bytesPerEntry);
                        busEstimate += ",\"payload\":";
                        busEstimate += String(kv.second.payloadSize);
                        busEstimate += ",\"meta\":";
                        busEstimate += String(kv.second.metaSize);
                        busEstimate += "}";
                    }
                    if (busEstimate.length() > 0)
                    {
                        if (estimateJson.length() > 0)
                            estimateJson += ",";
                        estimateJson += "\"";
                        estimateJson += pBus->getBusName();
                        estimateJson += "\":{";
                        estimateJson += busEstimate;
                        estimateJson += "}";
                    }
                }
            }
            else if (doStart)
            {
                pDevicesIF->setOfflineBufferPaused(std::vector<BusElemAddrType>(), false);
                if (targetAddrs.size() < allAddrs.size())
                {
                    std::vector<BusElemAddrType> pauseAddrs;
                    for (auto addr : allAddrs)
                    {
                        if (std::find(targetAddrs.begin(), targetAddrs.end(), addr) == targetAddrs.end())
                            pauseAddrs.push_back(addr);
                    }
                    if (!pauseAddrs.empty())
                        pDevicesIF->setOfflineBufferPaused(pauseAddrs, true);
                }
                // Keep draining paused by default to avoid consuming buffered backlog automatically
                pDevicesIF->setOfflineDrainPaused(std::vector<BusElemAddrType>(), true);
                if (rateOverrideMs > 0)
                    pDevicesIF->applyOfflineRateOverride(targetAddrs, rateOverrideMs);
                // Explicitly ensure target addresses are unpaused for buffering after rate override
                if (!targetAddrs.empty())
                    pDevicesIF->setOfflineBufferPaused(targetAddrs, false);
                // Record selection for visibility
                pDevicesIF->setOfflineDrainSelection(targetAddrs, requestedTypes, false);
                // Rebalance buffers across current selection
                pDevicesIF->rebalanceOfflineBuffers(targetAddrs);
                pDevicesIF->setOfflineAutoResume(true, targetAddrs, rateOverrideMs);
            }
            if (doStop)
            {
                if (targetAddrs.size() == allAddrs.size())
                    pDevicesIF->setOfflineBufferPaused(std::vector<BusElemAddrType>(), true);
                else
                    pDevicesIF->setOfflineBufferPaused(targetAddrs, true);
                pDevicesIF->setOfflineDrainPaused(std::vector<BusElemAddrType>(), true);
                pDevicesIF->clearOfflineRateOverride(targetAddrs);
                pDevicesIF->setOfflineDrainSelection(std::vector<BusElemAddrType>(), std::vector<std::string>(), false);
                pDevicesIF->setOfflineAutoResume(false, std::vector<BusElemAddrType>(), 0);
                if (clearOnStop)
                    pDevicesIF->resetOfflineBuffers(targetAddrs);
            }
            if (doReset && !doStop)
            {
                pDevicesIF->resetOfflineBuffers(targetAddrs);
                pDevicesIF->setOfflineAutoResume(false, std::vector<BusElemAddrType>(), 0);
            }

            // Snapshot control state for stats/response
            std::set<BusElemAddrType> bufferPaused;
            std::set<BusElemAddrType> drainPaused;
            std::set<BusElemAddrType> drainSelectedAddrs;
            std::set<std::string> drainSelectedTypes;
            bool drainOnly = false;
            uint32_t perBusOverride = 0;
            bool globalBufPaused = false;
            bool globalDrainPaused = false;
            std::map<BusElemAddrType, uint32_t> rateOverridesUs;
            pDevicesIF->getOfflineControlSnapshot(bufferPaused, drainPaused, drainSelectedAddrs, drainSelectedTypes,
                        drainOnly, perBusOverride, globalBufPaused, globalDrainPaused, rateOverridesUs);

            // Stats per address
            String busJson;
            for (auto address : targetAddrs)
            {
                OfflineDataStats stats = pDevicesIF->getOfflineStats(address);
                if (stats.maxEntries == 0)
                    continue;
                offlineBytesTotal += (uint64_t)stats.maxEntries * (uint64_t)(stats.payloadSize + stats.metaSize);
                bool bufPaused = globalBufPaused || (bufferPaused.count(address) > 0);
                bool drPaused = globalDrainPaused || (drainPaused.count(address) > 0);
                bool selectedByAddr = drainSelectedAddrs.count(address) > 0;
                bool selectedByType = false;
                if (pDevicesIF && !drainSelectedTypes.empty())
                {
                    std::string typeName;
                    if (pDevicesIF->getDeviceTypeName(address, typeName))
                        selectedByType = drainSelectedTypes.count(typeName) > 0;
                }
                if (drainOnly && !(selectedByAddr || selectedByType))
                    drPaused = true;
                String addrJson;
                addrJson.reserve(96);
                addrJson += "\"depth\":"; addrJson += String(stats.depth);
                addrJson += ",\"drops\":"; addrJson += String(stats.drops);
                addrJson += ",\"max\":"; addrJson += String(stats.maxEntries);
                addrJson += ",\"bytes\":"; addrJson += String(stats.bytesInUse());
                addrJson += ",\"wraps\":"; addrJson += String(stats.tsWrapCount);
                addrJson += ",\"oldestMs\":"; addrJson += String((uint32_t)stats.oldestCaptureMs);
                addrJson += ",\"bufPaused\":"; addrJson += String(bufPaused ? 1 : 0);
                addrJson += ",\"drainPaused\":"; addrJson += String(drPaused ? 1 : 0);
                addrJson += ",\"payload\":"; addrJson += String(stats.payloadSize);
                addrJson += ",\"meta\":"; addrJson += String(stats.metaSize);

                if (busJson.length() > 0)
                    busJson += ",";
                busJson += "\"0x";
                busJson += String(address, 16);
                busJson += "\":{";
                busJson += addrJson;
                busJson += "}";
            }
            if (statsJson.length() > 0)
                statsJson += ",";
            statsJson += "\"";
            statsJson += pBus->getBusName();
            statsJson += "\":{";
            statsJson += busJson;
            statsJson += "}";

            String busCtrl;
            busCtrl.reserve(160);
            busCtrl += "\"bufferPausedGlobal\":"; busCtrl += String(globalBufPaused ? 1 : 0);
            busCtrl += ",\"drainPausedGlobal\":"; busCtrl += String(globalDrainPaused ? 1 : 0);
            busCtrl += ",\"bufferPaused\":"; busCtrl += addrSetToJson(bufferPaused);
            busCtrl += ",\"drainPaused\":"; busCtrl += addrSetToJson(drainPaused);
            busCtrl += ",\"selectedAddrs\":"; busCtrl += addrSetToJson(drainSelectedAddrs);
            busCtrl += ",\"selectedTypes\":"; busCtrl += strSetToJson(drainSelectedTypes);
            if (perBusOverride > 0)
            {
                busCtrl += ",\"maxPerPublishOverride\":";
                busCtrl += String(perBusOverride);
            }
            String rateOverridesJson = rateOverridesToJson(rateOverridesUs);
            if (rateOverridesJson.length() > 0)
            {
                busCtrl += ",\"rateOverrides\":"; busCtrl += rateOverridesJson;
            }
            if (controlJson.length() > 0)
                controlJson += ",";
            controlJson += "\"";
            controlJson += pBus->getBusName();
            controlJson += "\":{";
            controlJson += busCtrl;
            controlJson += "}";

            if (doPeek)
            {
                uint32_t busRemaining = 0;
                String busPeek = pDevicesIF->peekOfflineDataJson(targetAddrs, startIdx, maxResponses, maxBytes, busRemaining);
                if (busPeek.length() > 2)
                {
                    if (peekJson.length() > 0)
                        peekJson += ",";
                    peekJson += "\"";
                    peekJson += pBus->getBusName();
                    peekJson += "\":";
                    peekJson += busPeek;
                }
                peekRemainingTotal += busRemaining;
            }
        }
        if ((busName.length() > 0) && !busMatched)
            return Raft::setJsonErrorResult(reqStr.c_str(), respStr, "failBusNotFound");
        String statsWrapper = "{" + statsJson + "}";
        String extra = "\"stats\":" + statsWrapper;
        if (controlJson.length() > 0)
            extra += ",\"control\":{" + controlJson + "}";
        if (peekJson.length() > 0)
            extra += ",\"peek\":{" + peekJson + "}";
        if (estimateJson.length() > 0)
            extra += ",\"estimate\":{" + estimateJson + "}";
        if (peekRemainingTotal > 0)
            extra += ",\"peekRemaining\":" + String(peekRemainingTotal);
        uint32_t freePsram = 0;
        uint32_t freeInternal = 0;
#ifdef ESP_PLATFORM
        freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        freeInternal = heap_caps_get_free_size(MALLOC_CAP_8BIT);
#endif
        String memJson = "\"mem\":{";
        memJson += "\"offlineBytesInUse\":" + String((uint32_t)offlineBytesTotal);
#ifdef ESP_PLATFORM
        memJson += ",\"freePsram\":" + String(freePsram);
        memJson += ",\"freeInternal\":" + String(freeInternal);
#endif
        memJson += "}";
        extra += "," + memJson;
        return Raft::setJsonBoolResult(reqStr.c_str(), respStr, true, extra.c_str());
    }

    // Check for raw command
    if (cmdName.equalsIgnoreCase("cmdraw"))
    {
        // Get bus name
        String busName = jsonParams.getString("bus", "");
        if (busName.length() == 0)
            return Raft::setJsonErrorResult(reqStr.c_str(), respStr, "failBusMissing");

        // Get args
        String addrStr = jsonParams.getString("addr", "");
        String hexWriteData = jsonParams.getString("hexWr", "");
        int numBytesToRead = jsonParams.getLong("numToRd", 0);
        // String msgKey = jsonParams.getString("msgKey", "");

        // Check valid
        if (addrStr.length() == 0)
            return Raft::setJsonErrorResult(reqStr.c_str(), respStr, "failMissingAddr");

        // Find the bus
        RaftBus* pBus = raftBusSystem.getBusByName(busName);
        if (!pBus)
            return Raft::setJsonErrorResult(reqStr.c_str(), respStr, "failBusNotFound");

        // Convert address
        BusElemAddrType addr = strtol(addrStr.c_str(), NULL, 16);

        // Get bytes to write
        uint32_t numBytesToWrite = hexWriteData.length() / 2;
        std::vector<uint8_t> writeVec;
        writeVec.resize(numBytesToWrite);
        uint32_t writeBytesLen = Raft::getBytesFromHexStr(hexWriteData.c_str(), writeVec.data(), numBytesToWrite);
        writeVec.resize(writeBytesLen);

        // Store the msg key for response
        // TODO store the msgKey for responses
        // _cmdResponseMsgKey = msgKey;

        // Form HWElemReq
        static const uint32_t CMDID_CMDRAW = 100;
        HWElemReq hwElemReq = {writeVec, numBytesToRead, CMDID_CMDRAW, "cmdraw", 0};

        // Form request
        BusRequestInfo busReqInfo("", addr);
        busReqInfo.set(BUS_REQ_TYPE_STD, hwElemReq, 0, 
                [](void* pCallbackData, BusRequestResult& reqResult)
                    {
                        if (pCallbackData)
                            ((DeviceManager*)pCallbackData)->cmdResultReportCallback(reqResult);
                    }, 
                this);

#ifdef DEBUG_MAKE_BUS_REQUEST_VERBOSE
        String outStr;
        Raft::getHexStrFromBytes(hwElemReq._writeData.data(), 
                    hwElemReq._writeData.size() > 16 ? 16 : hwElemReq._writeData.size(),
                    outStr);
        LOG_I(MODULE_PREFIX, "apiHWDevice addr %s len %d data %s ...", 
                        addrStr.c_str(), 
                        hwElemReq._writeData.size(),
                        outStr.c_str());
#endif

        bool rslt = pBus->addRequest(busReqInfo);
        if (!rslt)
        {
            LOG_W(MODULE_PREFIX, "apiHWDevice failed send raw command");
        }

        // Debug
#ifdef DEBUG_API_CMDRAW
        LOG_I(MODULE_PREFIX, "apiHWDevice hexWriteData %s numToRead %d", hexWriteData.c_str(), numBytesToRead);
#endif

        // Set result
        return Raft::setJsonBoolResult(reqStr.c_str(), respStr, rslt);    
    }

#ifdef DEVICE_MANAGER_ENABLE_DEMO_DEVICE

    // Check for demo command
    if (cmdName.equalsIgnoreCase("demo"))
    {
        // Get demo parameters
        String deviceType = jsonParams.getString("type", "");
        if (deviceType.length() == 0)
            deviceType = "ACCDEMO";

        uint32_t sampleRateMs = jsonParams.getLong("rate", 100);
        uint32_t durationMs = jsonParams.getLong("duration", 0);
        uint32_t offlineIntvS = jsonParams.getLong("offlineIntvS", 0);
        uint32_t offlineDurS = jsonParams.getLong("offlineDurS", 10);

        // Validate parameters
        if (sampleRateMs < 10)
            sampleRateMs = 10; // Minimum 10ms
        if (sampleRateMs > 60000)
            sampleRateMs = 60000; // Maximum 60s
        if (offlineDurS < 1)
            offlineDurS = 1; // Minimum 1s offline duration

        // Check if the device already exists
        RaftDevice* pExistingDevice = getDeviceByID(deviceType.c_str());
        if (pExistingDevice)
        {
            // Return error if the device already exists
            return Raft::setJsonErrorResult(reqStr.c_str(), respStr, "failDemoDeviceExists");
        }

        // Setup the demo device
        RaftJson jsonConfig = "{\"name\":\"DemoDevice\",\"type\":\"" + deviceType + 
                                 "\",\"sampleRateMs\":" + String(sampleRateMs) + 
                                 ",\"durationMs\":" + String(durationMs) +
                                 ",\"offlineIntvS\":" + String(offlineIntvS) +
                                 ",\"offlineDurS\":" + String(offlineDurS) + "}";
        setupDevice(deviceType.c_str(), jsonConfig);

        // Set result
        String resultStr = "\"demoStarted\":true,\"type\":\"" + deviceType + 
                          "\",\"rate\":" + String(sampleRateMs) + 
                          ",\"duration\":" + String(durationMs) +
                          ",\"offlineIntvS\":" + String(offlineIntvS) +
                          ",\"offlineDurS\":" + String(offlineDurS);
        return Raft::setJsonBoolResult(reqStr.c_str(), respStr, true, resultStr.c_str());
    }
#endif

    // Set result
    return Raft::setJsonErrorResult(reqStr.c_str(), respStr, "failUnknownCmd");
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Cmd result report callbacks
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void DeviceManager::cmdResultReportCallback(BusRequestResult &reqResult)
{
#ifdef DEBUG_CMD_RESULT
    LOG_I(MODULE_PREFIX, "cmdResultReportCallback len %d", reqResult.getReadDataLen());
    Raft::logHexBuf(reqResult.getReadData(), reqResult.getReadDataLen(), MODULE_PREFIX, "cmdResultReportCallback");
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Register for device data notifications (note that callbacks may occur on different threads)
/// @param pDeviceName Name of the device
/// @param dataChangeCB Callback for data change
/// @param minTimeBetweenReportsMs Minimum time between reports (ms)
/// @param pCallbackInfo Callback info (passed to the callback)
void DeviceManager::registerForDeviceData(const char* pDeviceName, RaftDeviceDataChangeCB dataChangeCB, 
        uint32_t minTimeBetweenReportsMs, const void* pCallbackInfo)
{
    // Add to requests for device data changes
    _deviceDataChangeCBList.push_back(DeviceDataChangeRec(pDeviceName, dataChangeCB, minTimeBetweenReportsMs, pCallbackInfo));

    // Debug
    bool found = false;
    for (auto& rec : _deviceDataChangeCBList)
    {
        if (rec.deviceName == pDeviceName)
        {
            found = true;
            break;
        }
    }
    LOG_I(MODULE_PREFIX, "registerForDeviceData %s %s minTime %dms", 
        pDeviceName, found ? "OK" : "DEVICE_NOT_PRESENT", minTimeBetweenReportsMs);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Register for device status changes
/// @param statusChangeCB Callback for status change
void DeviceManager::registerForDeviceStatusChange(RaftDeviceStatusChangeCB statusChangeCB)
{
    // Add to requests for device status changes
    _deviceStatusChangeCBList.push_back(statusChangeCB);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get a frozen version of device list
/// @param pDeviceList (out) list of devices
/// @param maxNumDevices maximum number of devices to return
/// @return number of devices
uint32_t DeviceManager::getDeviceListFrozen(RaftDevice** pDevices, uint32_t maxDevices) const
{
    if (xSemaphoreTake(_accessMutex, pdMS_TO_TICKS(5)) != pdTRUE)
        return 0;
    uint32_t numDevices = 0;
    for (auto* pDevice : _deviceList)
    {
        if (numDevices >= maxDevices)
            break;
        if (pDevice)
            pDevices[numDevices++] = pDevice;
    }
    xSemaphoreGive(_accessMutex);
    return numDevices;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Find device in device list
/// @param pDeviceID ID of the device
/// @return pointer to device if found
RaftDevice* DeviceManager::getDeviceByID(const char* pDeviceID) const
{
    if (xSemaphoreTake(_accessMutex, pdMS_TO_TICKS(5)) != pdTRUE)
        return nullptr;
    for (auto* pDevice : _deviceList)
    {
        if (pDevice && (pDevice->idMatches(pDeviceID)))
        {
            xSemaphoreGive(_accessMutex);
            return pDevice;
        }
    }
    xSemaphoreGive(_accessMutex);
    return nullptr;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief call device status change callbacks
/// @param pDevice pointer to the device
/// @param el bus element address and status
/// @param newlyCreated true if the device was newly created
void DeviceManager::callDeviceStatusChangeCBs(RaftDevice* pDevice, const BusElemAddrAndStatus& el, bool newlyCreated)
{
    // Obtain a lock & make a copy of the device status change callbacks
    if (xSemaphoreTake(_accessMutex, pdMS_TO_TICKS(5)) != pdTRUE)
        return;
    std::vector<RaftDeviceStatusChangeCB> statusChangeCallbacks(_deviceStatusChangeCBList.begin(), _deviceStatusChangeCBList.end());
    xSemaphoreGive(_accessMutex);

    // Call the device status change callbacks
    for (RaftDeviceStatusChangeCB statusChangeCB : statusChangeCallbacks)
    {
        statusChangeCB(*pDevice, el.isChangeToOnline || newlyCreated, newlyCreated);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Register for device data change callbacks
/// @param pDeviceName Name of the device (nullptr for all devices)
/// @return number of devices registered for data change callbacks
uint32_t DeviceManager::registerForDeviceDataChangeCBs(const char* pDeviceName)
{
    // Get semaphore
    if (xSemaphoreTake(_accessMutex, pdMS_TO_TICKS(5)) != pdTRUE)
        return 0;

    // Create a vector of devices for the device data change callbacks
    std::vector<DeviceDataChangeRecTmp> deviceListForDataChangeCB;
    for (auto& rec : _deviceDataChangeCBList)
    {
        // Check if the device name matches (if specified)
        if (pDeviceName && (rec.deviceName != pDeviceName))
            continue;
        // Find device
        RaftDevice* pDevice = nullptr;
        for (auto* pTestDevice : _deviceList)
        {
            if (!pTestDevice)
                continue;
            if (rec.deviceName == pTestDevice->getDeviceName())
            {
                pDevice = pTestDevice;
                break;
            }
        }
        if (!pDevice)
            continue;
        deviceListForDataChangeCB.push_back({pDevice, rec.dataChangeCB, rec.minTimeBetweenReportsMs, rec.pCallbackInfo});
    }
    xSemaphoreGive(_accessMutex);

    // Check for any device data change callbacks
    for (auto& cbRec : deviceListForDataChangeCB)
    {
        // Register for device data notification from the device
        cbRec.pDevice->registerForDeviceData(
            cbRec.dataChangeCB,
            cbRec.minTimeBetweenReportsMs,
            cbRec.pCallbackInfo
        );
    }
    return deviceListForDataChangeCB.size();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Device event callback
/// @param device Device
/// @param eventName Name of the event
/// @param eventData Data associated with the event
void DeviceManager::deviceEventCB(RaftDevice& device, const char* eventName, const char* eventData)
{
    // Get sys manager
    SysManager* pSysMan = getSysManager();
    if (!pSysMan)
        return;
    String cmdStr = "{\"msgType\":\"sysevent\",\"msgName\":\"" + String(eventName) + "\"";
    if (eventData)
        cmdStr += eventData;
    cmdStr += "}";
    pSysMan->sendCmdJSON(
        "SysMan",
        cmdStr.c_str()
    );
}
