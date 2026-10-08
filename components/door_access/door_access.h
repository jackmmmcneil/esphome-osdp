#pragma once

#include <string>
#include <vector>

#include "esphome/components/json/json_util.h"
#include "esphome/components/osdp_cp/osdp_cp.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/core/time.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

namespace esphome::door_access {

static const size_t USER_NAME_LEN = 24;
static const size_t CARD_HEX_LEN = 32;  // hex characters
static const size_t PIN_HASH_LEN = 16;

/// One credential holder, stored as-is in NVS (one preference slot each).
struct StoredUser {
  uint8_t magic;  // USER_MAGIC when the slot is in use
  uint8_t flags;
  uint8_t days;  // bit0 = Monday ... bit6 = Sunday
  uint8_t reserved;
  uint16_t start_min;  // schedule window, minutes since midnight
  uint16_t end_min;
  char name[USER_NAME_LEN + 1];
  char card[CARD_HEX_LEN + 1];
  uint8_t pin_hash[PIN_HASH_LEN];
  uint8_t duress_hash[PIN_HASH_LEN];
} __attribute__((packed));

enum UserFlag : uint8_t {
  FLAG_MASTER = 1 << 0,
  FLAG_CARD_AND_PIN = 1 << 1,
  FLAG_HAS_PIN = 1 << 2,
  FLAG_HAS_DURESS = 1 << 3,
  FLAG_HAS_SCHEDULE = 1 << 4,
};

// Trigger argument lists (strings by value so automations can keep them).
class GrantedTrigger final : public Trigger<std::string, std::string> {};               // user, method
class DeniedTrigger final : public Trigger<std::string, std::string, std::string, std::string> {};  // user, method, reason, card
class DuressTrigger final : public Trigger<std::string, std::string> {};                // user, method
class CommandTrigger final : public Trigger<std::string, std::string, bool> {};         // user, code, authenticated
class EnrolledTrigger final : public Trigger<std::string, std::string> {};              // user, card
class EnrolFailedTrigger final : public Trigger<std::string, std::string> {};           // user, reason

class DoorAccess final : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

  // ---- config
  void set_reader(osdp_cp::OSDPControlPanel *reader) { this->reader_ = reader; }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *clock) { this->clock_ = clock; }
#endif
  void set_max_users(uint8_t n) { this->max_users_ = n; }
  void set_unlock_time(uint32_t ms) { this->unlock_ms_ = ms; }
  void set_pin_timeout(uint32_t ms) { this->pin_timeout_ms_ = ms; }
  void set_card_pin_timeout(uint32_t ms) { this->card_pin_timeout_ms_ = ms; }
  void set_max_pin_failures(uint8_t n) { this->max_failures_ = n; }
  void set_lockout_time(uint32_t ms) { this->lockout_ms_ = ms; }
  void set_idle_color(uint8_t c) { this->idle_color_ = c; }
  void set_lockdown_color(uint8_t c) { this->lockdown_color_ = c; }

  void register_granted_trigger(GrantedTrigger *t) { this->granted_triggers_.push_back(t); }
  void register_denied_trigger(DeniedTrigger *t) { this->denied_triggers_.push_back(t); }
  void register_duress_trigger(DuressTrigger *t) { this->duress_triggers_.push_back(t); }
  void register_command_trigger(CommandTrigger *t) { this->command_triggers_.push_back(t); }
  void register_enrolled_trigger(EnrolledTrigger *t) { this->enrolled_triggers_.push_back(t); }
  void register_enrol_failed_trigger(EnrolFailedTrigger *t) { this->enrol_failed_triggers_.push_back(t); }

  // ---- credential management (return true on success; see last_error())
  bool add_user(const std::string &name, const std::string &mode, bool master);
  bool remove_user(const std::string &name);
  bool set_card(const std::string &name, const std::string &card);
  bool set_pin(const std::string &name, const std::string &pin);
  bool set_duress_pin(const std::string &name, const std::string &pin);
  bool set_schedule(const std::string &name, const std::string &days, const std::string &start,
                    const std::string &end);
  bool start_enrol(const std::string &name, uint32_t timeout_ms = 30000);
  void cancel_enrol();
  const std::string &last_error() const { return this->last_error_; }

  size_t user_count() const;
  bool is_master(const std::string &name) const;
  void users_to_json(JsonObject root) const;

  // ---- runtime
  void set_lockdown(bool on);
  bool is_lockdown() const { return this->lockdown_; }
  void remote_unlock(const std::string &source);

  // Inputs (wired to the reader in setup; public for testing).
  void handle_card(const std::string &hex);
  void handle_key(uint8_t key);

  /// Is `t` inside this user's schedule? Public and static for testing.
  static bool schedule_allows(const StoredUser &u, const ESPTime &t);

 protected:
  enum class State : uint8_t { IDLE, AWAIT_PIN, ENROL };

  void submit_(const std::string &entry);
  void authorize_(int idx, const std::string &method, bool duress, const std::string &code);
  void deny_(const std::string &user, const std::string &method, const std::string &reason,
             const std::string &card = "");
  void record_failure_();
  bool locked_out_() const;

  int find_by_name_(const std::string &name) const;
  int find_by_card_(const std::string &card) const;
  int find_free_slot_() const;
  bool pin_in_use_(const uint8_t *hash, int except_idx, bool except_duress, bool except_pin) const;
  void hash_pin_(const std::string &pin, uint8_t *out) const;
  bool fail_(const std::string &msg);
  void save_(int idx);
  void load_salt_();

  // Reader feedback.
  void show_idle_();
  void fb_grant_();
  void fb_deny_();
  void fb_prompt_pin_();
  void fb_ack_();
  void fb_enrol_(uint32_t ms);

  osdp_cp::OSDPControlPanel *reader_{nullptr};
#ifdef USE_TIME
  time::RealTimeClock *clock_{nullptr};
#endif

  uint8_t max_users_{32};
  uint32_t unlock_ms_{5000};
  uint32_t pin_timeout_ms_{5000};
  uint32_t card_pin_timeout_ms_{10000};
  uint8_t max_failures_{5};
  uint32_t lockout_ms_{60000};
  uint8_t idle_color_{4};      // blue
  uint8_t lockdown_color_{1};  // red

  std::vector<StoredUser> users_;
  std::vector<ESPPreferenceObject> prefs_;
  uint8_t salt_[16]{};

  State state_{State::IDLE};
  std::string entry_;
  uint32_t last_key_ms_{0};
  int pending_idx_{-1};
  uint32_t deadline_ms_{0};
  std::string enrol_name_;

  uint8_t failures_{0};
  uint32_t lockout_until_ms_{0};
  bool lockdown_{false};
  bool reader_was_online_{false};
  std::string last_error_;

  std::vector<GrantedTrigger *> granted_triggers_;
  std::vector<DeniedTrigger *> denied_triggers_;
  std::vector<DuressTrigger *> duress_triggers_;
  std::vector<CommandTrigger *> command_triggers_;
  std::vector<EnrolledTrigger *> enrolled_triggers_;
  std::vector<EnrolFailedTrigger *> enrol_failed_triggers_;
};

}  // namespace esphome::door_access
