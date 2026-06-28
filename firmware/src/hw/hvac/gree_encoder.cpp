#include "hw/hvac/gree_encoder.h"
#include "hw/ir_service.h"
#include <string.h>

namespace {
inline constexpr uint16_t GREE_HDR_MARK    = 9000;
inline constexpr uint16_t GREE_HDR_SPACE   = 4500;
inline constexpr uint16_t GREE_BIT_MARK    = 620;
inline constexpr uint16_t GREE_ONE_SPACE   = 1600;
inline constexpr uint16_t GREE_ZERO_SPACE  = 540;
inline constexpr uint32_t GREE_BLOCK_GAP   = 19980;
inline constexpr uint32_t GREE_FRAME_GAP   = 7300;

inline constexpr uint8_t GREE_MODE_AUTO   = 0x00;
inline constexpr uint8_t GREE_MODE_COOL   = 0x01;
inline constexpr uint8_t GREE_MODE_DRY    = 0x02;
inline constexpr uint8_t GREE_MODE_FAN    = 0x03;
inline constexpr uint8_t GREE_MODE_HEAT   = 0x04;
inline constexpr uint8_t GREE_POWER_BIT   = 0x08;

inline constexpr uint8_t GREE_FAN_AUTO    = 0x00;
inline constexpr uint8_t GREE_FAN_LOW     = 0x10;
inline constexpr uint8_t GREE_FAN_MED     = 0x20;
inline constexpr uint8_t GREE_FAN_HIGH    = 0x30;

inline constexpr uint8_t GREE_MSG_A       = 0x50;
inline constexpr uint8_t GREE_MSG_B       = 0x70;

uint8_t calcChecksum(const uint8_t data[8]) {
    uint8_t sum = 10;
    sum += data[0] & 0x0F;
    sum += data[1] & 0x0F;
    sum += data[2] & 0x0F;
    sum += data[3] & 0x0F;
    sum += data[4] >> 4;
    sum += data[5] >> 4;
    sum += data[6] >> 4;
    return (uint8_t)((sum & 0x0F) << 4);
}

void sendByte(IRsend& s, uint8_t data) {
    for (uint8_t mask = 1; mask; mask <<= 1) {
        s.mark(GREE_BIT_MARK);
        s.space((data & mask) ? GREE_ONE_SPACE : GREE_ZERO_SPACE);
    }
}

void sendFrame(IRsend& s, const uint8_t frame[8], uint32_t endSpace) {
    s.mark(GREE_HDR_MARK);
    s.space(GREE_HDR_SPACE);
    for (int i = 0; i < 4; i++) sendByte(s, frame[i]);

    s.mark(GREE_BIT_MARK); s.space(GREE_ZERO_SPACE);
    s.mark(GREE_BIT_MARK); s.space(GREE_ONE_SPACE);
    s.mark(GREE_BIT_MARK); s.space(GREE_ZERO_SPACE);

    s.mark(GREE_BIT_MARK);
    s.space(GREE_BLOCK_GAP);

    for (int i = 4; i < 8; i++) sendByte(s, frame[i]);

    s.mark(GREE_BIT_MARK);
    s.space(endSpace);
}
}

bool GreeEncoder::send(const hvac::Command& cmd) {
    uint8_t frameA[8] = {0x00, 0x00, 0x20, GREE_MSG_A, 0x00, 0x00, 0x00, 0x00};

    uint8_t modeVal = GREE_MODE_COOL;
    if      (cmd.mode == "Heat") modeVal = GREE_MODE_HEAT;
    else if (cmd.mode == "Dry")  modeVal = GREE_MODE_DRY;
    else if (cmd.mode == "Fan")  modeVal = GREE_MODE_FAN;
    else if (cmd.mode == "Auto") modeVal = GREE_MODE_AUTO;

    uint8_t fanVal = GREE_FAN_AUTO;
    if      (cmd.fan == "Low")    fanVal = GREE_FAN_LOW;
    else if (cmd.fan == "Medium") fanVal = GREE_FAN_MED;
    else if (cmd.fan == "High" || cmd.fan == "Highest" || cmd.fan == "Max")
                                 fanVal = GREE_FAN_HIGH;

    int temp = constrain(cmd.temp, minTemp(), maxTemp());
    frameA[0] = (uint8_t)(fanVal | modeVal);
    if (cmd.power) frameA[0] |= GREE_POWER_BIT;
    if (cmd.sleep) frameA[0] |= 0x80;

    frameA[1] = (uint8_t)((temp - 16) & 0x0F);

    frameA[2] = cmd.light ? 0x20 : 0x00;
    if (cmd.turbo) frameA[2] |= 0x10;
    if (cmd.clean) frameA[2] |= 0x80;

    frameA[7] = 0;
    if (cmd.econo) frameA[7] |= 0x04;
    frameA[7] |= calcChecksum(frameA);

    uint8_t frameB[8];
    memcpy(frameB, frameA, 8);
    frameB[3] = GREE_MSG_B;
    frameB[6] |= fanVal;
    frameB[7] = 0;
    if (cmd.econo) frameB[7] |= 0x04;
    frameB[7] |= calcChecksum(frameB);

    ir.disableRx();
    IRsend& s = ir.rawSender();
    s.enableIROut(38);
    sendFrame(s, frameA, GREE_FRAME_GAP);
    sendFrame(s, frameB, 20000);
    ir.enableRx();

    Serial.printf("[GREE] Pwr:%s Mode:%s T:%d Fan:%s T:%d E:%d S:%d L:%d C:%d\n",
                  cmd.power ? "ON" : "OFF", cmd.mode.c_str(), temp, cmd.fan.c_str(),
                  cmd.turbo, cmd.econo, cmd.sleep, cmd.light, cmd.clean);
    return true;
}
