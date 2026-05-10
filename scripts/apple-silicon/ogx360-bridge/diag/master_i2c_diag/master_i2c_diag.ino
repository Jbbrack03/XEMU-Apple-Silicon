// One-shot diagnostic — same as master.ino but with I2C ACK reporting
// every 5 seconds for the per-address transmit status.
#include <Arduino.h>
#include <Wire.h>

static const uint8_t  FRAME_SYNC1   = 0xAB;
static const uint8_t  FRAME_SYNC2   = 0xCD;
static const uint8_t  FRAME_SIZE    = 25;
static const uint8_t  PAYLOAD_SIZE  = 20;
static const uint8_t  TYPE_DUKE     = 1;
static const uint32_t I2C_CLOCK_HZ      = 400000;
static const uint16_t I2C_TIMEOUT_US    = 4000;
static const uint8_t  I2C_STATUS_DUKE   = 0xF1;
static const uint8_t  I2C_PING          = 0xAA;
static const uint8_t  MIN_SLAVE_ADDR    = 1;
static const uint8_t  MAX_SLAVE_ADDR    = 3;
static const uint32_t SEND_INTERVAL_MS  = 4;
static const uint8_t  LED_PIN           = LED_BUILTIN;

struct __attribute__((packed)) DukeIn {
    uint8_t  startByte;
    uint8_t  bLength;
    uint16_t wButtons;
    uint8_t  A, B, X, Y, BLACK, WHITE, L, R;
    int16_t  leftStickX, leftStickY, rightStickX, rightStickY;
};

DukeIn current_state[3];
uint32_t last_send_time = 0;
uint8_t  i2c_last_status[3] = {0xFF, 0xFF, 0xFF};
uint32_t i2c_tx_count[3] = {0,0,0};
uint32_t i2c_ack_count[3] = {0,0,0};
uint32_t boot_ping_status[3] = {0xFF, 0xFF, 0xFF};

uint8_t send_to_slave(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(I2C_STATUS_DUKE);
    Wire.write((uint8_t *)&current_state[addr - 1], PAYLOAD_SIZE);
    return Wire.endTransmission(true);
}

uint8_t ping_slave(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(I2C_PING);
    return Wire.endTransmission(true);
}

void setup() {
    pinMode(LED_PIN, OUTPUT);
    Serial.begin(115200);
    Wire.begin();
    Wire.setClock(I2C_CLOCK_HZ);
    Wire.setWireTimeout(I2C_TIMEOUT_US, true);
    for (uint8_t i = 0; i < 3; i++) {
        memset(&current_state[i], 0, sizeof(DukeIn));
        current_state[i].bLength = PAYLOAD_SIZE;
    }
    delay(50);
    for (uint8_t addr = MIN_SLAVE_ADDR; addr <= MAX_SLAVE_ADDR; addr++) {
        boot_ping_status[addr - 1] = ping_slave(addr);
        delay(80);
    }
}

void loop() {
    uint32_t now = millis();
    if ((now - last_send_time) >= SEND_INTERVAL_MS) {
        last_send_time = now;
        for (uint8_t addr = MIN_SLAVE_ADDR; addr <= MAX_SLAVE_ADDR; addr++) {
            uint8_t s = send_to_slave(addr);
            i2c_last_status[addr - 1] = s;
            i2c_tx_count[addr - 1]++;
            if (s == 0) i2c_ack_count[addr - 1]++;
        }
    }
    static uint32_t status_timer = 0;
    if ((now - status_timer) >= 2000) {
        status_timer = now;
        Serial.print(F("# boot_ping="));
        Serial.print(boot_ping_status[0]); Serial.print(F(","));
        Serial.print(boot_ping_status[1]); Serial.print(F(","));
        Serial.print(boot_ping_status[2]);
        Serial.print(F(" tx_status="));
        Serial.print(i2c_last_status[0]); Serial.print(F(","));
        Serial.print(i2c_last_status[1]); Serial.print(F(","));
        Serial.print(i2c_last_status[2]);
        Serial.print(F(" ack/tx="));
        Serial.print(i2c_ack_count[0]); Serial.print(F("/"));
        Serial.print(i2c_tx_count[0]); Serial.print(F(","));
        Serial.print(i2c_ack_count[1]); Serial.print(F("/"));
        Serial.print(i2c_tx_count[1]); Serial.print(F(","));
        Serial.print(i2c_ack_count[2]); Serial.print(F("/"));
        Serial.print(i2c_tx_count[2]);
        Serial.println();
    }
    static uint32_t led_toggle = 0;
    if ((now - led_toggle) >= 500) {
        led_toggle = now;
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
}
