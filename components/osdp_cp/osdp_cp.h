#pragma once

#include <string>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/key_provider/key_provider.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

#include "libosdp.h"

namespace esphome::osdp_cp {

/// Fired on every card read. Arguments: card data as uppercase hex, bit length.
class CardTrigger final : public Trigger<std::string, int> {};

/// OSDP Control Panel for a single reader (PD) on one RS-485 bus.
///
/// Keypad presses are forwarded through KeyProvider, so a `key_collector`
/// can assemble PINs. '*' and '#' are normalised from OSDP's 0x7F / 0x0D.
class OSDPControlPanel final : public key_provider::KeyProvider, public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_address(uint8_t address) { this->address_ = address; }
  void set_flow_control_pin(GPIOPin *pin) { this->flow_control_pin_ = pin; }
  void set_scbk(const std::string &hex);
  void set_enforce_secure(bool enforce) { this->enforce_secure_ = enforce; }
  void set_scan(bool scan) { this->scan_ = scan; }
  /// Bench mode with no RS-485 transceiver: drive bus A from the UART TX pin,
  /// bus B from an inverted copy on b_pin, and release both while listening.
  void set_direct_drive(uint8_t a_pin, uint8_t b_pin) {
    this->direct_ = true;
    this->a_pin_ = a_pin;
    this->b_pin_ = b_pin;
  }
  void set_idle_color(uint8_t color) {
    this->idle_color_ = color;
    this->has_idle_color_ = true;
  }
  void set_online_sensor(binary_sensor::BinarySensor *sens) { this->online_sensor_ = sens; }
  void set_secure_channel_sensor(binary_sensor::BinarySensor *sens) { this->sc_sensor_ = sens; }
  void set_last_card_sensor(text_sensor::TextSensor *sens) { this->last_card_sensor_ = sens; }
  void register_card_trigger(CardTrigger *trig) { this->card_triggers_.push_back(trig); }
  /// C++ hook for other components (door_access): card hex + bit length.
  template<typename F> void add_on_card_callback(F &&callback) { this->card_callback_.add(std::forward<F>(callback)); }

  /// Drive the reader LED. Temporary patterns revert to the permanent (idle) state after duration_ms.
  void set_led(uint8_t color, uint8_t off_color, uint32_t on_ms, uint32_t off_ms, uint32_t duration_ms, bool permanent);
  /// Sound the reader buzzer `count` times.
  void beep(uint8_t count, uint32_t on_ms, uint32_t off_ms);

  bool is_online() const { return this->online_; }
  bool is_secure() const { return this->sc_active_; }

 protected:
  static int channel_send_(void *data, uint8_t *buf, int len);
  static int channel_recv_(void *data, uint8_t *buf, int maxlen);
  static void channel_flush_(void *data);
  static int event_callback_(void *arg, int pd, struct osdp_event *ev);
  static void log_callback_(int level, const char *file, unsigned long line, const char *msg);

  void handle_card_(const struct osdp_event_cardread &ev);
  void handle_keypress_(const struct osdp_event_keypress &ev);
  void handle_notification_(const struct osdp_event_notification &ev);
  void set_online_(bool online);
  bool submit_(struct osdp_cmd &cmd);
  void start_osdp_();
  void direct_setup_();
  void direct_drive_();
  void direct_release_();

  enum class ScanState : uint8_t { OFF, SEND, WAIT, PAUSE };
  void scan_loop_();
  void scan_send_id_(uint8_t address);
  bool scan_parse_(uint8_t *address);
  void scan_set_baud_(uint32_t baud);

  osdp_t *ctx_{nullptr};
  osdp_pd_info_t info_{};
  uint8_t address_{0};
  GPIOPin *flow_control_pin_{nullptr};
  uint8_t scbk_[16]{};
  bool has_scbk_{false};
  bool enforce_secure_{false};
  uint8_t idle_color_{0};
  bool has_idle_color_{false};
  bool idle_led_pending_{false};
  bool online_{false};
  bool sc_active_{false};

  bool direct_{false};
  uint8_t a_pin_{0};
  uint8_t b_pin_{0};
  uint32_t tx_sig_{0};
  uint32_t rx_sig_{0};

  bool scan_{false};
  ScanState scan_state_{ScanState::OFF};
  size_t scan_idx_{0};
  bool scan_unicast_{false};
  uint8_t scan_addr_{0};
  uint32_t scan_deadline_{0};
  uint32_t scan_bytes_[5]{};
  uint32_t scan_replies_[5]{};
  size_t scan_probe_{0};
  std::vector<uint8_t> scan_rx_;

  binary_sensor::BinarySensor *online_sensor_{nullptr};
  binary_sensor::BinarySensor *sc_sensor_{nullptr};
  text_sensor::TextSensor *last_card_sensor_{nullptr};
  std::vector<CardTrigger *> card_triggers_;
  CallbackManager<void(const std::string &, int)> card_callback_{};
};

template<typename... Ts> class LedAction final : public Action<Ts...> {
 public:
  explicit LedAction(OSDPControlPanel *parent) : parent_(parent) {}
  void set_color(uint8_t c) { this->color_ = c; }
  void set_off_color(uint8_t c) { this->off_color_ = c; }
  void set_on_time(uint32_t ms) { this->on_ms_ = ms; }
  void set_off_time(uint32_t ms) { this->off_ms_ = ms; }
  void set_duration(uint32_t ms) { this->duration_ms_ = ms; }
  void set_permanent(bool p) { this->permanent_ = p; }

  void play(const Ts &...x) override {
    this->parent_->set_led(this->color_, this->off_color_, this->on_ms_, this->off_ms_, this->duration_ms_,
                           this->permanent_);
  }

 protected:
  OSDPControlPanel *parent_;
  uint8_t color_{0};
  uint8_t off_color_{0};
  uint32_t on_ms_{500};
  uint32_t off_ms_{0};
  uint32_t duration_ms_{2000};
  bool permanent_{false};
};

template<typename... Ts> class BuzzerAction final : public Action<Ts...> {
 public:
  explicit BuzzerAction(OSDPControlPanel *parent) : parent_(parent) {}
  void set_beeps(uint8_t n) { this->beeps_ = n; }
  void set_on_time(uint32_t ms) { this->on_ms_ = ms; }
  void set_off_time(uint32_t ms) { this->off_ms_ = ms; }

  void play(const Ts &...x) override { this->parent_->beep(this->beeps_, this->on_ms_, this->off_ms_); }

 protected:
  OSDPControlPanel *parent_;
  uint8_t beeps_{1};
  uint32_t on_ms_{200};
  uint32_t off_ms_{200};
};

}  // namespace esphome::osdp_cp
