// Host loopback test: ESPHome osdp_cp component (CP) <-> LibOSDP PD in memory.
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <time.h>
#include <unistd.h>

#include "esphome/components/osdp_cp/osdp_cp.h"

using namespace esphome;
extern "C" int64_t osdp_millis_now(void);


static std::deque<uint8_t> to_pd, to_cp;

class FakeUart : public uart::UARTComponent {
 public:
  void write_array(const uint8_t *d, size_t n) override { to_pd.insert(to_pd.end(), d, d + n); }
  bool peek_byte(uint8_t *d) override {
    if (to_cp.empty()) return false;
    *d = to_cp.front();
    return true;
  }
  bool read_array(uint8_t *d, size_t n) override {
    if (to_cp.size() < n) return false;
    for (size_t i = 0; i < n; i++) { d[i] = to_cp.front(); to_cp.pop_front(); }
    return true;
  }
  size_t available() override { return to_cp.size(); }
  uart::UARTFlushResult flush() override { return uart::UARTFlushResult::UART_FLUSH_RESULT_ASSUMED_SUCCESS; }
  void check_logger_conflict() override {}
};


static int pd_send(void *, uint8_t *b, int n) { to_cp.insert(to_cp.end(), b, b + n); return n; }
static int pd_recv(void *, uint8_t *b, int max) {
  int i = 0;
  while (i < max && !to_pd.empty()) { b[i++] = to_pd.front(); to_pd.pop_front(); }
  return i;
}

static int leds = 0, buzzers = 0, keysets = 0;
static int pd_cmd_cb(void *, struct osdp_cmd *cmd) {
  if (cmd->id == OSDP_CMD_LED) {
    leds++;
    printf("  PD got LED: temp cc=%d color=%d timer=%d | perm cc=%d color=%d\n", cmd->led.temporary.control_code,
           cmd->led.temporary.on_color, cmd->led.temporary.timer_count, cmd->led.permanent.control_code,
           cmd->led.permanent.on_color);
  } else if (cmd->id == OSDP_CMD_BUZZER) {
    buzzers++;
    printf("  PD got BUZZER: reps=%d on=%d off=%d\n", cmd->buzzer.rep_count, cmd->buzzer.on_count,
           cmd->buzzer.off_count);
  } else if (cmd->id == OSDP_CMD_KEYSET) {
    keysets++;
    printf("  PD got KEYSET (new SCBK provisioned)\n");
  }
  return 0;
}

class Sink : public text_sensor::TextSensor {};

static bool run(bool secure, bool scan = false) {
  printf("=== %s%s ===\n", secure ? "secure channel (PD in install mode)" : "plaintext", scan ? " + scan" : "");
  to_pd.clear();
  to_cp.clear();
  leds = buzzers = keysets = 0;

  FakeUart uart;
  uart.set_baud_rate(9600);
  osdp_cp::OSDPControlPanel cp;
  cp.set_uart_parent(&uart);
  cp.set_address(scan ? 0 : 5);
  cp.set_scan(scan);
  if (secure) cp.set_scbk("000102030405060708090a0b0c0d0e0f");
  cp.set_idle_color(1);
  binary_sensor::BinarySensor online, sc;
  text_sensor::TextSensor last;
  cp.set_online_sensor(&online);
  cp.set_secure_channel_sensor(&sc);
  cp.set_last_card_sensor(&last);
  std::string keys;
  cp.add_on_key_callback([&keys](uint8_t k) { keys.push_back((char) k); });
  cp.setup();

  static struct osdp_pd_cap caps[] = {
      {OSDP_PD_CAP_READER_LED_CONTROL, 1, 1},
      {OSDP_PD_CAP_READER_AUDIBLE_OUTPUT, 1, 1},
      {OSDP_PD_CAP_CARD_DATA_FORMAT, 1, 0},
      {OSDP_PD_CAP_READER_TEXT_OUTPUT, 0, 0},
      {OSDP_PD_CAP_COMMUNICATION_SECURITY, 1, 0},
      {(uint8_t) -1, 0, 0},
  };
  osdp_pd_info_t pdi{};
  pdi.name = "fake-a4120";
  pdi.baud_rate = 9600;
  pdi.address = 5;
  pdi.flags = secure ? OSDP_FLAG_INSTALL_MODE : 0;
  pdi.id.vendor_code = 0xACCC8E;
  pdi.cap = caps;
  pdi.channel.recv = pd_recv;
  pdi.channel.send = pd_send;
  pdi.scbk = nullptr;
  osdp_t *pd = osdp_pd_setup(&pdi);
  if (!pd) { printf("PD setup failed\n"); return false; }
  osdp_pd_set_command_callback(pd, pd_cmd_cb, nullptr);

  bool sent_card = false, sent_keys = false, sent_cmds = false;
  int64_t t0 = osdp_millis_now();
  while (osdp_millis_now() - t0 < 15000) {
    cp.loop();
    osdp_pd_refresh(pd);
    usleep(2000);
    bool ready = cp.is_online() && (!secure || cp.is_secure()) && (!secure || keysets > 0);
    if (ready && !sent_card) {
      struct osdp_event ev;
      memset(&ev, 0, sizeof(ev));
      ev.type = OSDP_EVENT_CARDREAD;
      ev.cardread.format = OSDP_CARD_FMT_RAW_UNSPECIFIED;
      ev.cardread.length = 56;  // 7-byte UID
      uint8_t uid[] = {0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
      memcpy(ev.cardread.data, uid, sizeof(uid));
      osdp_pd_submit_event(pd, &ev);
      sent_card = true;
    }
    if (sent_card && !last.state.empty() && !sent_keys) {
      struct osdp_event ev;
      memset(&ev, 0, sizeof(ev));
      ev.type = OSDP_EVENT_KEYPRESS;
      const uint8_t k[] = {'1', '2', '3', '4', 0x7F, 0x0D};
      ev.keypress.length = sizeof(k);
      memcpy(ev.keypress.data, k, sizeof(k));
      osdp_pd_submit_event(pd, &ev);
      sent_keys = true;
    }
    if (sent_keys && keys.size() == 6 && !sent_cmds) {
      cp.set_led(2, 0, 500, 0, 5000, false);
      cp.beep(3, 100, 100);
      sent_cmds = true;
    }
    if (sent_cmds && buzzers > 0 && leds >= 2) break;
  }
  osdp_pd_teardown(pd);

  bool ok = online.state && last.state == "04A1B2C3D4E5F6" && keys == "1234*#" && leds >= 2 && buzzers == 1 &&
            (!secure || (sc.state && keysets == 1));
  printf("  online=%d sc=%d keyset=%d last_card=%s keys=%s leds=%d buzzers=%d -> %s\n", online.state, sc.state,
         keysets, last.state.c_str(), keys.c_str(), leds, buzzers, ok ? "PASS" : "FAIL");
  return ok;
}

int main() {
  bool a = run(false);
  bool b = run(true);
  bool c = run(false, true);
  return (a && b && c) ? 0 : 1;
}
