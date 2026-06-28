#include "hw/hvac/irac_encoder.h"
#include "hw/ir_service.h"
#include <IRutils.h>

namespace {
stdAc::opmode_t strToMode(const String& s) {
    if (s == "Heat") return stdAc::opmode_t::kHeat;
    if (s == "Dry")  return stdAc::opmode_t::kDry;
    if (s == "Fan")  return stdAc::opmode_t::kFan;
    if (s == "Auto") return stdAc::opmode_t::kAuto;
    return stdAc::opmode_t::kCool;
}

stdAc::fanspeed_t strToFan(const String& s) {
    if (s == "Low")     return stdAc::fanspeed_t::kLow;
    if (s == "Medium")  return stdAc::fanspeed_t::kMedium;
    if (s == "High")    return stdAc::fanspeed_t::kHigh;
    if (s == "Highest" || s == "Max") return stdAc::fanspeed_t::kMax;
    return stdAc::fanspeed_t::kAuto;
}

stdAc::swingv_t strToSwing(const String& s) {
    if (s == "Auto")    return stdAc::swingv_t::kAuto;
    if (s == "Highest") return stdAc::swingv_t::kHighest;
    if (s == "Low")     return stdAc::swingv_t::kLow;
    return stdAc::swingv_t::kOff;
}
}

bool IracEncoder::send(const hvac::Command& cmd) {
    decode_type_t proto = strToDecodeType(cmd.vendor.c_str());
    if (proto == decode_type_t::UNKNOWN) {
        Serial.printf("[IRAC] unknown vendor: %s\n", cmd.vendor.c_str());
        return false;
    }

    stdAc::state_t st = {};
    st.protocol = proto;
    st.model    = 1;
    st.power    = cmd.power;
    st.mode     = strToMode(cmd.mode);
    st.degrees  = constrain(cmd.temp, minTemp(), maxTemp());
    st.celsius  = true;
    st.fanspeed = strToFan(cmd.fan);
    st.swingv   = strToSwing(cmd.swing);
    st.light    = cmd.light;
    st.turbo    = cmd.turbo;
    st.econo    = cmd.econo;
    st.sleep    = cmd.sleep ? 0 : -1;
    st.clean    = cmd.clean;

    bool ok = ir.sendAc(st);
    if (ok) {
        Serial.printf("[IRAC] %s Pwr:%s T:%d Fan:%s\n",
                      cmd.vendor.c_str(), cmd.power ? "ON" : "OFF", cmd.temp, cmd.fan.c_str());
    } else {
        Serial.printf("[IRAC] sendAc failed for %s\n", cmd.vendor.c_str());
    }
    return ok;
}
