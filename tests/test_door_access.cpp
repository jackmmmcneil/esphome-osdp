// Host tests for the door_access component logic.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

#include "esphome/components/door_access/door_access.h"
#include "esphome/components/host/preferences.h"

using namespace esphome;
using namespace esphome::door_access;

static int fails = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("  FAIL line %d: %s\n", __LINE__, #cond);                  \
      fails++;                                                          \
    }                                                                   \
  } while (0)

struct Log {
  std::vector<std::string> ev;
  std::string last() const { return ev.empty() ? "" : ev.back(); }
  void clear() { ev.clear(); }
};
static Log L;

static void keys(DoorAccess &d, const char *s) {
  for (; *s; s++)
    d.handle_key((uint8_t) *s);
}

int main() {
  char dir[] = "/tmp/esphome-osdp-prefsXXXXXX";
  setenv("ESPHOME_PREFDIR", mkdtemp(dir), 1);
  host::setup_preferences();

  auto *d = new DoorAccess();
  d->set_max_users(8);
  d->set_max_pin_failures(3);
  d->set_lockout_time(60000);

  auto *g = new GrantedTrigger();
  auto *n = new DeniedTrigger();
  auto *du = new DuressTrigger();
  auto *c = new CommandTrigger();
  auto *en = new EnrolledTrigger();
  auto *ef = new EnrolFailedTrigger();
  // Hook triggers by wrapping Automation-less triggers: use small subclasses? Triggers need an
  // automation parent; attach Automations with a lambda action.
  d->register_granted_trigger(g);
  d->register_denied_trigger(n);
  d->register_duress_trigger(du);
  d->register_command_trigger(c);
  d->register_enrolled_trigger(en);
  d->register_enrol_failed_trigger(ef);

  struct GA : Action<std::string, std::string> {
    std::string tag;
    explicit GA(std::string t) : tag(std::move(t)) {}
    void play(const std::string &a, const std::string &b) override { L.ev.push_back(tag + ":" + a + ":" + b); }
  };
  struct DA : Action<std::string, std::string, std::string, std::string> {
    void play(const std::string &u, const std::string &m, const std::string &r, const std::string &card) override {
      L.ev.push_back("denied:" + u + ":" + m + ":" + r + (card.empty() ? "" : ":" + card));
    }
  };
  struct CA : Action<std::string, std::string, bool> {
    void play(const std::string &u, const std::string &code, const bool &auth) override {
      L.ev.push_back("cmd:" + u + ":" + code + ":" + (auth ? "auth" : "anon"));
    }
  };
  auto bind2 = [](Trigger<std::string, std::string> *t, const char *tag) {
    auto *a = new Automation<std::string, std::string>(t);
    a->add_actions({new GA(tag)});
  };
  bind2(g, "granted");
  bind2(du, "duress");
  bind2(en, "enrolled");
  bind2(ef, "enrol_failed");
  (new Automation<std::string, std::string, std::string, std::string>(n))->add_actions({new DA()});
  (new Automation<std::string, std::string, bool>(c))->add_actions({new CA()});

  d->setup();

  printf("== management\n");
  CHECK(d->add_user("Alex", "card_or_pin", true));
  CHECK(d->set_card("alex", "80:65:95:aa:2f:59:04"));
  CHECK(d->set_pin("Alex", "482916"));
  CHECK(d->set_duress_pin("Alex", "482917"));
  CHECK(d->add_user("Cleaner", "", false));
  CHECK(!d->set_pin("Cleaner", "482916"));  // duplicate PIN
  CHECK(d->last_error() == "That PIN is already in use");
  CHECK(!d->set_pin("Cleaner", "12a4"));
  CHECK(d->set_pin("Cleaner", "1111"));
  CHECK(d->add_user("Guest", "card_and_pin", false));
  CHECK(d->set_card("Guest", "0A1B2C3D"));
  CHECK(!d->set_card("Cleaner", "0a1b2c3d"));  // card taken
  CHECK(d->set_pin("Guest", "2222"));
  CHECK(!d->add_user("X", "sometimes", false));
  CHECK(!d->set_schedule("Cleaner", "funday", "08:00", "12:00"));
  CHECK(!d->set_schedule("Cleaner", "tue", "8am", "12:00"));
  CHECK(d->set_schedule("Cleaner", "tue", "08:00", "12:00"));
  CHECK(d->user_count() == 3);
  CHECK(d->is_master("ALEX") && !d->is_master("Guest"));

  printf("== card / pin\n");
  L.clear();
  d->handle_card("806595AA2F5904");
  CHECK(L.last() == "granted:Alex:card");
  keys(*d, "482916#");
  CHECK(L.last() == "granted:Alex:pin");
  d->handle_card("DEADBEEF");
  CHECK(L.last() == "denied::card:unknown_card:DEADBEEF");
  keys(*d, "2222#");  // card_and_pin user can't use PIN alone
  CHECK(L.last() == "denied:Guest:pin:card_required");

  printf("== card + pin\n");
  L.clear();
  d->handle_card("0a1b2c3d");
  CHECK(L.ev.empty());  // waiting for PIN
  keys(*d, "2222#");
  CHECK(L.last() == "granted:Guest:card+pin");
  d->handle_card("0A1B2C3D");
  keys(*d, "9999#");
  CHECK(L.last() == "denied:Guest:card+pin:wrong_pin");

  printf("== duress\n");
  L.clear();
  keys(*d, "482917#");
  CHECK(L.ev.size() == 2 && L.ev[0] == "duress:Alex:pin" && L.ev[1] == "granted:Alex:pin");

  printf("== commands\n");
  L.clear();
  keys(*d, "*0#");
  CHECK(L.last() == "cmd::0:anon");
  keys(*d, "482916*1#");
  CHECK(L.last() == "cmd:Alex:1:auth");
  keys(*d, "0000*1#");
  CHECK(L.last() == "denied::pin:unknown_pin");

  printf("== schedule (no clock -> scheduled users denied)\n");
  keys(*d, "1111#");
  CHECK(L.last() == "denied:Cleaner:pin:outside_schedule");
  {
    StoredUser u{};
    u.flags = FLAG_HAS_SCHEDULE;
    u.days = 0x02;  // Tuesday
    u.start_min = 8 * 60;
    u.end_min = 12 * 60;
    ESPTime t{};
    t.day_of_week = 3;  // Tuesday (1 = Sunday)
    t.hour = 9;
    t.minute = 30;
    CHECK(DoorAccess::schedule_allows(u, t));
    t.hour = 12;
    t.minute = 0;
    CHECK(!DoorAccess::schedule_allows(u, t));
    t.day_of_week = 4;
    t.hour = 9;
    CHECK(!DoorAccess::schedule_allows(u, t));
    // Overnight Fri 22:00 -> Sat 06:00
    u.days = 0x10;
    u.start_min = 22 * 60;
    u.end_min = 6 * 60;
    t.day_of_week = 6;  // Friday
    t.hour = 23;
    CHECK(DoorAccess::schedule_allows(u, t));
    t.day_of_week = 7;  // Saturday 05:00 belongs to Friday's window
    t.hour = 5;
    CHECK(DoorAccess::schedule_allows(u, t));
    t.hour = 7;
    CHECK(!DoorAccess::schedule_allows(u, t));
    t.day_of_week = 6;
    t.hour = 5;  // Friday 05:00 belongs to Thursday -> not allowed
    CHECK(!DoorAccess::schedule_allows(u, t));
  }

  printf("== lockdown\n");
  L.clear();
  d->set_lockdown(true);
  d->handle_card("0A1B2C3D");
  CHECK(L.last() == "denied:Guest:card:lockdown");
  d->handle_card("806595AA2F5904");  // master
  CHECK(L.last() == "granted:Alex:card");
  d->set_lockdown(false);

  printf("== enrol\n");
  L.clear();
  CHECK(d->start_enrol("Sam"));
  d->handle_card("11223344");
  CHECK(L.last() == "enrolled:Sam:11223344");
  d->handle_card("11223344");
  CHECK(L.last() == "granted:Sam:card");
  CHECK(d->start_enrol("Pat"));
  d->handle_card("0A1B2C3D");
  CHECK(L.last() == "enrol_failed:Pat:card already assigned to Guest");

  printf("== lockout\n");
  L.clear();
  keys(*d, "5555#");
  keys(*d, "5556#");
  keys(*d, "5557#");  // 3rd failure triggers lockout
  keys(*d, "482916#");
  CHECK(L.last() == "denied::pin:locked_out");

  printf("== persistence\n");
  global_preferences->sync();
  {
    auto *d2 = new DoorAccess();
    d2->set_max_users(8);
    d2->setup();
    CHECK(d2->user_count() == 4);
    CHECK(d2->is_master("alex"));
    CHECK(!d2->set_pin("Sam", "482916"));  // hashes survive reload (same salt)
    JsonDocument doc;
    d2->users_to_json(doc.to<JsonObject>());
    std::string out;
    serializeJson(doc, out);
    printf("  %s\n", out.c_str());
    CHECK(out.find("482916") == std::string::npos);  // PINs never exported
    CHECK(out.find("\"Cleaner\"") != std::string::npos && out.find("tue 08:00-12:00") != std::string::npos);
    CHECK(d2->remove_user("Sam") && d2->user_count() == 3);
  }

  printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
  return fails ? 1 : 0;
}
