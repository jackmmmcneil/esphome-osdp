# esphome-osdp

ESPHome components that turn an ESP32 into an **OSDP access controller**: wire
an OSDP keypad/card reader to an ESP32 through a cheap RS-485 module and get
cards, PINs, two-factor, schedules, duress codes, lockdown and keypad commands,
all managed from Home Assistant and working standalone if Home Assistant or
Wi-Fi is down.

- **`osdp_cp`**: reader driver. Polls an OSDP reader (Open Supervised Device
  Protocol, IEC 60839-11-5) over RS-485, reports card reads and key presses,
  drives the reader LED and buzzer, and supports OSDP **Secure Channel**
  (AES-128), including provisioning a key onto a factory-fresh reader. Built on
  [LibOSDP](https://github.com/goToMain/libosdp), bundled in.
- **`door_access`**: access logic. Users with cards, PINs, duress PINs and
  schedules stored on the ESP (PINs salted and hashed), card+PIN two-factor,
  lockdown with master override, learn-mode card enrolment, `*` keypad
  commands, and Home Assistant actions and events for all of it.

Tested with an **AXIS A4120-E** reader on an ESP32 (esp32dev, ESP-IDF). Any
reader that speaks plain OSDP over RS-485 should work; reports for other
readers are welcome.

> **Not affiliated with or endorsed by Axis Communications.** AXIS is a
> trademark of Axis AB. This is a hobby project; read the
> [security notes](#security-notes) before using it on a door that matters.

## Contents

```
components/osdp_cp/        Reader driver (LibOSDP v3.2.7 vendored as libosdp_*)
components/door_access/    Access logic
examples/reader-test.yaml  Bench test: auto-detects the reader, raw bus log, LED/beep buttons
examples/reader-test-direct.yaml  Same, with no RS-485 module (ESP drives A/B; bench only)
examples/door.yaml         Full door controller with Home Assistant integration
examples/secrets.yaml.example
tests/                     Host test suite (run_tests.sh)
tools/vendor_libosdp.py    Re-vendors a newer LibOSDP
```

## Quick start

Requires ESPHome **2026.9.0 or newer**.

1. Wire it up (below).
2. Copy `examples/reader-test.yaml` and `examples/secrets.yaml.example` into
   your ESPHome config folder (rename the latter `secrets.yaml`, or merge it
   into yours). The examples pull the components straight from this repo:

   ```yaml
   external_components:
     - source: github://jackmmmcneil/esphome-osdp@main
       components: [osdp_cp, door_access]
   ```

3. Flash `reader-test.yaml` and watch the log. It scans every baud rate and
   address and logs `FOUND READER: address X at Y baud`, then `Reader online`;
   the reader LED goes amber. Badge a card and press some keys to check both.
4. Move to `examples/door.yaml`, set the `address` and uart `baud_rate` it
   found, flash, and add your first user from Home Assistant (below).

## Hardware

| ESP32 (esp32dev) | Goes to |
|---|---|
| GPIO17 (TX2) | RS-485 module DI (or TXD) |
| GPIO16 (RX2) | RS-485 module RO (or RXD); see level shifting below |
| GPIO4 | RS-485 module DE + RE, tied together. Used as the uart `flow_control_pin`, so the ESP32 switches direction in hardware. Omit for auto-direction modules |
| GPIO26 | Relay module IN, switching the strike or mag-lock supply |
| 5V / VIN | RS-485 module VCC (if it's a 5 V module) and ESP power from a 12 V→5 V buck |
| GND | RS-485 module GND **and** the reader's – |

The reader takes 12 V on its + and –. **One common ground** between the 12 V
supply, ESP32, RS-485 module and reader is essential. Never put 12 V on the
ESP or the module. RS-485 A/B go to the reader's A/B; if a scan reports bytes
but no valid reply, swap them.

**Level shifting:** a 3.3 V transceiver (MAX3485/SP3485) connects directly.
Common 5 V MAX485 boards drive RO to 5 V, so add a divider on RO, for example
1 kΩ in series and 2 kΩ to ground (any 1:2 ratio works), or a level-shifter
board. Their DI/DE/RE inputs accept the ESP's 3.3 V fine.

Mount the ESP32 and relay on the **secure side** of the door. Only the reader
goes outside.

### No RS-485 module yet?

`examples/reader-test-direct.yaml` sets `direct_drive`, which makes the ESP32
act as a crude transceiver: A is the UART TX pin, B is an inverted copy on a
second pin, and both are released while listening. Wire GPIO17 to A and
GPIO18 to B (220 Ω in each line), plus ESP GND to the reader's –. It's
outside the RS-485 spec and only for short bench leads.

## Secure Channel

Once plaintext works, generate a key (`openssl rand -hex 16`), put it in
`osdp_scbk` in `secrets.yaml`, and uncomment `scbk:` in the YAML. If the
reader accepts the default install key (SCBK-D), the ESP32 writes your key to
it on the next connection, and the **Reader secure channel** sensor turns on.
Keep the key safe: the reader only talks encrypted with it from then on. Add
`enforce_secure: true` once it's working so the ESP32 never falls back to
plaintext.

A reader previously paired with another controller in secure mode refuses
plaintext. Factory-reset it, or give `scbk` the key it was paired with.

## Access logic (door_access)

Credentials live on the ESP (NVS flash), so the door keeps working when Home
Assistant or Wi-Fi is down. PINs are stored as salted SHA-256 hashes and are
never sent back out.

### Managing users from Home Assistant

Developer Tools > Actions, all named `esphome.<device_name>_<action>` (e.g. `esphome.front_door_add_user`):

| Action | Fields |
|---|---|
| `add_user` | `name`, `mode` (`card_or_pin` or `card_and_pin`), `master` |
| `set_card` | `name`, `card` (hex from the "Last card" sensor; empty clears) |
| `set_pin` | `name`, `pin` (4-8 digits, unique; empty clears) |
| `set_duress_pin` | `name`, `pin` |
| `set_schedule` | `name`, `days` (`tue`, `mon-fri`, `weekdays`, `weekends`...), `start`, `end` (`08:00`); all empty = any time |
| `enrol_card` | `name`: the next card badged within 30 s is assigned (user created if new) |
| `remove_user` | `name` |
| `list_users` | returns JSON (tick "return response") |

Each action returns success or a readable error. Or use the dashboard:
type a name into **Enrol name**, press **Enrol card**, badge the card.

Example:

```yaml
action: esphome.front_door_add_user
data: {name: Cleaner, mode: card_or_pin, master: false}
---
action: esphome.front_door_set_pin
data: {name: Cleaner, pin: "7391"}
---
action: esphome.front_door_set_schedule
data: {name: Cleaner, days: tue, start: "08:00", end: "12:00"}
```

### At the keypad

| Entry | Effect |
|---|---|
| card | unlock (`card_or_pin` users) |
| card, then `PIN#` | unlock (`card_and_pin` users; reader flashes amber while it waits) |
| `PIN#` | unlock (`card_or_pin` users) |
| `PIN*code#` | command as that user, no unlock |
| `*code#` | command without a PIN |
| master `PIN*9#` | toggle lockdown |

- **Duress PIN**: opens the door exactly like the real PIN, and fires
  `esphome.door_duress` silently.
- **Lockdown** (switch or `*9`): only `master` users get in; reader solid red.
  The Home Assistant Unlock button still works.
- **Schedules** use Home Assistant time. If the clock hasn't synced, scheduled
  users are denied; masters and unscheduled users aren't affected. Overnight
  windows (`22:00`-`06:00`) work.
- Five wrong PINs lock the keypad for 60 s.

### Events for automations

| Event | Data |
|---|---|
| `esphome.door_access` | `result` (granted/denied), `user`, `method`, `reason`, `card` |
| `esphome.door_duress` | `user`, `method` |
| `esphome.door_command` | `user`, `code`, `authenticated` |
| `esphome.door_enrolled` | `user`, `card` |

Duress notification example:

```yaml
triggers:
  - trigger: event
    event_type: esphome.door_duress
actions:
  - action: notify.mobile_app_your_phone
    data:
      title: "DURESS: {{ trigger.event.data.door }}"
      message: "{{ trigger.event.data.user }} used their duress code"
      data: {push: {interruption-level: critical}}
```

Command example (`PIN*1#` arms the alarm):

```yaml
triggers:
  - trigger: event
    event_type: esphome.door_command
    event_data: {code: "1", authenticated: "true"}
actions:
  - action: alarm_control_panel.alarm_arm_away
    target: {entity_id: alarm_control_panel.home}
```

## Component reference

```yaml
osdp_cp:
  uart_id: osdp_bus
  address: 0                 # reader's OSDP address, 0-126
  scan: false                # true: find the reader's baud/address on boot
  direct_drive:              # ESP32 only, no transceiver (bench use)
    b_pin: GPIO18            # A is the uart tx_pin
  flow_control_pin: GPIO4    # software DE/RE; prefer the uart's flow_control_pin on ESP32
  scbk: !secret osdp_scbk    # optional, 32 hex chars
  enforce_secure: false
  idle_color: red            # LED colour while idle (door_access overrides this)
  online: {name: ...}        # binary sensors / text sensor, all optional
  secure_channel: {name: ...}
  last_card: {name: ...}
  on_card:                   # vars: card (hex string), bits (int)
    - then: ...
```

The component is a `key_provider`, so any `key_collector` can take keys from it.

Actions:

```yaml
- osdp_cp.led:
    id: reader
    color: green             # off red green amber blue magenta cyan white
    off_color: "off"
    on_time: 500ms           # flash on/off; off_time 0 = steady
    off_time: 0ms
    duration: 2s
    permanent: false         # true sets the idle state instead
- osdp_cp.buzzer:
    id: reader
    beeps: 3
    on_time: 100ms
    off_time: 100ms
```

```yaml
door_access:
  id: door
  reader_id: reader
  time_id: ha_time           # optional; needed for schedules
  max_users: 32              # 1-100, one NVS slot each
  unlock_time: 5s            # green LED duration
  pin_timeout: 5s            # clear a half-typed entry
  card_pin_timeout: 10s      # wait for the PIN after a card+PIN card
  max_pin_failures: 5
  lockout_time: 60s
  idle_color: blue
  lockdown_color: red
  on_granted:      # vars: user, method (card | pin | card+pin | remote)
  on_denied:       # vars: user, method, reason, card
  on_duress:       # vars: user, method
  on_command:      # vars: user, code, authenticated (bool)
  on_enrolled:     # vars: user, card
  on_enrol_failed: # vars: user, reason
```

Denial reasons: `unknown_card`, `unknown_pin`, `wrong_pin`, `card_required`,
`no_pin_set`, `pin_timeout`, `outside_schedule`, `no_time`, `lockdown`,
`locked_out`.

From lambdas: `add_user()`, `set_card()`, `set_pin()`, `set_duress_pin()`,
`set_schedule()`, `remove_user()`, `start_enrol()`, `cancel_enrol()`,
`set_lockdown()`, `remote_unlock()`, `is_master()`, `user_count()`,
`last_error()`. See `examples/door.yaml`.

## Security notes

- **Card IDs are not secrets.** Without vendor-specific credential support the
  reader reports each card's UID, which can be read and cloned. Use
  `card_and_pin` for anything that matters.
- **Turn on Secure Channel** with `enforce_secure: true`. Without it, anyone
  with access to the reader cable can sniff card IDs and PINs, or inject them.
- The ESP32, relay and RS-485 module belong on the secure side of the door.
  Anyone who can reach the relay wires can open the door.
- Protect the device: use API encryption (in the examples), a strong fallback
  AP password, and keep it on a trusted network.
- No warranty (see LICENSE). Check local regulations for egress, fail-safe
  locking and fire codes before fitting it to a real door.

## Tests

```
tests/run_tests.sh          # config validation + unit tests
tests/run_tests.sh --e2e    # plus: build examples/door.yaml for ESPHome's host
                            # platform and drive it over the native API
```

The unit tests run the real component code on Linux: the reader driver against
a simulated OSDP reader (plaintext, Secure Channel key provisioning, auto-scan),
and the access logic (credentials, card+PIN, duress, lockdown, schedules,
enrolment, lockout, persistence). CI runs everything on each push.

## Updating LibOSDP

```
git clone --recurse-submodules -b vX.Y.Z https://github.com/goToMain/libosdp
python3 tools/vendor_libosdp.py path/to/libosdp
```

## Licence

Apache-2.0. Bundled [LibOSDP](https://github.com/goToMain/libosdp) is
Apache-2.0, © Siddharth Chandrasekaran (see
`components/osdp_cp/LIBOSDP_LICENSE`).
