#pragma once

#include <Arduino.h>

#include "core/pins.h"
#include "core/constants.h"

#include "config/config_store.h"

#include "hw/led_manager.h"
#include "hw/button_manager.h"
#include "hw/sensor_service.h"
#include "hw/ir_service.h"
#include "hw/hvac/hvac_registry.h"

#include "net/network_manager.h"
#include "net/mqtt_service.h"
#include "net/udp_mesh.h"

#include "web/web_server.h"
#include "ota/ota_manager.h"

#include "modes/device_mode.h"

struct HvacState {
    String vendor = "GREE";
    bool   power  = false;
    String mode   = "Cool";
    int    temp   = 26;
    String fan    = "Auto";
    bool   turbo  = false;
    bool   econo  = false;
    bool   sleep  = false;
    bool   light  = true;
    bool   clean  = false;

    void applyFrom(const hvac::Command& cmd);
    void persistTo(Config& cfg) const;
    void loadFrom(const Config& cfg);
};

struct SlaveInfo {
    char     mac[18]   = "";
    uint32_t ip        = 0;
    uint32_t lastSeen  = 0;
    char     name[33]  = "";
    char     icon[12]  = "ac";
    char     floor[33] = "";
};

class SlaveRegistry {
public:
    static const size_t CAPACITY = MAX_SLAVES;

    void load();
    void save();
    void clear();

    int  findByMac(const char* mac) const;
    int  findById(const String& id) const;
    int  findFree() const;

    void clearSlot(size_t idx);
    void pruneOffline();
    void refreshFromApStations();

    SlaveInfo&       at(size_t i)       { return slaves_[i]; }
    const SlaveInfo& at(size_t i) const { return slaves_[i]; }
    size_t size() const { return CAPACITY; }

private:
    SlaveInfo slaves_[MAX_SLAVES];
};

class PairingWindow {
public:
    bool     active() const { return active_; }
    uint32_t remainingMs() const;
    void     start();
    void     stop();
    void     loop();

private:
    bool     active_ = false;
    uint32_t until_  = 0;
};

class AppContext {
public:
    HvacState      hvacState;
    SlaveRegistry  slaves;
    PairingWindow  pairing;

    DeviceMode*    mode = nullptr;

    void           begin();
    void           loop();
    void           changeMode(DeviceModeId id);
    void           reboot(uint32_t delayMs = REBOOT_DEFAULT_DELAY_MS);

    DeviceModeId   decideBootMode();

    const String&  chipId()     const { return chipId_; }
    const String&  macAddress() const { return mac_; }
    const String&  slaveId()    const { return slaveId_; }

    bool           isApMaster() const { return modeId_ == DeviceModeId::ApMaster; }
    bool           isStaSlave() const { return modeId_ == DeviceModeId::StaSlave; }
    bool           isStaHome()  const { return modeId_ == DeviceModeId::StaHome; }
    DeviceModeId   modeId()     const { return modeId_; }
    const char*    modeName()   const;

private:
    DeviceModeId modeId_ = DeviceModeId::ApMaster;
    String chipId_;
    String mac_;
    String slaveId_;
};

extern AppContext ctx;

const char* modeString(DeviceModeId id);
const char* getApSsid();
const char* getApPass();
