#pragma once

#include <Arduino.h>

namespace hvac {

struct Command {
    String vendor;
    bool   power    = false;
    String mode;
    int    temp      = 26;
    String fan;
    String swing;
    bool   turbo    = false;
    bool   econo    = false;
    bool   sleep    = false;
    bool   light    = true;
    bool   clean    = false;
};

class IEncoder {
public:
    virtual ~IEncoder() = default;
    virtual const char* vendor() const = 0;
    virtual int  minTemp() const = 0;
    virtual int  maxTemp() const = 0;
    virtual bool send(const Command& cmd) = 0;
};

}
