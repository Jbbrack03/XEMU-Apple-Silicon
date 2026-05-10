// Diagnostic master: same wire framing as production, but on every
// I2C transmit, ECHOES the exact 21 bytes it's pushing on the bus
// back to the host via CDC. Lets the host compare (a) what the host
// sent, (b) what the master holds in current_state, and (c) what the
// slave receives.
//
// Also prints sizeof(DukeIn) and offsetof of every field at boot.

#include <Arduino.h>
#include <Wire.h>
#include <stddef.h>

struct __attribute__((packed)) DukeIn {
    uint8_t  startByte;
    uint8_t  bLength;
    uint16_t wButtons;
    uint8_t  A, B, X, Y;
    uint8_t  BLACK, WHITE;
    uint8_t  L, R;
    int16_t  leftStickX, leftStickY;
    int16_t  rightStickX, rightStickY;
};
static_assert(sizeof(DukeIn) == 20, "DukeIn must be exactly 20 bytes");

static const uint8_t  FRAME_SYNC1   = 0xAB;
static const uint8_t  FRAME_SYNC2   = 0xCD;
static const uint8_t  FRAME_SIZE    = 25;
static const uint8_t  PAYLOAD_SIZE  = 20;
static const uint8_t  TYPE_DUKE     = 1;
static const uint8_t  I2C_STATUS_DUKE = 0xF1;

DukeIn current_state[3];
uint8_t rx_buf[FRAME_SIZE];
uint8_t rx_pos = 0;
uint32_t last_send_time = 0;
uint32_t frames_received = 0;

void parser_reset() { rx_pos = 0; }

void apply_frame() {
    uint8_t cksum = 0;
    for (uint8_t i = 2; i < FRAME_SIZE - 1; i++) cksum ^= rx_buf[i];
    if (cksum != rx_buf[FRAME_SIZE - 1]) return;
    uint8_t port = rx_buf[2];
    uint8_t type = rx_buf[3];
    if (port < 1 || port > 3 || type != TYPE_DUKE) return;
    memcpy(&current_state[port - 1], &rx_buf[4], PAYLOAD_SIZE);
    current_state[port - 1].startByte = 0;
    current_state[port - 1].bLength   = PAYLOAD_SIZE;
    frames_received++;
}

void feed_byte(uint8_t b) {
    if (rx_pos == 0) {
        if (b == FRAME_SYNC1) rx_buf[rx_pos++] = b;
        return;
    }
    if (rx_pos == 1) {
        if (b == FRAME_SYNC2) rx_buf[rx_pos++] = b;
        else if (b == FRAME_SYNC1) rx_pos = 1;
        else parser_reset();
        return;
    }
    rx_buf[rx_pos++] = b;
    if (rx_pos == FRAME_SIZE) {
        apply_frame();
        parser_reset();
    }
}

void print_hex_byte(uint8_t b) {
    if (b < 0x10) Serial.print('0');
    Serial.print(b, HEX);
}

void send_to_slave_with_echo(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(I2C_STATUS_DUKE);
    Wire.write((uint8_t *)&current_state[addr - 1], PAYLOAD_SIZE);
    uint8_t status = Wire.endTransmission(true);
    // Echo every send. Format: "TX a=ADDR s=STATUS [21 hex bytes]"
    Serial.print(F("TX a="));
    Serial.print(addr);
    Serial.print(F(" s="));
    Serial.print(status);
    Serial.print(F(" "));
    print_hex_byte(I2C_STATUS_DUKE);
    uint8_t *p = (uint8_t *)&current_state[addr - 1];
    for (uint8_t i = 0; i < PAYLOAD_SIZE; i++) {
        print_hex_byte(p[i]);
    }
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    Wire.begin();
    Wire.setClock(400000);
    Wire.setWireTimeout(4000, true);
    for (uint8_t i = 0; i < 3; i++) {
        memset(&current_state[i], 0, sizeof(DukeIn));
        current_state[i].bLength = PAYLOAD_SIZE;
    }
    delay(2500);  // give CDC time to come up so host sees the boot lines
    Serial.print(F("# sizeof(DukeIn)=")); Serial.println(sizeof(DukeIn));
    Serial.print(F("# offsetof startByte="));   Serial.println((unsigned)offsetof(DukeIn, startByte));
    Serial.print(F("# offsetof bLength="));     Serial.println((unsigned)offsetof(DukeIn, bLength));
    Serial.print(F("# offsetof wButtons="));    Serial.println((unsigned)offsetof(DukeIn, wButtons));
    Serial.print(F("# offsetof A="));           Serial.println((unsigned)offsetof(DukeIn, A));
    Serial.print(F("# offsetof B="));           Serial.println((unsigned)offsetof(DukeIn, B));
    Serial.print(F("# offsetof leftStickX=")); Serial.println((unsigned)offsetof(DukeIn, leftStickX));
}

void loop() {
    while (Serial.available()) feed_byte((uint8_t)Serial.read());
    uint32_t now = millis();
    if ((now - last_send_time) >= 100) {  // 10Hz instead of 250Hz to keep echo logs readable
        last_send_time = now;
        send_to_slave_with_echo(1);
    }
}
