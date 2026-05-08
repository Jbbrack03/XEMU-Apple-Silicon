// ogx360-bridge — custom slot 1 master firmware
// Replaces the Ryzee119 OGX360 master role: instead of reading USB
// controllers via the MAX3421E host shield, we receive controller state
// frames from the Mac over USB CDC serial and forward them to the
// unmodified Ryzee119 slave firmware running on slots 2-4 via the
// existing master/slave I2C protocol.
//
// Target: ATmega32U4 @ 16 MHz, 5V (Pro Micro / Leonardo).
// USB:    CDC serial only (the Pro Micro's default Arduino USB stack).
//         No XID HID. The Xbox doesn't see this MCU at all; only the
//         slave on slot 2 (or 3/4) talks to the Xbox.
//
// Wire frame format (Mac -> this MCU over USB CDC at 115200 baud, 8N1):
//
//     [SYNC1=0xAB][SYNC2=0xCD][PORT][TYPE][PAYLOAD x 20][CHECKSUM]
//
//     PORT     = I2C slave address (1, 2, or 3)
//     TYPE     = 1 (DUKE)  -- only DUKE is supported in this version
//     PAYLOAD  = 20 bytes, exact little-endian usbd_duke_in_t struct
//                from Ryzee119's usbd_xid.h:145-162. Fields:
//                  u8 startByte (0)
//                  u8 bLength   (20)
//                  u16 wButtons
//                  u8 A,B,X,Y,BLACK,WHITE,L,R
//                  i16 leftStickX, leftStickY, rightStickX, rightStickY
//     CHECKSUM = XOR of PORT,TYPE, and 20 PAYLOAD bytes
//
// Frame total: 25 bytes. Re-syncs on bad checksum or framing error.
//
// I2C output (this MCU as master to Ryzee119 slave) — see protocol-analysis.md:
//
//     status = 0xF0 | type    (0xF1 for DUKE)
//     Wire.beginTransmission(slave_addr);
//     Wire.write(status);
//     Wire.write(payload, 20);
//     Wire.endTransmission(true);
//
// Send cadence: every 4 ms (250 Hz), regardless of how often the Mac
// sends new frames. Slave keeps emitting whatever was last given to it.

#include <Arduino.h>
#include <Wire.h>

// ---- Wire frame constants ----
static const uint8_t  FRAME_SYNC1   = 0xAB;
static const uint8_t  FRAME_SYNC2   = 0xCD;
static const uint8_t  FRAME_SIZE    = 25;
static const uint8_t  PAYLOAD_SIZE  = 20;
static const uint8_t  TYPE_DUKE     = 1;

// ---- I2C constants ----
static const uint32_t I2C_CLOCK_HZ      = 400000;
static const uint16_t I2C_TIMEOUT_US    = 4000;
static const uint8_t  I2C_STATUS_DUKE   = 0xF1;
static const uint8_t  I2C_PING          = 0xAA;
static const uint8_t  MIN_SLAVE_ADDR    = 1;
static const uint8_t  MAX_SLAVE_ADDR    = 3;

// ---- Pacing ----
static const uint32_t SEND_INTERVAL_MS  = 4;     // 250 Hz to slave

// ---- LED ----
static const uint8_t  LED_PIN           = LED_BUILTIN;  // typically D17 / RXLED

// ---- Per-port latched state ----
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

DukeIn current_state[3];           // index 0..2 -> port 1..3
bool   port_active[3]   = {false, false, false};
uint32_t last_send_time = 0;
uint32_t last_rx_time   = 0;
uint32_t frames_received = 0;
uint32_t frames_rejected_checksum = 0;
uint32_t frames_rejected_framing  = 0;

// ---- Frame parser state ----
uint8_t  rx_buf[FRAME_SIZE];
uint8_t  rx_pos = 0;

// Reset parser to look for a fresh SYNC1.
inline void parser_reset() { rx_pos = 0; }

// Validate a fully-received frame and apply it.
void apply_frame() {
    // rx_buf[0..1] = SYNC, rx_buf[2] = port, rx_buf[3] = type,
    // rx_buf[4..23] = payload, rx_buf[24] = checksum
    uint8_t cksum = 0;
    for (uint8_t i = 2; i < FRAME_SIZE - 1; i++) {
        cksum ^= rx_buf[i];
    }
    if (cksum != rx_buf[FRAME_SIZE - 1]) {
        frames_rejected_checksum++;
        return;
    }
    uint8_t port = rx_buf[2];
    uint8_t type = rx_buf[3];
    if (port < MIN_SLAVE_ADDR || port > MAX_SLAVE_ADDR) {
        frames_rejected_framing++;
        return;
    }
    if (type != TYPE_DUKE) {
        // Future: STEELBATTALION = 2 not yet supported on the bridge.
        frames_rejected_framing++;
        return;
    }
    memcpy(&current_state[port - 1], &rx_buf[4], PAYLOAD_SIZE);
    // Slave firmware expects startByte=0, bLength=20 — overwrite
    // defensively in case the Mac passes garbage in those fields.
    current_state[port - 1].startByte = 0;
    current_state[port - 1].bLength   = PAYLOAD_SIZE;
    port_active[port - 1] = true;
    frames_received++;
    last_rx_time = millis();
}

// Feed one byte into the framing state machine.
void feed_byte(uint8_t b) {
    if (rx_pos == 0) {
        if (b == FRAME_SYNC1) rx_buf[rx_pos++] = b;
        return;
    }
    if (rx_pos == 1) {
        if (b == FRAME_SYNC2) {
            rx_buf[rx_pos++] = b;
        } else if (b == FRAME_SYNC1) {
            // Treat repeated SYNC1 as a fresh start.
            rx_pos = 1;
        } else {
            frames_rejected_framing++;
            parser_reset();
        }
        return;
    }
    rx_buf[rx_pos++] = b;
    if (rx_pos == FRAME_SIZE) {
        apply_frame();
        parser_reset();
    }
}

// Send one slave's latched state via I2C.
void send_to_slave(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(I2C_STATUS_DUKE);
    Wire.write((uint8_t *)&current_state[addr - 1], PAYLOAD_SIZE);
    Wire.endTransmission(true);
    // We intentionally skip Wire.requestFrom() for rumble — we don't
    // forward rumble back to the Mac in this version. The slave's
    // onRequest handler is benign if not called.
}

// Send a single ping byte (0xAA) to a slave; the slave will blink its
// LED. Useful at boot to confirm presence and to identify which I2C
// address the user's slot 2 has been jumpered to.
void ping_slave(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(I2C_PING);
    Wire.endTransmission(true);
}

// Initialize one port's latched state to a sane "all neutral" Duke.
void init_port_state(uint8_t idx) {
    memset(&current_state[idx], 0, sizeof(DukeIn));
    current_state[idx].startByte = 0;
    current_state[idx].bLength   = PAYLOAD_SIZE;
}

void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    // PLAYER_ID pins are tied to GND on slot 1 of the OGX360 PCB.
    // Leave them as default INPUT (Hi-Z) so we don't fight the board
    // ground. We don't actually read them — the role of this MCU
    // is fixed by virtue of which slot it's installed in.

    // USB CDC serial to the Mac.
    Serial.begin(115200);
    // Don't block on Serial — the slaves should keep getting fresh
    // I2C state even if no Mac is attached yet.

    // I2C master init.
    Wire.begin();
    Wire.setClock(I2C_CLOCK_HZ);
    Wire.setWireTimeout(I2C_TIMEOUT_US, true);

    for (uint8_t i = 0; i < 3; i++) init_port_state(i);

    // Ping slaves at boot — gives a visible LED blink on whichever
    // slot is jumpered to which address. Helpful for first-time setup.
    delay(50);
    for (uint8_t addr = MIN_SLAVE_ADDR; addr <= MAX_SLAVE_ADDR; addr++) {
        ping_slave(addr);
        delay(80);
    }
}

void loop() {
    // 1. Drain whatever the Mac has sent over CDC.
    while (Serial.available()) {
        feed_byte((uint8_t)Serial.read());
    }

    // 2. Send latched state to all slaves periodically.
    uint32_t now = millis();
    if ((now - last_send_time) >= SEND_INTERVAL_MS) {
        last_send_time = now;
        for (uint8_t addr = MIN_SLAVE_ADDR; addr <= MAX_SLAVE_ADDR; addr++) {
            // Only address slaves we've gotten a frame for, OR address
            // address 1 unconditionally (typical single-slave setup).
            // This avoids spamming bus errors on truly absent addresses
            // while still keeping the active port refreshed.
            if (addr == 1 || port_active[addr - 1]) {
                send_to_slave(addr);
            }
        }
    }

    // 3. LED heartbeat — slow blink when no recent Mac frames, faster
    //    blink when actively receiving. Heartbeat is non-blocking.
    static uint32_t led_toggle = 0;
    bool active = (now - last_rx_time) < 250;
    uint16_t period = active ? 50 : 1000;
    if ((now - led_toggle) >= period) {
        led_toggle = now;
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }

    // 4. (Debug-only) Optionally print a status line every ~5s so the
    //    Mac can see frames are arriving cleanly. Cheap; ~30 bytes/5s.
#ifdef DEBUG_STATUS_LINES
    static uint32_t status_timer = 0;
    if ((now - status_timer) >= 5000) {
        status_timer = now;
        Serial.print(F("# rx="));
        Serial.print(frames_received);
        Serial.print(F(" cksum_err="));
        Serial.print(frames_rejected_checksum);
        Serial.print(F(" frame_err="));
        Serial.print(frames_rejected_framing);
        Serial.println();
    }
#endif
}
