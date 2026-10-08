#include "door_access.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "esphome/components/sha256/sha256.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::door_access {

static const char *const TAG = "door_access";

static const uint8_t USER_MAGIC = 0xA7;
static const uint32_t PREF_SALT = 0xD00A5A17;
static const uint32_t PREF_USER_BASE = 0xD00A1000;

// OSDP LED colours (osdp_led_color_e)
static const uint8_t RED = 1, GREEN = 2, AMBER = 3, BLUE = 4;

static const char *const DAY_NAMES[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

// ------------------------------------------------------------ helpers ---

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

static bool names_equal(const char *stored, const std::string &name) {
  return lower(stored) == lower(name);
}

/// Uppercase hex, separators removed. Empty string if invalid.
static std::string normalize_card(const std::string &in) {
  std::string out;
  for (char c : in) {
    if (c == ':' || c == ' ' || c == '-')
      continue;
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return "";
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

static bool valid_pin(const std::string &pin) {
  if (pin.size() < 4 || pin.size() > 8)
    return false;
  return std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; });
}

static bool parse_hhmm(const std::string &s, uint16_t *out) {
  int h, m;
  char extra;
  if (sscanf(s.c_str(), "%d:%d%c", &h, &m, &extra) != 2 || h < 0 || h > 24 || m < 0 || m > 59 || (h == 24 && m))
    return false;
  *out = static_cast<uint16_t>(h * 60 + m);
  return true;
}

static bool parse_days(const std::string &spec, uint8_t *out) {
  std::string s = lower(trim(spec));
  if (s.empty() || s == "all" || s == "every day" || s == "daily") {
    *out = 0x7F;
    return true;
  }
  if (s == "weekdays") {
    *out = 0x1F;
    return true;
  }
  if (s == "weekends") {
    *out = 0x60;
    return true;
  }
  uint8_t mask = 0;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t comma = s.find(',', pos);
    std::string tok = trim(s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
    if (!tok.empty()) {
      // Allow ranges like mon-fri.
      size_t dash = tok.find('-');
      std::string a = dash == std::string::npos ? tok : trim(tok.substr(0, dash));
      std::string b = dash == std::string::npos ? tok : trim(tok.substr(dash + 1));
      int ia = -1, ib = -1;
      for (int i = 0; i < 7; i++) {
        if (a.compare(0, 3, DAY_NAMES[i]) == 0 && a.size() >= 3)
          ia = i;
        if (b.compare(0, 3, DAY_NAMES[i]) == 0 && b.size() >= 3)
          ib = i;
      }
      if (ia < 0 || ib < 0)
        return false;
      for (int i = ia;; i = (i + 1) % 7) {
        mask |= 1 << i;
        if (i == ib)
          break;
      }
    }
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }
  if (mask == 0)
    return false;
  *out = mask;
  return true;
}

static std::string days_to_string(uint8_t mask) {
  if (mask == 0x7F)
    return "every day";
  if (mask == 0x1F)
    return "weekdays";
  if (mask == 0x60)
    return "weekends";
  std::string out;
  for (int i = 0; i < 7; i++) {
    if (mask & (1 << i)) {
      if (!out.empty())
        out += ",";
      out += DAY_NAMES[i];
    }
  }
  return out;
}

static std::string hhmm(uint16_t mins) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned) (mins / 60) % 100, (unsigned) mins % 60);
  return buf;
}

// -------------------------------------------------------------- setup ---

void DoorAccess::setup() {
  this->load_salt_();
  this->users_.assign(this->max_users_, StoredUser{});
  this->prefs_.reserve(this->max_users_);
  for (uint8_t i = 0; i < this->max_users_; i++) {
    this->prefs_.push_back(global_preferences->make_preference<StoredUser>(PREF_USER_BASE + i, true));
    StoredUser u{};
    if (this->prefs_[i].load(&u) && u.magic == USER_MAGIC) {
      u.name[USER_NAME_LEN] = '\0';
      u.card[CARD_HEX_LEN] = '\0';
      this->users_[i] = u;
    }
  }
  ESP_LOGI(TAG, "Loaded %u user(s)", (unsigned) this->user_count());

  if (this->reader_ != nullptr) {
    this->reader_->add_on_card_callback([this](const std::string &hex, int) { this->handle_card(hex); });
    this->reader_->add_on_key_callback([this](uint8_t key) { this->handle_key(key); });
  }
}

void DoorAccess::load_salt_() {
  auto pref = global_preferences->make_preference<decltype(this->salt_)>(PREF_SALT, true);
  if (!pref.load(&this->salt_)) {
    for (size_t i = 0; i < sizeof(this->salt_); i += 4) {
      uint32_t r = random_uint32();
      memcpy(this->salt_ + i, &r, 4);
    }
    pref.save(&this->salt_);
    global_preferences->sync();
  }
}

void DoorAccess::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Door access:\n"
                "  Users: %u of %u\n"
                "  Unlock time: %u ms\n"
                "  PIN lockout: %u failures -> %u s\n"
                "  Time source: %s",
                (unsigned) this->user_count(), this->max_users_, (unsigned) this->unlock_ms_, this->max_failures_,
                (unsigned) (this->lockout_ms_ / 1000),
#ifdef USE_TIME
                this->clock_ != nullptr ? "yes" : "none (scheduled users are denied)"
#else
                "none (scheduled users are denied)"
#endif
  );
}

void DoorAccess::loop() {
  const uint32_t now = millis();

  if (this->reader_ != nullptr) {
    bool online = this->reader_->is_online();
    if (online && !this->reader_was_online_)
      this->show_idle_();
    this->reader_was_online_ = online;
  }

  if (!this->entry_.empty() && this->state_ != State::AWAIT_PIN && now - this->last_key_ms_ > this->pin_timeout_ms_) {
    ESP_LOGD(TAG, "Keypad entry timed out");
    this->entry_.clear();
  }

  if (this->state_ == State::AWAIT_PIN && (int32_t) (now - this->deadline_ms_) >= 0) {
    std::string name = this->pending_idx_ >= 0 ? this->users_[this->pending_idx_].name : "";
    this->state_ = State::IDLE;
    this->pending_idx_ = -1;
    this->entry_.clear();
    this->deny_(name, "card+pin", "pin_timeout");
  }

  if (this->state_ == State::ENROL && (int32_t) (now - this->deadline_ms_) >= 0) {
    ESP_LOGW(TAG, "Enrolment for '%s' timed out", this->enrol_name_.c_str());
    this->state_ = State::IDLE;
    for (auto *t : this->enrol_failed_triggers_)
      t->trigger(this->enrol_name_, "timeout");
    this->show_idle_();
  }
}

// ------------------------------------------------------------- inputs ---

void DoorAccess::handle_card(const std::string &hex) {
  std::string card = normalize_card(hex);
  if (card.empty() || card.size() > CARD_HEX_LEN)
    return;

  if (this->state_ == State::ENROL) {
    std::string name = this->enrol_name_;
    this->state_ = State::IDLE;
    int owner = this->find_by_card_(card);
    int idx = this->find_by_name_(name);
    if (owner >= 0 && owner != idx) {
      ESP_LOGW(TAG, "Enrol: card already belongs to '%s'", this->users_[owner].name);
      for (auto *t : this->enrol_failed_triggers_)
        t->trigger(name, std::string("card already assigned to ") + this->users_[owner].name);
      this->fb_deny_();
      this->show_idle_();
      return;
    }
    if (idx < 0 && !this->add_user(name, "card_or_pin", false)) {
      for (auto *t : this->enrol_failed_triggers_)
        t->trigger(name, this->last_error_);
      this->fb_deny_();
      this->show_idle_();
      return;
    }
    this->set_card(name, card);
    ESP_LOGI(TAG, "Enrolled card %s for '%s'", card.c_str(), name.c_str());
    for (auto *t : this->enrolled_triggers_)
      t->trigger(name, card);
    this->fb_ack_();
    this->show_idle_();
    return;
  }

  this->entry_.clear();
  if (this->state_ == State::AWAIT_PIN) {
    // A new card replaces a pending card+PIN attempt.
    this->state_ = State::IDLE;
    this->pending_idx_ = -1;
  }

  int idx = this->find_by_card_(card);
  if (idx < 0) {
    this->deny_("", "card", "unknown_card", card);
    return;
  }
  const StoredUser &u = this->users_[idx];
  if (this->lockdown_ && !(u.flags & FLAG_MASTER)) {
    this->deny_(u.name, "card", "lockdown");
    return;
  }
  if (u.flags & FLAG_CARD_AND_PIN) {
    if (!(u.flags & FLAG_HAS_PIN)) {
      this->deny_(u.name, "card", "no_pin_set");
      return;
    }
    ESP_LOGI(TAG, "Card for '%s' accepted; waiting for PIN", u.name);
    this->state_ = State::AWAIT_PIN;
    this->pending_idx_ = idx;
    this->deadline_ms_ = millis() + this->card_pin_timeout_ms_;
    this->fb_prompt_pin_();
    return;
  }
  this->authorize_(idx, "card", false, "");
}

void DoorAccess::handle_key(uint8_t key) {
  if (key == '#') {
    std::string entry = this->entry_;
    this->entry_.clear();
    if (!entry.empty())
      this->submit_(entry);
    return;
  }
  if (this->entry_.size() < 24)
    this->entry_.push_back(static_cast<char>(key));
  this->last_key_ms_ = millis();
}

// Entry formats (all end with #):
//   PIN#          unlock (or finish card+PIN)
//   PIN*CODE#     authenticated command, no unlock
//   *CODE#        unauthenticated command (e.g. doorbell)
void DoorAccess::submit_(const std::string &entry) {
  if (entry[0] == '*') {
    std::string code = entry.substr(1);
    if (code.empty())
      return;
    ESP_LOGI(TAG, "Command *%s# (no PIN)", code.c_str());
    for (auto *t : this->command_triggers_)
      t->trigger("", code, false);
    this->fb_ack_();
    return;
  }

  size_t star = entry.find('*');
  std::string pin = entry.substr(0, star);
  std::string code = star == std::string::npos ? "" : entry.substr(star + 1);
  const bool awaiting = this->state_ == State::AWAIT_PIN;
  const std::string method = awaiting ? "card+pin" : "pin";

  if (this->locked_out_()) {
    std::string name = awaiting ? this->users_[this->pending_idx_].name : "";
    this->state_ = State::IDLE;
    this->pending_idx_ = -1;
    this->deny_(name, method, "locked_out");
    return;
  }

  if (!valid_pin(pin)) {
    std::string name = awaiting ? this->users_[this->pending_idx_].name : "";
    this->state_ = State::IDLE;
    this->pending_idx_ = -1;
    this->record_failure_();
    this->deny_(name, method, "wrong_pin");
    return;
  }

  uint8_t hash[PIN_HASH_LEN];
  this->hash_pin_(pin, hash);

  if (awaiting) {
    int idx = this->pending_idx_;
    this->state_ = State::IDLE;
    this->pending_idx_ = -1;
    const StoredUser &u = this->users_[idx];
    bool ok = (u.flags & FLAG_HAS_PIN) && memcmp(u.pin_hash, hash, PIN_HASH_LEN) == 0;
    bool duress = (u.flags & FLAG_HAS_DURESS) && memcmp(u.duress_hash, hash, PIN_HASH_LEN) == 0;
    if (!ok && !duress) {
      this->record_failure_();
      this->deny_(u.name, method, "wrong_pin");
      return;
    }
    this->authorize_(idx, method, duress, code);
    return;
  }

  for (int i = 0; i < (int) this->users_.size(); i++) {
    const StoredUser &u = this->users_[i];
    if (u.magic != USER_MAGIC)
      continue;
    bool ok = (u.flags & FLAG_HAS_PIN) && memcmp(u.pin_hash, hash, PIN_HASH_LEN) == 0;
    bool duress = (u.flags & FLAG_HAS_DURESS) && memcmp(u.duress_hash, hash, PIN_HASH_LEN) == 0;
    if (!ok && !duress)
      continue;
    if (u.flags & FLAG_CARD_AND_PIN) {
      this->deny_(u.name, method, "card_required");
      return;
    }
    this->authorize_(i, method, duress, code);
    return;
  }
  this->record_failure_();
  this->deny_("", method, "unknown_pin");
}

void DoorAccess::authorize_(int idx, const std::string &method, bool duress, const std::string &code) {
  const StoredUser &u = this->users_[idx];
  const bool master = u.flags & FLAG_MASTER;
  if (this->lockdown_ && !master) {
    this->deny_(u.name, method, "lockdown");
    return;
  }
  if (!master && (u.flags & FLAG_HAS_SCHEDULE)) {
    bool allowed = false;
#ifdef USE_TIME
    if (this->clock_ != nullptr) {
      ESPTime now = this->clock_->now();
      if (!now.is_valid()) {
        this->deny_(u.name, method, "no_time");
        return;
      }
      allowed = schedule_allows(u, now);
    }
#endif
    if (!allowed) {
      this->deny_(u.name, method, "outside_schedule");
      return;
    }
  }

  this->failures_ = 0;
  std::string name = u.name;
  if (duress) {
    // Looks like a normal unlock at the door; the alert goes out separately.
    ESP_LOGW(TAG, "DURESS code used by '%s'", name.c_str());
    for (auto *t : this->duress_triggers_)
      t->trigger(name, method);
  }
  if (!code.empty()) {
    ESP_LOGI(TAG, "Command *%s# by '%s'", code.c_str(), name.c_str());
    for (auto *t : this->command_triggers_)
      t->trigger(name, code, true);
    this->fb_ack_();
    return;
  }
  ESP_LOGI(TAG, "Access granted: '%s' (%s)", name.c_str(), method.c_str());
  this->fb_grant_();
  for (auto *t : this->granted_triggers_)
    t->trigger(name, method);
}

void DoorAccess::deny_(const std::string &user, const std::string &method, const std::string &reason,
                       const std::string &card) {
  ESP_LOGW(TAG, "Access denied: user='%s' method=%s reason=%s%s%s", user.c_str(), method.c_str(), reason.c_str(),
           card.empty() ? "" : " card=", card.c_str());
  this->fb_deny_();
  for (auto *t : this->denied_triggers_)
    t->trigger(user, method, reason, card);
}

void DoorAccess::record_failure_() {
  if (++this->failures_ >= this->max_failures_) {
    this->failures_ = 0;
    this->lockout_until_ms_ = millis() + this->lockout_ms_;
    ESP_LOGW(TAG, "Too many wrong PINs; keypad locked for %u s", (unsigned) (this->lockout_ms_ / 1000));
  }
}

bool DoorAccess::locked_out_() const {
  return this->lockout_until_ms_ != 0 && (int32_t) (millis() - this->lockout_until_ms_) < 0;
}

bool DoorAccess::schedule_allows(const StoredUser &u, const ESPTime &t) {
  if (!(u.flags & FLAG_HAS_SCHEDULE))
    return true;
  int today = (t.day_of_week + 5) % 7;  // ESPTime: 1 = Sunday. Ours: 0 = Monday.
  int yesterday = (today + 6) % 7;
  int m = t.hour * 60 + t.minute;
  bool all_day = u.start_min == u.end_min;
  if (all_day)
    return u.days & (1 << today);
  if (u.start_min < u.end_min)
    return (u.days & (1 << today)) && m >= u.start_min && m < u.end_min;
  // Overnight window, e.g. 22:00-06:00. The early-morning part belongs to the previous day.
  if (m >= u.start_min)
    return u.days & (1 << today);
  if (m < u.end_min)
    return u.days & (1 << yesterday);
  return false;
}

// ----------------------------------------------------------- runtime ---

void DoorAccess::set_lockdown(bool on) {
  if (on == this->lockdown_)
    return;
  this->lockdown_ = on;
  ESP_LOGW(TAG, "Lockdown %s", on ? "ON: only master credentials accepted" : "off");
  if (on && this->state_ == State::AWAIT_PIN) {
    this->state_ = State::IDLE;
    this->pending_idx_ = -1;
  }
  this->show_idle_();
}

void DoorAccess::remote_unlock(const std::string &source) {
  ESP_LOGI(TAG, "Remote unlock (%s)", source.c_str());
  this->fb_grant_();
  for (auto *t : this->granted_triggers_)
    t->trigger(source, "remote");
}

// ------------------------------------------------------- management ---

bool DoorAccess::fail_(const std::string &msg) {
  this->last_error_ = msg;
  ESP_LOGW(TAG, "%s", msg.c_str());
  return false;
}

int DoorAccess::find_by_name_(const std::string &name) const {
  for (int i = 0; i < (int) this->users_.size(); i++) {
    if (this->users_[i].magic == USER_MAGIC && names_equal(this->users_[i].name, name))
      return i;
  }
  return -1;
}

int DoorAccess::find_by_card_(const std::string &card) const {
  for (int i = 0; i < (int) this->users_.size(); i++) {
    if (this->users_[i].magic == USER_MAGIC && this->users_[i].card[0] != '\0' && card == this->users_[i].card)
      return i;
  }
  return -1;
}

int DoorAccess::find_free_slot_() const {
  for (int i = 0; i < (int) this->users_.size(); i++) {
    if (this->users_[i].magic != USER_MAGIC)
      return i;
  }
  return -1;
}

bool DoorAccess::pin_in_use_(const uint8_t *hash, int except_idx, bool except_duress, bool except_pin) const {
  for (int i = 0; i < (int) this->users_.size(); i++) {
    const StoredUser &u = this->users_[i];
    if (u.magic != USER_MAGIC)
      continue;
    bool skip_pin = i == except_idx && except_pin;
    bool skip_duress = i == except_idx && except_duress;
    if (!skip_pin && (u.flags & FLAG_HAS_PIN) && memcmp(u.pin_hash, hash, PIN_HASH_LEN) == 0)
      return true;
    if (!skip_duress && (u.flags & FLAG_HAS_DURESS) && memcmp(u.duress_hash, hash, PIN_HASH_LEN) == 0)
      return true;
  }
  return false;
}

void DoorAccess::hash_pin_(const std::string &pin, uint8_t *out) const {
  sha256::SHA256 sha;
  sha.init();
  sha.add(this->salt_, sizeof(this->salt_));
  sha.add(pin);
  sha.calculate();
  uint8_t full[32];
  sha.get_bytes(full);
  memcpy(out, full, PIN_HASH_LEN);
}

void DoorAccess::save_(int idx) {
  this->prefs_[idx].save(&this->users_[idx]);
  global_preferences->sync();
}

bool DoorAccess::add_user(const std::string &raw_name, const std::string &raw_mode, bool master) {
  this->last_error_.clear();
  std::string name = trim(raw_name);
  if (name.empty() || name.size() > USER_NAME_LEN)
    return this->fail_("Name must be 1-24 characters");
  std::string mode = lower(trim(raw_mode));
  bool card_and_pin;
  if (mode.empty() || mode == "card_or_pin" || mode == "either")
    card_and_pin = false;
  else if (mode == "card_and_pin" || mode == "both")
    card_and_pin = true;
  else
    return this->fail_("Mode must be card_or_pin or card_and_pin");

  int idx = this->find_by_name_(name);
  if (idx < 0) {
    idx = this->find_free_slot_();
    if (idx < 0)
      return this->fail_("User list is full");
    this->users_[idx] = StoredUser{};
    this->users_[idx].magic = USER_MAGIC;
    strncpy(this->users_[idx].name, name.c_str(), USER_NAME_LEN);
    ESP_LOGI(TAG, "Added user '%s'", name.c_str());
  }
  StoredUser &u = this->users_[idx];
  u.flags = (u.flags & ~(FLAG_MASTER | FLAG_CARD_AND_PIN)) | (master ? FLAG_MASTER : 0) |
            (card_and_pin ? FLAG_CARD_AND_PIN : 0);
  this->save_(idx);
  return true;
}

bool DoorAccess::remove_user(const std::string &name) {
  this->last_error_.clear();
  int idx = this->find_by_name_(name);
  if (idx < 0)
    return this->fail_("No user named '" + trim(name) + "'");
  ESP_LOGI(TAG, "Removed user '%s'", this->users_[idx].name);
  this->users_[idx] = StoredUser{};
  this->save_(idx);
  if (this->pending_idx_ == idx) {
    this->state_ = State::IDLE;
    this->pending_idx_ = -1;
  }
  return true;
}

bool DoorAccess::set_card(const std::string &name, const std::string &raw_card) {
  this->last_error_.clear();
  int idx = this->find_by_name_(name);
  if (idx < 0)
    return this->fail_("No user named '" + trim(name) + "'");
  std::string card = normalize_card(trim(raw_card));
  if (!trim(raw_card).empty() && (card.empty() || card.size() > CARD_HEX_LEN))
    return this->fail_("Card must be hex, up to 32 characters");
  int owner = card.empty() ? -1 : this->find_by_card_(card);
  if (owner >= 0 && owner != idx)
    return this->fail_(std::string("Card already assigned to ") + this->users_[owner].name);
  memset(this->users_[idx].card, 0, sizeof(this->users_[idx].card));
  strncpy(this->users_[idx].card, card.c_str(), CARD_HEX_LEN);
  this->save_(idx);
  ESP_LOGI(TAG, "%s card for '%s'", card.empty() ? "Cleared" : "Set", this->users_[idx].name);
  return true;
}

bool DoorAccess::set_pin(const std::string &name, const std::string &raw_pin) {
  this->last_error_.clear();
  int idx = this->find_by_name_(name);
  if (idx < 0)
    return this->fail_("No user named '" + trim(name) + "'");
  std::string pin = trim(raw_pin);
  StoredUser &u = this->users_[idx];
  if (pin.empty()) {
    u.flags &= ~FLAG_HAS_PIN;
    memset(u.pin_hash, 0, PIN_HASH_LEN);
  } else {
    if (!valid_pin(pin))
      return this->fail_("PIN must be 4-8 digits");
    uint8_t hash[PIN_HASH_LEN];
    this->hash_pin_(pin, hash);
    if (this->pin_in_use_(hash, idx, false, true))
      return this->fail_("That PIN is already in use");
    memcpy(u.pin_hash, hash, PIN_HASH_LEN);
    u.flags |= FLAG_HAS_PIN;
  }
  this->save_(idx);
  ESP_LOGI(TAG, "%s PIN for '%s'", pin.empty() ? "Cleared" : "Set", u.name);
  return true;
}

bool DoorAccess::set_duress_pin(const std::string &name, const std::string &raw_pin) {
  this->last_error_.clear();
  int idx = this->find_by_name_(name);
  if (idx < 0)
    return this->fail_("No user named '" + trim(name) + "'");
  std::string pin = trim(raw_pin);
  StoredUser &u = this->users_[idx];
  if (pin.empty()) {
    u.flags &= ~FLAG_HAS_DURESS;
    memset(u.duress_hash, 0, PIN_HASH_LEN);
  } else {
    if (!valid_pin(pin))
      return this->fail_("PIN must be 4-8 digits");
    uint8_t hash[PIN_HASH_LEN];
    this->hash_pin_(pin, hash);
    if (this->pin_in_use_(hash, idx, true, false))
      return this->fail_("That PIN is already in use");
    memcpy(u.duress_hash, hash, PIN_HASH_LEN);
    u.flags |= FLAG_HAS_DURESS;
  }
  this->save_(idx);
  ESP_LOGI(TAG, "%s duress PIN for '%s'", pin.empty() ? "Cleared" : "Set", u.name);
  return true;
}

bool DoorAccess::set_schedule(const std::string &name, const std::string &days, const std::string &start,
                              const std::string &end) {
  this->last_error_.clear();
  int idx = this->find_by_name_(name);
  if (idx < 0)
    return this->fail_("No user named '" + trim(name) + "'");
  StoredUser &u = this->users_[idx];
  if (trim(days).empty() && trim(start).empty() && trim(end).empty()) {
    u.flags &= ~FLAG_HAS_SCHEDULE;
    u.days = 0;
    u.start_min = u.end_min = 0;
    this->save_(idx);
    ESP_LOGI(TAG, "Cleared schedule for '%s' (any time)", u.name);
    return true;
  }
  uint8_t mask;
  if (!parse_days(days, &mask))
    return this->fail_("Days: e.g. 'mon,tue', 'mon-fri', 'weekdays', 'weekends' or 'all'");
  uint16_t s = 0, e = 0;
  if (!trim(start).empty() || !trim(end).empty()) {
    if (!parse_hhmm(trim(start), &s) || !parse_hhmm(trim(end), &e))
      return this->fail_("Times must be HH:MM, e.g. 08:00 and 12:30");
  }
  u.days = mask;
  u.start_min = s;
  u.end_min = e;
  u.flags |= FLAG_HAS_SCHEDULE;
  this->save_(idx);
  ESP_LOGI(TAG, "Schedule for '%s': %s %s", u.name, days_to_string(mask).c_str(),
           s == e ? "all day" : (hhmm(s) + "-" + hhmm(e)).c_str());
  return true;
}

bool DoorAccess::start_enrol(const std::string &raw_name, uint32_t timeout_ms) {
  this->last_error_.clear();
  std::string name = trim(raw_name);
  if (name.empty() || name.size() > USER_NAME_LEN)
    return this->fail_("Enter a name (1-24 characters) before enrolling");
  if (this->find_by_name_(name) < 0 && this->find_free_slot_() < 0)
    return this->fail_("User list is full");
  this->enrol_name_ = name;
  this->state_ = State::ENROL;
  this->pending_idx_ = -1;
  this->entry_.clear();
  this->deadline_ms_ = millis() + timeout_ms;
  ESP_LOGI(TAG, "Enrolment: badge the card for '%s' within %u s", name.c_str(), (unsigned) (timeout_ms / 1000));
  this->fb_enrol_(timeout_ms);
  return true;
}

void DoorAccess::cancel_enrol() {
  if (this->state_ != State::ENROL)
    return;
  this->state_ = State::IDLE;
  for (auto *t : this->enrol_failed_triggers_)
    t->trigger(this->enrol_name_, "cancelled");
  this->show_idle_();
}

size_t DoorAccess::user_count() const {
  return std::count_if(this->users_.begin(), this->users_.end(),
                       [](const StoredUser &u) { return u.magic == USER_MAGIC; });
}

bool DoorAccess::is_master(const std::string &name) const {
  int idx = this->find_by_name_(name);
  return idx >= 0 && (this->users_[idx].flags & FLAG_MASTER);
}

void DoorAccess::users_to_json(JsonObject root) const {
  JsonArray arr = root["users"].to<JsonArray>();
  for (const auto &u : this->users_) {
    if (u.magic != USER_MAGIC)
      continue;
    JsonObject o = arr.add<JsonObject>();
    o["name"] = u.name;
    o["card"] = u.card;
    o["pin"] = (u.flags & FLAG_HAS_PIN) != 0;
    o["duress_pin"] = (u.flags & FLAG_HAS_DURESS) != 0;
    o["mode"] = (u.flags & FLAG_CARD_AND_PIN) ? "card_and_pin" : "card_or_pin";
    o["master"] = (u.flags & FLAG_MASTER) != 0;
    if (u.flags & FLAG_HAS_SCHEDULE) {
      o["schedule"] = days_to_string(u.days) + " " +
                      (u.start_min == u.end_min ? std::string("all day") : hhmm(u.start_min) + "-" + hhmm(u.end_min));
    } else {
      o["schedule"] = "any time";
    }
  }
  root["count"] = this->user_count();
  root["lockdown"] = this->lockdown_;
}

// ------------------------------------------------------------ feedback ---

void DoorAccess::show_idle_() {
  if (this->reader_ == nullptr)
    return;
  this->reader_->set_led(this->lockdown_ ? this->lockdown_color_ : this->idle_color_, 0, 500, 0, 0, true);
  if (this->state_ == State::ENROL) {
    int32_t left = (int32_t) (this->deadline_ms_ - millis());
    if (left > 0)
      this->fb_enrol_(left);
  }
}

void DoorAccess::fb_grant_() {
  if (this->reader_ == nullptr)
    return;
  this->reader_->set_led(GREEN, 0, 500, 0, this->unlock_ms_, false);
  this->reader_->beep(1, 300, 0);
}

void DoorAccess::fb_deny_() {
  if (this->reader_ == nullptr)
    return;
  this->reader_->set_led(RED, 0, 200, 200, 2000, false);
  this->reader_->beep(3, 100, 100);
}

void DoorAccess::fb_prompt_pin_() {
  if (this->reader_ == nullptr)
    return;
  this->reader_->set_led(AMBER, 0, 300, 300, this->card_pin_timeout_ms_, false);
  this->reader_->beep(1, 100, 0);
}

void DoorAccess::fb_ack_() {
  if (this->reader_ == nullptr)
    return;
  this->reader_->set_led(GREEN, 0, 100, 100, 1000, false);
  this->reader_->beep(2, 100, 100);
}

void DoorAccess::fb_enrol_(uint32_t ms) {
  if (this->reader_ == nullptr)
    return;
  this->reader_->set_led(BLUE, AMBER, 500, 500, ms, false);
}

}  // namespace esphome::door_access
