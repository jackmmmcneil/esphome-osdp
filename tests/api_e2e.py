"""Drive the host-built door firmware through the real ESPHome native API,
the same way Home Assistant does."""

import asyncio
import json
import sys

from aioesphomeapi import APIClient

PASS = []
FAIL = []


def check(name, cond, detail=""):
    (PASS if cond else FAIL).append(name)
    print(("  ok   " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


async def main():
    cli = APIClient("127.0.0.1", 6053, None)
    await cli.connect(login=True)
    info = await cli.device_info()
    entities, services = await cli.list_entities_services()
    svc = {s.name: s for s in services}
    print("device:", info.name, "| actions:", sorted(svc))
    print("entities:", sorted(e.name for e in entities))

    async def call(_action, **data):
        resp = await cli.execute_service(svc[_action], data, return_response=True)
        return resp

    r = await call("add_user", name="Alex", mode="card_or_pin", master=True)
    check("add_user ok", r.success, r.error_message)
    r = await call("set_pin", name="Alex", pin="482916")
    check("set_pin ok", r.success, r.error_message)
    r = await call("add_user", name="Cleaner", mode="", master=False)
    r = await call("set_pin", name="Cleaner", pin="482916")
    check("duplicate PIN rejected", not r.success and "already in use" in r.error_message, r.error_message)
    r = await call("set_pin", name="Cleaner", pin="12")
    check("short PIN rejected", not r.success, r.error_message)
    r = await call("set_schedule", name="Cleaner", days="tue", start="08:00", end="12:00")
    check("set_schedule ok", r.success, r.error_message)
    r = await call("set_schedule", name="Cleaner", days="someday", start="", end="")
    check("bad schedule rejected", not r.success, r.error_message)
    r = await call("set_card", name="Alex", card="80:65:95:AA:2F:59:04")
    check("set_card ok", r.success, r.error_message)
    r = await call("set_card", name="Nobody", card="1234")
    check("unknown user rejected", not r.success and "No user" in r.error_message, r.error_message)
    r = await call("enrol_card", name="Sam")
    check("enrol_card starts", r.success, r.error_message)

    r = await call("list_users")
    data = json.loads(r.response_data) if r.response_data else {}
    print("  list_users ->", json.dumps(data))
    names = [u["name"] for u in data.get("users", [])]
    check("list_users returns users", names == ["Alex", "Cleaner"], str(names))
    check("PINs not exported", "482916" not in r.response_data.decode() if isinstance(r.response_data, bytes) else "482916" not in str(r.response_data))
    alex = next(u for u in data["users"] if u["name"] == "Alex")
    check("card normalised", alex["card"] == "806595AA2F5904", alex["card"])
    cleaner = next(u for u in data["users"] if u["name"] == "Cleaner")
    check("schedule reported", cleaner["schedule"] == "tue 08:00-12:00", cleaner["schedule"])

    r = await call("remove_user", name="cleaner")
    check("remove_user (case-insensitive)", r.success, r.error_message)
    r = await call("list_users")
    data = json.loads(r.response_data)
    check("count after remove", data["count"] == 1, str(data["count"]))

    await cli.disconnect()


asyncio.run(main())
print(f"\n{len(PASS)} passed, {len(FAIL)} failed")
sys.exit(1 if FAIL else 0)
