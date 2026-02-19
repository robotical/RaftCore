/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Bus Address Status
//
// Rob Dobson 2024
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "BusAddrStatus.h"

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Handle device responding information
/// @param isResponding true if device is responding
/// @param flagSpuriousRecord (out) true if this is a spurious record
/// @param okMax max number of successful responses before declaring online
/// @param failMax max number of failed responses before declaring offline
/// @return true if status has changed
bool BusAddrStatus::handleResponding(bool isResponding, bool &flagSpuriousRecord, uint32_t okMax, uint32_t failMax)
{
    // Handle is responding or not
    if (isResponding)
    {
        // If not already online then count upwards
        if (!isOnline)
        {
            // Check if we've reached the threshold for online
            count = (count < okMax) ? count + 1 : count;
            if (count >= okMax)
            {
                // Now online
                isChange = !isChange;
                count = 0;
                isOnline = true;
                onlineState = DeviceOnlineState::ONLINE;
                wasOnceOnline = true;
                flagForDeletion = false;
                return true;
            }
        }
    }
    else
    {
        // Not responding - check for change to offline
        if (isOnline || !wasOnceOnline || flagForDeletion)
        {
            // Count down to offline/spurious threshold
            count = (count < -failMax) ? count : count - 1;
            if (count <= -failMax)
            {
                // Now offline/spurious
                count = 0;
                if (!wasOnceOnline || flagForDeletion)
                    flagSpuriousRecord = true;
                else
                    isChange = !isChange;
                isOnline = false;
                onlineState = DeviceOnlineState::OFFLINE;
                // flag for deletion next time around the loop
                // we can't do it at the same time we make the change to offline as we need the busStatusMgr in RaftI2C to log the change
                // and call the busElemStatusCB to clean up the device first, before the we delete the record in the busStatusManager's address list
                // TODO: this behaviour should be refactored to be less convoluted / more robust
                flagForDeletion = true;
                return true;
            }
        }
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief Get JSON for device status
/// @return JSON string
String BusAddrStatus::getJson() const
{
    // Create JSON
    char jsonStr[128];
    snprintf(jsonStr, sizeof(jsonStr), 
        "{\"a\":\"0x%04X\",\"s\":\"%c%c%c\"}", 
        (int)address, 
        isOnline ? 'O' : 'X', 
        wasOnceOnline ? 'W' : 'X', 
        isNewlyIdentified ? 'N' : 'X'
    );
    return jsonStr;
}
