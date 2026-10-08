#include "osdp_cp.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "esphome/core/hal.h"
#ifdef USE_ESP32
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#include <soc/uart_periph.h>
#include "esphome/components/uart/uart_component_esp_idf.h"
#endif
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

// Internal LibOSDP helper (CRC-16/AUG-CCITT, seed 0x1D0F), used by the scanner.
extern "C" uint16_t osdp_compute_crc16(const uint8_t *buf, size_t len);

// LibOSDP's clock. 64-bit so its timeouts survive the 49-day millis() wrap.
extern "C" int64_t osdp_millis_now(void) { return static_cast<int64_t>(esphome::millis_64()); }

namespace esphome::osdp_cp {

static const char *const TAG = "osdp_cp";
static const char *const LIB_TAG = "libosdp";

// OSDP keypad codes for the two non-digit keys (OSDP spec, osdp_KEYPAD).
static const uint8_t OSDP_KEY_STAR = 0x7F;
static const uint8_t OSDP_KEY_HASH = 0x0D;

/// Convert milliseconds to OSDP 100 ms units, clamped to the field size.
static uint16_t to_units(uint32_t ms, uint16_t min_units, uint16_t max_units) {
  uint32_t units = (ms + 50) / 100;
  if (units < min_units)
    units = min_units;
  if (units > max_units)
    units = max_units;
  return static_cast<uint16_t>(units);
}

void OSDPControlPanel::set_scbk(const std::string &hex) {
  if (parse_hex(hex, this->scbk_, sizeof(this->scbk_))) {
    this->has_scbk_ = true;
  } else {
    ESP_LOGE(TAG, "Invalid SCBK; secure channel disabled");
    this->has_scbk_ = false;
  }
}

void OSDPControlPanel::setup() {
  // LibOSDP's bundled crypto draws SC challenge nonces from rand(); seed it
  // from the ESP32 hardware RNG so they aren't predictable after boot.
  srand(random_uint32());

  if (this->flow_control_pin_ != nullptr) {
    this->flow_control_pin_->setup();
    this->flow_control_pin_->digital_write(false);
  }

  osdp_set_log_callback(&OSDPControlPanel::log_callback_);

  if (this->direct_)
    this->direct_setup_();

  if (this->online_sensor_ != nullptr)
    this->online_sensor_->publish_initial_state(false);
  if (this->sc_sensor_ != nullptr)
    this->sc_sensor_->publish_initial_state(false);

  if (this->scan_) {
    ESP_LOGI(TAG, "Scanning for the reader (broadcast osdp_ID at each baud rate)");
    this->scan_state_ = ScanState::SEND;
    this->scan_idx_ = 0;
    return;
  }
  this->start_osdp_();
}

void OSDPControlPanel::start_osdp_() {
  this->info_.name = "reader";
  this->info_.baud_rate = static_cast<int>(this->parent_->get_baud_rate());
  this->info_.address = this->address_;
  this->info_.flags = OSDP_FLAG_ENABLE_NOTIFICATION | OSDP_FLAG_IGN_UNSOLICITED;
  if (this->enforce_secure_)
    this->info_.flags |= OSDP_FLAG_ENFORCE_SECURE;
  this->info_.cap = nullptr;
  this->info_.channel.data = this;
  this->info_.channel.id = 0;
  this->info_.channel.recv = &OSDPControlPanel::channel_recv_;
  this->info_.channel.recv_pkt = nullptr;
  this->info_.channel.send = &OSDPControlPanel::channel_send_;
  this->info_.channel.flush = &OSDPControlPanel::channel_flush_;
  this->info_.channel.release_pkt = nullptr;
  this->info_.channel.close = nullptr;
  this->info_.scbk = this->has_scbk_ ? this->scbk_ : nullptr;

  ESP_LOGI(TAG, "Polling reader at address %u, %u baud", this->address_,
           (unsigned) this->parent_->get_baud_rate());
  this->ctx_ = osdp_cp_setup(1, &this->info_);
  if (this->ctx_ == nullptr) {
    ESP_LOGE(TAG, "LibOSDP CP setup failed");
    this->mark_failed();
    return;
  }
  osdp_cp_set_event_callback(this->ctx_, &OSDPControlPanel::event_callback_, this);
}

void OSDPControlPanel::loop() {
  if (this->scan_state_ != ScanState::OFF) {
    this->scan_loop_();
    return;
  }
  if (this->ctx_ == nullptr)
    return;
  osdp_cp_refresh(this->ctx_);
  if (this->idle_led_pending_ && this->online_) {
    this->idle_led_pending_ = false;
    this->set_led(this->idle_color_, 0, 500, 0, 0, true);
  }
}

// ------------------------------------------------------------------- scan ---
//
// Bypasses LibOSDP and sends hand-built osdp_ID commands, first to the
// broadcast address at each baud rate, then (if the reader only answers as
// 0x7F) to each unicast address. Any valid reply tells us baud + address.

static const uint32_t SCAN_BAUDS[] = {9600, 19200, 38400, 57600, 115200};
static const size_t SCAN_BAUD_COUNT = sizeof(SCAN_BAUDS) / sizeof(SCAN_BAUDS[0]);
static const uint8_t OSDP_SOM = 0x53;
static const uint8_t OSDP_BROADCAST = 0x7F;
static const uint8_t OSDP_CMD_ID_CODE = 0x61;
static const uint8_t OSDP_REPLY_PDID = 0x45;
// Probed at every baud rate: broadcast first, then the usual factory addresses
// in case the reader ignores broadcasts.
static const uint8_t SCAN_PROBES[] = {0x7F, 0x00, 0x01};
static const size_t SCAN_PROBE_COUNT = sizeof(SCAN_PROBES);

void OSDPControlPanel::scan_send_id_(uint8_t address) {
  uint8_t pkt[10];
  pkt[0] = 0xFF;  // OSDP 2.2 mark byte
  pkt[1] = OSDP_SOM;
  pkt[2] = address;
  pkt[3] = 9;  // length, SOM through CRC
  pkt[4] = 0;
  pkt[5] = 0x04;  // sequence 0, CRC-16 follows
  pkt[6] = OSDP_CMD_ID_CODE;
  pkt[7] = 0x00;  // standard ID report
  uint16_t crc = osdp_compute_crc16(&pkt[1], 7);
  pkt[8] = crc & 0xFF;
  pkt[9] = crc >> 8;
  channel_flush_(this);
  this->scan_rx_.clear();
  channel_send_(this, pkt, sizeof(pkt));
}

bool OSDPControlPanel::scan_parse_(uint8_t *address) {
  auto &rx = this->scan_rx_;
  for (size_t i = 0; i + 8 <= rx.size(); i++) {
    if (rx[i] != OSDP_SOM)
      continue;
    size_t len = rx[i + 2] | (rx[i + 3] << 8);
    if (len < 7 || len > 512 || i + len > rx.size())
      continue;
    const uint8_t *p = &rx[i];
    bool valid;
    if (p[4] & 0x04) {
      uint16_t crc = osdp_compute_crc16(p, len - 2);
      valid = (p[len - 2] | (p[len - 1] << 8)) == crc;
    } else {
      uint8_t sum = 0;
      for (size_t j = 0; j < len - 1; j++)
        sum += p[j];
      valid = static_cast<uint8_t>(-sum) == p[len - 1];
    }
    if (!valid || !(p[1] & 0x80))
      continue;  // not a reply (could be our own echo)
    *address = p[1] & 0x7F;
    // osdp_PDID: 3-byte vendor OUI, model, version, serial, firmware.
    size_t data_off = (p[4] & 0x08) ? 6 + p[5] : 6;  // skip security block if any
    if (p[data_off - 1] == OSDP_REPLY_PDID && len >= data_off + 3 + 2) {
      ESP_LOGI(TAG, "  Reader ID: vendor %02X:%02X:%02X model %u version %u", p[data_off], p[data_off + 1],
               p[data_off + 2], p[data_off + 3], p[data_off + 4]);
    }
    return true;
  }
  return false;
}

void OSDPControlPanel::scan_set_baud_(uint32_t baud) {
#if defined(USE_ESP32) || defined(USE_ESP8266)
  if (this->parent_->get_baud_rate() != baud) {
    this->parent_->set_baud_rate(baud);
    this->parent_->load_settings(false);
    if (this->direct_)
      this->direct_release_();  // load_settings re-attaches TX/RX to their pins
  }
#else
  (void) baud;
#endif
}

void OSDPControlPanel::scan_loop_() {
  const uint32_t now = millis();
  switch (this->scan_state_) {
    case ScanState::SEND: {
      uint32_t baud = SCAN_BAUDS[this->scan_idx_];
      this->scan_set_baud_(baud);
      uint8_t addr = this->scan_unicast_ ? this->scan_addr_ : SCAN_PROBES[this->scan_probe_];
      if (!this->scan_unicast_) {
        ESP_LOGD(TAG, "Scan: %u baud, address 0x%02X", (unsigned) baud, addr);
      }
      this->scan_send_id_(addr);
      this->scan_deadline_ = now + 300;
      this->scan_state_ = ScanState::WAIT;
      break;
    }
    case ScanState::WAIT: {
      uint8_t buf[64];
      size_t avail;
      while ((avail = this->available()) > 0 && this->scan_rx_.size() < 512) {
        size_t n = std::min(avail, sizeof(buf));
        if (!this->read_array(buf, n))
          break;
        this->scan_rx_.insert(this->scan_rx_.end(), buf, buf + n);
        this->scan_bytes_[this->scan_idx_] += n;
      }
      uint8_t found;
      if (this->scan_parse_(&found)) {
        [[maybe_unused]] uint32_t baud = SCAN_BAUDS[this->scan_idx_];
        if (found == OSDP_BROADCAST && !this->scan_unicast_) {
          ESP_LOGI(TAG, "Reader answered at %u baud as 0x7F; sweeping unicast addresses", (unsigned) baud);
          this->scan_unicast_ = true;
          this->scan_addr_ = 0;
          this->scan_state_ = ScanState::SEND;
          break;
        }
        ESP_LOGW(TAG,
                 "FOUND READER: address %u at %u baud. Put 'address: %u' under osdp_cp and "
                 "'baud_rate: %u' under uart, then remove 'scan: true'.",
                 found, (unsigned) baud, found, (unsigned) baud);
        this->address_ = found;
        this->scan_state_ = ScanState::OFF;
        this->start_osdp_();
        break;
      }
      if ((int32_t) (now - this->scan_deadline_) < 0)
        break;
      if (this->scan_unicast_) {
        if (++this->scan_addr_ < OSDP_BROADCAST) {
          this->scan_state_ = ScanState::SEND;
          break;
        }
        ESP_LOGW(TAG, "Unicast sweep found nothing; restarting scan");
        this->scan_unicast_ = false;
      } else {
        // A lone byte is usually just the line settling as we stop driving it.
        if (this->scan_rx_.size() > 1)
          this->scan_replies_[this->scan_idx_]++;
        if (++this->scan_probe_ < SCAN_PROBE_COUNT) {
          this->scan_state_ = ScanState::SEND;
          break;
        }
        this->scan_probe_ = 0;
      }
      if (++this->scan_idx_ < SCAN_BAUD_COUNT) {
        this->scan_state_ = ScanState::SEND;
        break;
      }
      // Full pass with no answer: say what came back, then go again.
      ESP_LOGW(TAG, "Scan pass found no reader. Received per baud rate:");
      bool any = false;
      for (size_t i = 0; i < SCAN_BAUD_COUNT; i++) {
        ESP_LOGW(TAG, "  %6u: %u bytes, %u of %u probes got more than 1 byte", (unsigned) SCAN_BAUDS[i],
                 (unsigned) this->scan_bytes_[i], (unsigned) this->scan_replies_[i], (unsigned) SCAN_PROBE_COUNT);
        any |= this->scan_replies_[i] > 0;
        this->scan_bytes_[i] = 0;
        this->scan_replies_[i] = 0;
      }
      if (any) {
        ESP_LOGW(TAG, "  Bytes but no valid reply: try swapping A/B, or check for a TX echo (RE not tied to DE)");
      } else {
        ESP_LOGW(TAG, "  No replies: check 12 V to the reader, common ground, A/B wiring (try swapping), "
                      "and that the reader isn't locked to secure mode");
      }
      this->scan_idx_ = 0;
      this->scan_deadline_ = now + 3000;
      this->scan_state_ = ScanState::PAUSE;
      break;
    }
    case ScanState::PAUSE:
      if ((int32_t) (now - this->scan_deadline_) >= 0)
        this->scan_state_ = ScanState::SEND;
      break;
    case ScanState::OFF:
      break;
  }
}

void OSDPControlPanel::dump_config() {
  ESP_LOGCONFIG(TAG,
                "OSDP Control Panel (LibOSDP %s):\n"
                "  PD address: %u\n"
                "  Baud rate: %u\n"
                "  Secure channel: %s%s\n"
                "  Scan on boot: %s",
                osdp_get_version(), this->address_, (unsigned) this->parent_->get_baud_rate(),
                this->has_scbk_ ? "SCBK set" : "off (plaintext)", this->enforce_secure_ ? ", enforced" : "",
                YESNO(this->scan_));
  LOG_PIN("  Flow control pin: ", this->flow_control_pin_);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup failed");
  }
}

// ---------------------------------------------------------------- channel ---

int OSDPControlPanel::channel_send_(void *data, uint8_t *buf, int len) {
  auto *self = static_cast<OSDPControlPanel *>(data);
  if (len <= 0)
    return 0;
  if (self->direct_) {
    self->direct_drive_();
    self->write_array(buf, static_cast<size_t>(len));
    self->flush();  // returns once the stop bit of the last byte is out
    self->direct_release_();
    return len;
  }
  if (self->flow_control_pin_ != nullptr)
    self->flow_control_pin_->digital_write(true);
  self->write_array(buf, static_cast<size_t>(len));
  if (self->flow_control_pin_ != nullptr) {
    // Hold DE high until the last stop bit has left the shift register.
    self->flush();
    self->flow_control_pin_->digital_write(false);
  }
  return len;
}

int OSDPControlPanel::channel_recv_(void *data, uint8_t *buf, int maxlen) {
  auto *self = static_cast<OSDPControlPanel *>(data);
  size_t avail = self->available();
  if (avail == 0 || maxlen <= 0)
    return 0;
  size_t n = std::min(avail, static_cast<size_t>(maxlen));
  if (!self->read_array(buf, n))
    return -1;
  return static_cast<int>(n);
}

void OSDPControlPanel::channel_flush_(void *data) {
  auto *self = static_cast<OSDPControlPanel *>(data);
  uint8_t scratch[32];
  size_t avail;
  while ((avail = self->available()) > 0) {
    self->read_array(scratch, std::min(avail, sizeof(scratch)));
  }
}

// --------------------------------------------------------- direct drive ---
//
// No transceiver: A = UART TX, B = inverted UART TX (via the GPIO matrix), so
// the pair swings +/-3.3 V differentially. While listening both pins are
// released (high impedance, weakly biased to the idle state) and the UART RX
// signal is read from the A pin. RX is muted while we transmit so our own
// bytes aren't echoed back into LibOSDP.

#ifdef USE_ESP32
void OSDPControlPanel::direct_setup_() {
  auto *uart = static_cast<uart::IDFUARTComponent *>(this->parent_);
  uint8_t port = uart->get_hw_serial_number();
  this->tx_sig_ = UART_PERIPH_SIGNAL(port, SOC_UART_TX_PIN_IDX);
  this->rx_sig_ = UART_PERIPH_SIGNAL(port, SOC_UART_RX_PIN_IDX);
  gpio_reset_pin(static_cast<gpio_num_t>(this->b_pin_));
  // Full drive strength in case the reader has a 120 ohm terminator across A/B.
  gpio_set_drive_capability(static_cast<gpio_num_t>(this->a_pin_), GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability(static_cast<gpio_num_t>(this->b_pin_), GPIO_DRIVE_CAP_3);
  this->direct_release_();
  ESP_LOGW(TAG, "Direct-drive RS-485 (no transceiver): A=GPIO%u, B=GPIO%u. Bench use only.", this->a_pin_,
           this->b_pin_);
}

void OSDPControlPanel::direct_drive_() {
  auto a = static_cast<gpio_num_t>(this->a_pin_);
  auto b = static_cast<gpio_num_t>(this->b_pin_);
  esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, this->rx_sig_, false);  // mute RX
  gpio_set_pull_mode(a, GPIO_FLOATING);
  gpio_set_pull_mode(b, GPIO_FLOATING);
  esp_rom_gpio_connect_out_signal(a, this->tx_sig_, false, false);
  esp_rom_gpio_connect_out_signal(b, this->tx_sig_, true, false);
  gpio_set_direction(a, GPIO_MODE_INPUT_OUTPUT);
  gpio_set_direction(b, GPIO_MODE_INPUT_OUTPUT);
}

void OSDPControlPanel::direct_release_() {
  auto a = static_cast<gpio_num_t>(this->a_pin_);
  auto b = static_cast<gpio_num_t>(this->b_pin_);
  // Hand the pins back to plain GPIO holding the idle state (A high, B low) so
  // the switch-over doesn't put a glitch on the bus, then let go of them.
  gpio_set_level(a, 1);
  gpio_set_level(b, 0);
  esp_rom_gpio_connect_out_signal(a, SIG_GPIO_OUT_IDX, false, false);
  esp_rom_gpio_connect_out_signal(b, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction(a, GPIO_MODE_INPUT);
  gpio_set_direction(b, GPIO_MODE_INPUT);
  // Idle bias: A high, B low = a line at rest reads as mark (logic 1).
  gpio_set_pull_mode(a, GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(b, GPIO_PULLDOWN_ONLY);
  esp_rom_gpio_connect_in_signal(a, this->rx_sig_, false);
}
#else
void OSDPControlPanel::direct_setup_() { ESP_LOGE(TAG, "direct_drive is only supported on ESP32"); }
void OSDPControlPanel::direct_drive_() {}
void OSDPControlPanel::direct_release_() {}
#endif

// ----------------------------------------------------------------- events ---

int OSDPControlPanel::event_callback_(void *arg, int pd, struct osdp_event *ev) {
  auto *self = static_cast<OSDPControlPanel *>(arg);
  (void) pd;
  switch (ev->type) {
    case OSDP_EVENT_CARDREAD:
      self->handle_card_(ev->cardread);
      break;
    case OSDP_EVENT_KEYPRESS:
      self->handle_keypress_(ev->keypress);
      break;
    case OSDP_EVENT_NOTIFICATION:
      self->handle_notification_(ev->notif);
      break;
    case OSDP_EVENT_STATUS:
      ESP_LOGD(TAG, "Status report received");
      break;
    default:
      ESP_LOGV(TAG, "Unhandled event type %d", ev->type);
      break;
  }
  return 0;
}

void OSDPControlPanel::handle_card_(const struct osdp_event_cardread &ev) {
  int bits = ev.length;
  size_t nbytes;
  if (ev.format == OSDP_CARD_FMT_ASCII) {
    nbytes = static_cast<size_t>(ev.length);
    bits = ev.length * 8;
  } else {
    nbytes = static_cast<size_t>((ev.length + 7) / 8);
  }
  nbytes = std::min(nbytes, sizeof(ev.data));

  std::string hex;
  hex.reserve(nbytes * 2);
  for (size_t i = 0; i < nbytes; i++) {
    hex.push_back(format_hex_pretty_char(ev.data[i] >> 4));
    hex.push_back(format_hex_pretty_char(ev.data[i] & 0x0F));
  }

  ESP_LOGI(TAG, "Card read: %s (%d bits, format %d%s)", hex.c_str(), bits, ev.format,
           ev.direction ? ", reverse" : "");

  if (this->last_card_sensor_ != nullptr)
    this->last_card_sensor_->publish_state(hex);
  for (auto *trig : this->card_triggers_)
    trig->trigger(hex, bits);
  this->card_callback_.call(hex, bits);
}

void OSDPControlPanel::handle_keypress_(const struct osdp_event_keypress &ev) {
  int len = std::min(ev.length, static_cast<int>(sizeof(ev.data)));
  for (int i = 0; i < len; i++) {
    uint8_t key = ev.data[i];
    if (key == OSDP_KEY_STAR)
      key = '*';
    else if (key == OSDP_KEY_HASH)
      key = '#';
    else if (key <= 9)
      key = '0' + key;  // some readers send raw digit values
    // Don't log the digit itself: these are PINs.
    ESP_LOGV(TAG, "Key press");
    this->send_key_(key);
  }
}

void OSDPControlPanel::handle_notification_(const struct osdp_event_notification &ev) {
  switch (ev.type) {
    case OSDP_EVENT_NOTIFICATION_PD_STATUS:
      this->set_online_(ev.arg0 != 0);
      break;
    case OSDP_EVENT_NOTIFICATION_SC_STATUS:
      this->sc_active_ = ev.arg0 != 0;
      if (this->sc_active_ && ev.arg1 != 0) {
        ESP_LOGW(TAG, "Secure channel up with the default install key (SCBK-D); provisioning SCBK");
      } else {
        ESP_LOGI(TAG, "Secure channel %s", this->sc_active_ ? "active" : "inactive");
      }
      if (this->sc_sensor_ != nullptr)
        this->sc_sensor_->publish_state(this->sc_active_ && ev.arg1 == 0);
      break;
    case OSDP_EVENT_NOTIFICATION_COMMAND:
      // LibOSDP passes true (non-zero) on success here, despite osdp.h saying 0.
      if (ev.arg1 == 0) {
        ESP_LOGW(TAG, "Command %d to reader failed", ev.arg0);
      } else {
        ESP_LOGV(TAG, "Command %d acknowledged", ev.arg0);
      }
      break;
    default:
      break;
  }
}

void OSDPControlPanel::set_online_(bool online) {
  if (online == this->online_)
    return;
  this->online_ = online;
  if (online) {
    ESP_LOGI(TAG, "Reader online");
    // LibOSDP notifies before it finishes the state change, so commands
    // submitted here are rejected; send the idle LED from the next loop().
    this->idle_led_pending_ = this->has_idle_color_;
  } else {
    ESP_LOGW(TAG, "Reader offline");
    this->sc_active_ = false;
    if (this->sc_sensor_ != nullptr)
      this->sc_sensor_->publish_state(false);
  }
  if (this->online_sensor_ != nullptr)
    this->online_sensor_->publish_state(online);
}

// --------------------------------------------------------------- commands ---

bool OSDPControlPanel::submit_(struct osdp_cmd &cmd) {
  if (this->ctx_ == nullptr || !this->online_) {
    ESP_LOGD(TAG, "Reader offline; command dropped");
    return false;
  }
  if (osdp_cp_submit_command(this->ctx_, 0, &cmd) != 0) {
    ESP_LOGW(TAG, "Failed to queue command %d", cmd.id);
    return false;
  }
  return true;
}

void OSDPControlPanel::set_led(uint8_t color, uint8_t off_color, uint32_t on_ms, uint32_t off_ms,
                               uint32_t duration_ms, bool permanent) {
  struct osdp_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.id = OSDP_CMD_LED;
  cmd.led.reader = 0;
  cmd.led.led_number = 0;

  struct osdp_cmd_led_params params;
  memset(&params, 0, sizeof(params));
  params.control_code = permanent ? 1 : 2;
  params.on_count = static_cast<uint8_t>(to_units(on_ms, 1, 255));
  params.off_count = static_cast<uint8_t>(to_units(off_ms, 0, 255));
  params.on_color = color;
  params.off_color = off_color;

  if (permanent) {
    cmd.led.permanent = params;
    cmd.led.temporary.control_code = 1;  // cancel any temporary pattern
  } else {
    params.timer_count = to_units(duration_ms, 1, 0xFFFF);
    cmd.led.temporary = params;
    cmd.led.permanent.control_code = 0;  // leave idle state alone
  }
  this->submit_(cmd);
}

void OSDPControlPanel::beep(uint8_t count, uint32_t on_ms, uint32_t off_ms) {
  struct osdp_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.id = OSDP_CMD_BUZZER;
  cmd.buzzer.reader = 0;
  cmd.buzzer.control_code = 2;  // default tone
  cmd.buzzer.on_count = static_cast<uint8_t>(to_units(on_ms, 1, 255));
  cmd.buzzer.off_count = static_cast<uint8_t>(to_units(off_ms, 0, 255));
  cmd.buzzer.rep_count = count == 0 ? 1 : count;
  this->submit_(cmd);
}

// ---------------------------------------------------------------- logging ---

void OSDPControlPanel::log_callback_(int level, const char *file, unsigned long line, const char *msg) {
  (void) file;
  (void) line;
  if (msg == nullptr)
    return;
  int len = static_cast<int>(strlen(msg));
  while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
    len--;
  if (level <= OSDP_LOG_ERROR) {
    ESP_LOGE(LIB_TAG, "%.*s", len, msg);
  } else if (level == OSDP_LOG_WARNING) {
    ESP_LOGW(LIB_TAG, "%.*s", len, msg);
  } else if (level <= OSDP_LOG_INFO) {
    ESP_LOGI(LIB_TAG, "%.*s", len, msg);
  } else {
    ESP_LOGV(LIB_TAG, "%.*s", len, msg);
  }
}

}  // namespace esphome::osdp_cp
