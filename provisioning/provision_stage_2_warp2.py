#!/usr/bin/python3 -u

import tinkerforge_util as tfutil

tfutil.create_parent_module(__file__, 'provisioning')

import contextlib
from contextlib import contextmanager
import datetime
import getpass
import io
import json
import os
import re
import secrets
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import csv
import traceback

from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC

from provisioning.tinkerforge.ip_connection import IPConnection, base58encode, base58decode, BASE58
from provisioning.tinkerforge.bricklet_rgb_led_v2 import BrickletRGBLEDV2
from provisioning.tinkerforge.bricklet_nfc import BrickletNFC, SimpleGetTagID
from provisioning.tinkerforge.bricklet_evse_v2 import BrickletEVSEV2

from provisioning.provision_common.provision_common import *
from provisioning.provision_stage_3_warp2 import Stage3

evse = None
power_off_on_error = True
generation = None

def run_bricklet_tests(ipcon, result, scanner, ssid, stage3):
    global evse
    global generation

    enumerations = enumerate_devices(ipcon)

    master = next((e for e in enumerations if e.device_identifier == 13), None)
    evse_enum = next((e for e in enumerations if e.device_identifier == BrickletEVSEV2.DEVICE_IDENTIFIER), None)
    nfc_enum = next((e for e in enumerations if e.device_identifier == BrickletNFC.DEVICE_IDENTIFIER), None)

    if evse_enum is None:
        fatal_error("No EVSE Bricklet found!")

    # Don't check len(enumeraions) if this is a basic:
    # In this case we are connected to localhost and there's a lot of bricks and bricklets connected to the test PC

    if scanner.qr_variant != "B":
        if nfc_enum is None:
            fatal_error("No NFC Bricklet found!")
        if len(enumerations) != 2:
            fatal_error("Unexpected number of devices! Expected 2 but got {}.".format(len(enumerations)))

    is_basic = master is not None

    evse = BrickletEVSEV2(evse_enum.uid, ipcon)
    jumper_config, has_lock_switch, evse_version, energy_meter_type = evse.get_hardware_configuration()

    is_smart = not is_basic and energy_meter_type == 0
    is_pro = not is_basic and energy_meter_type != 0

    if is_smart or is_pro:
        host = ssid + ".local"

    automatic = scanner.qr_stand == '0' or scanner.qr_stand_wiring == '0'

    stage3.test_front_panel_button(automatic)
    result["front_panel_button_tested"] = True

    if generation >= 3:
        def manual_check_color(color):
            stage3.beep_notify()

            input_result = "n"

            while input_result := input(green(f'Is front LED {color}? [y/n]')) not in ["y", "n"]:
                pass

            if input_result == "n":
                fatal_error(f"Front LED not {color}!")

        evse.set_indicator_led(255, 60000, 0, 255, 255)
        time.sleep(0.5)
        if automatic and not stage3.is_front_panel_led_red():
            fatal_error("Front LED not red!")
        elif not automatic:
            manual_check_color('red')

        evse.set_indicator_led(255, 60000, 120, 255, 255)
        time.sleep(0.5)
        if automatic and not stage3.is_front_panel_led_green():
            fatal_error("Front LED not green!")
        elif not automatic:
            manual_check_color('green')

        evse.set_indicator_led(255, 60000, 240, 255, 255)
        time.sleep(0.5)
        if automatic and not stage3.is_front_panel_led_blue():
            fatal_error("Front LED not blue!")
        elif not automatic:
            manual_check_color('blue')

        evse.set_indicator_led(255, 60000, 0, 0, 255)
        time.sleep(0.5)
        if automatic and not stage3.is_front_panel_led_white():
            fatal_error("Front LED not white!")
        elif not automatic:
            manual_check_color('white')

        evse.set_indicator_led(-1, 1000, 0, 0, 0)

        result["front_panel_led_tested"] = True

    seen_tags = []
    if is_smart or is_pro:
        if scanner.qr_stand != '0' and scanner.qr_stand_wiring != '0':
            def download_seen_tags():
                with urllib.request.urlopen('http://{}/nfc/seen_tags'.format(host), timeout=3) as f:
                    nfc_str = f.read()

                local_seen_tags = []

                for tag_info in json.loads(nfc_str):
                    if len(tag_info['tag_id']) == 0:
                        continue

                    local_seen_tags.append(SimpleGetTagID(tag_info['tag_type'], [int(x, base=16) for x in tag_info['tag_id'].split(':')], tag_info['last_seen']))

                return local_seen_tags

            seen_tags = collect_nfc_tag_ids(stage3, download_seen_tags, True)
        else:
            with urllib.request.urlopen('http://{}/nfc/seen_tags'.format(host), timeout=3) as f:
                nfc_str = f.read()

            nfc_data = json.loads(nfc_str)

            if nfc_data[0]['tag_type'] != 2 or \
               nfc_data[0]['tag_id'] != "04:BA:38:42:EF:6C:80" or \
               nfc_data[0]['last_seen'] > 100:
                fatal_error("Did not find NFC tag: {}".format(nfc_str))

    d = {"P": "Pro", "S": "Smart", "B": "Basic"}

    if is_basic and scanner.qr_variant != "B":
        fatal_error("Scanned QR code implies variant {}, but detected was Basic (i.e. an Master Brick was found)".format(d[scanner.qr_variant]))

    if is_smart and scanner.qr_variant != "S":
        fatal_error("Scanned QR code implies variant {}, but detected was Smart: An ESP32 Brick was found, but no energy meter. Is the meter not connected or the display not lighting up? Is the QR code correct?".format(d[scanner.qr_variant]))

    if is_pro and scanner.qr_variant != "P":
        fatal_error("Scanned QR code implies variant {}, but detected was Pro: An ESP32 Brick and an energy meter was found. Is the QR code correct?".format(d[scanner.qr_variant]))

    result["evse_version"] = evse_version
    print("EVSE version is {}".format(evse_version))

    result["evse_uid"] = evse_enum.uid
    print("EVSE UID is {}".format(evse_enum.uid))

    if is_basic:
        result["master_uid"] = master.uid
        print("Master UID is {}".format(master.uid))

    if scanner.qr_power == "11" and jumper_config != 3:
        fatal_error("Wrong jumper config detected: {} but expected {} as the configured power is {} kW.".format(jumper_config, 3, scanner.qr_power))

    if scanner.qr_power == "22" and jumper_config != 6:
        fatal_error("Wrong jumper config detected: {} but expected {} as the configured power is {} kW.".format(jumper_config, 6, scanner.qr_power))

    result["jumper_config_checked"] = True
    if has_lock_switch:
        fatal_error("Wallbox has lock switch. Is the diode missing?")

    result["diode_checked"] = True

    outgoing = evse.get_charging_slot(1).max_current
    if scanner.qr_power == "11" and outgoing != 20000:
        fatal_error("Wrong type 2 cable config detected: Allowed current is {} but expected 20 A, as this is a 11 kW box.".format(outgoing / 1000))
    if scanner.qr_power == "22" and outgoing != 32000:
        fatal_error("Wrong type 2 cable config detected: Allowed current is {} but expected 32 A, as this is a 22 kW box.".format(outgoing / 1000))

    result["resistor_checked"] = True

    if is_pro:
        meter_str = urllib.request.urlopen('http://{}/meter/live'.format(host), timeout=3).read()
        meter_data = json.loads(meter_str)
        sps = meter_data["samples_per_second"]
        samples = meter_data["samples"]
        # if not 0.2 < sps < 2.5:
        #     fatal_error("Expected between 0.2 and 2.5 energy meter samples per second, but got {}".format(sps))
        if len(samples) < 2:
            fatal_error("Expected at least 2 samples but got {}".format(len(samples)))

        for i in range(3):
            error_count = evse.get_energy_meter_errors()
            if any(x != 0 for x in error_count):
                # Allow exactly one timeout in the third attempt. This can happen for some reason, but one timeout is "still fine™".
                if i == 2 and (any(x != 0 for x in error_count[1:]) or error_count[0] > 1):
                    fatal_error("Energy meter error count is {}, expected only zeros or at most one timeout (first member)! ".format(error_count) + blink("Complain to Erik!"))
                else:
                    print(".")
            else:
                break
            time.sleep(3)

        result["energy_meter_reachable"] = True

        meter_str = urllib.request.urlopen('http://{}/meter/values'.format(host), timeout=3).read()
        meter_data = json.loads(meter_str)
        if meter_data["energy_abs"] >= 1:
            stage3.beep_notify()
            while my_input(f'Energy meter reports {meter_data["energy_abs"]:.3f} kWh. Only < 1 kWh is allowed. Check if this is okay and press y + return to continue') != "y":
                pass

    return seen_tags

def exists_evse_test_report(evse_uid):
    global generation
    evse_version = {2: 2, 3: 3}[generation]
    with open(os.path.join(f"evse_v{evse_version}_test_report", "full_test_log.csv"), newline='') as csvfile:
        reader = csv.reader(csvfile, delimiter=',')
        for row in reader:
            if row[0] == evse_uid:
                return True
    return False

def retry_wrapper(fn, s):
    for i in range(3):
        try:
            return fn()
        except Exception as e:
            print("Failed to {}. ".format(s), end='')
            if i == 2:
                print("(3/3) Giving up.")
                raise e
            print("({}/3). Retrying in 3 seconds.".format(i + 1))
            time.sleep(3)

def is_front_panel_button_pressed():
    global evse
    global generation
    assert generation in (2, 3)
    return retry_wrapper(lambda: evse.get_low_level_state().gpio[{2: 6, 3: 5}[generation]], "check if front panel button is pressed")

def get_iec_state():
    global evse
    return retry_wrapper(lambda: chr(ord('A') + evse.get_state().iec61851_state), "get IEC state")

def reset_dc_fault():
    global evse
    return retry_wrapper(lambda: evse.reset_dc_fault_current_state(0xDC42FA23), "reset DC fault current state")

def has_evse_error():
    global evse
    return retry_wrapper(lambda: evse.get_state().error_state != 0, "get EVSE error state")

def switch_phases(phases):
    global evse
    return retry_wrapper(lambda: evse.set_phase_control(phases), "set phases")

def get_cp_pwm():
    global evse
    return retry_wrapper(lambda: evse.get_low_level_state().cp_pwm_duty_cycle, "get cp pwm")

def get_evse_uptime():
    global evse
    return retry_wrapper(lambda: evse.get_low_level_state().uptime, "get EVSE uptime")

def reset_evse():
    global evse
    return retry_wrapper(lambda: evse.reset(), "reset EVSE")

class Scanner:
    def __init__(self):
        # T:WARP2-CP-22KW-50;V:2.1;S:5000000001;B:2021-09;A:0;;;
        pattern = r'^T:WARP(2|3)-C(B|S|P)-(11|22)KW-(50|75|CC)(?:-PC)?;V:(\d+\.\d+);S:(5\d{9});B:(\d{4}-\d{2})(?:;A:(0|1))?;;;*$'
        self.qr_charger_code = my_input("Scan the charger QR code:")
        m = re.match(pattern, self.qr_charger_code)

        while not m:
            self.qr_charger_code = my_input("Scan the charger QR code:", red)
            m = re.match(pattern, self.qr_charger_code)

        self.qr_gen = m.group(1)
        self.qr_variant = m.group(2)
        self.qr_power = m.group(3)
        self.qr_cable_len = m.group(4)
        self.qr_hw_version = m.group(5)
        self.qr_serial = m.group(6)
        self.qr_built = m.group(7)
        self.qr_accessories = m.group(8)

        if self.qr_accessories == None:
            self.qr_accessories = '0'

        print("Charger QR code data:")
        print("    WARP{} Charger {}".format(self.qr_gen, {"B": "Basic", "S": "Smart", "P": "Pro"}[self.qr_variant]))
        print("    {} kW".format(self.qr_power))

        if self.qr_cable_len == 'CC':
            print("    Custom Cable")
        else:
            print("    {:1.1f} m".format(int(self.qr_cable_len) / 10.0))

        print("    HW Version: {}".format(self.qr_hw_version))
        print("    Serial: {}".format(self.qr_serial))
        print("    Build month: {}".format(self.qr_built))
        print("    Accessories: {}".format(self.qr_accessories))

        if self.qr_accessories == '0':
            self.qr_accessories_code = None
            self.qr_stand = '0'
            self.qr_stand_wiring = '0'
            self.qr_stand_lock = False
            self.qr_supply_cable = 0.0
            self.qr_cee = False
            self.qr_custom_front_panel = False
            self.qr_custom_type2_cable = False
        else:
            # S:1;W:1;E:2.5;C:1;CFP:1;CT2:1;;;
            pattern = r'^(?:S:(0|1|2|1-PC|2-PC);)?(?:W:(0|1|2);)?(?:L:(0|1);)?E:(\d+\.\d+);C:(0|1);(?:CFP:(0|1);)?(?:CT2:(0|1);)?;;*$'
            self.qr_accessories_code = my_input("Scan the accessories QR code:")
            m = re.match(pattern, self.qr_accessories_code)

            while not m:
                self.qr_accessories_code = my_input("Scan the accessories QR code:", red)
                m = re.match(pattern, self.qr_accessories_code)

            self.qr_stand = m.group(1) if m.group(1) != None else '0'
            self.qr_stand_wiring = m.group(2) if m.group(2) != None else '0'
            self.qr_stand_lock = bool(int(m.group(3) if m.group(3) != None else '0'))
            self.qr_supply_cable = float(m.group(4))
            self.qr_cee = bool(int(m.group(5)))
            self.qr_custom_front_panel = bool(int(m.group(6) if m.group(6) != None else '0'))
            self.qr_custom_type2_cable = bool(int(m.group(7) if m.group(7) != None else '0'))

            print("Accessories QR code data:")
            print("    Stand: {}".format(self.qr_stand))
            print("    Stand Wiring: {}".format(self.qr_stand_wiring))
            print("    Stand Lock: {}".format(self.qr_stand_lock))
            print("    Supply Cable: {} m".format(self.qr_supply_cable))
            print("    CEE: {}".format(self.qr_cee))
            print("    Custom Front Panel: {}".format(self.qr_custom_front_panel))
            print("    Custom Type 2 Cable: {}".format(self.qr_custom_type2_cable))

        if self.qr_variant != "B":
            pattern = r"^WIFI:S:(warp{gen})-([{BASE58}]{{3,6}});T:WPA;P:([{BASE58}]{{4}}-[{BASE58}]{{4}}-[{BASE58}]{{4}}-[{BASE58}]{{4}});;$".format(BASE58=BASE58, gen=self.qr_gen)
            self.qr_esp_code = getpass.getpass(green("Scan the ESP Brick QR code: "))
            m = re.match(pattern, self.qr_esp_code)

            while not m:
                self.qr_esp_code = getpass.getpass(red("Scan the ESP Brick QR code: "))
                m = re.match(pattern, self.qr_esp_code)

            self.qr_hardware_type = m.group(1)
            self.qr_esp_uid = m.group(2)
            self.qr_passphrase = m.group(3)

            print("ESP Brick QR code data:")
            print("    Hardware type: {}".format(self.qr_hardware_type))
            print("    UID: {}".format(self.qr_esp_uid))


def led_wrap():
    scanner = Scanner()
    stage3 = Stage3(is_front_panel_button_pressed_function=is_front_panel_button_pressed,
                    has_evse_error_function=has_evse_error,
                    get_iec_state_function=get_iec_state,
                    reset_dc_fault_function=reset_dc_fault,
                    switch_phases_function=switch_phases,
                    get_evse_uptime_function=get_evse_uptime,
                    reset_evse_function=reset_evse,
                    get_cp_pwm_function=get_cp_pwm)

    stage3.setup()
    stage3.set_led_strip_color((0, 0, 255))
    try:
        main(stage3, scanner)
    except BaseException:
        if power_off_on_error:
            stage3.power_off()
        stage3.set_led_strip_color((255, 0, 0))
        stage3.beep_failure()
        raise
    else:
        stage3.power_off()
        stage3.set_led_strip_color((0, 255, 0))
        stage3.beep_success()

blink_start = None
blink_count = 0
def start_blink(count):
    global blink_start, blink_count
    assert count in [3, 2, 1, 0], count
    blink_start = time.monotonic()
    blink_count = count

def blink_tick(stage3):
    diff = (time.monotonic() - blink_start)
    diff -= int(diff)
    color = (255, 127, 0) if diff <= 0.5 else (0, 0, 0)
    if blink_count >= 3:
        stage3.set_led_strip_color(color)
    elif blink_count == 2:
        stage3.set_led_strip_color((0, 0, 255), color)
    elif blink_count == 1:
        stage3.set_led_strip_color((0, 0, 255), (0, 0, 255), color)
    else:
        stage3.set_led_strip_color((0, 0, 255))

def stop_blink(stage3):
    global blink_start, blink_count
    blink_start = None
    blink_count = 0
    stage3.set_led_strip_color((0, 0, 255))

def handle_electrical_test_report(action, serial_number, timeout):
    timestamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')

    if serial_number != None:
        command = action + '_' + serial_number + '_' + timestamp
    else:
        command = action + '_' + timestamp

    command_responses = [command + '_success', command + '_failure']

    with open(os.path.join('electrical_test_automation', 'monitor', command), 'w') as f:
        pass

    start = time.monotonic()
    command_response = None

    while command_response == None and start + timeout > time.monotonic():
        for name in os.listdir(os.path.join('electrical_test_automation', 'monitor')):
            if name in command_responses:
                command_response = name
                break

    if command_response == None:
        fatal_error('Electrical test report {0} did not complete in time, please check manually'.format(action))

    try:
        os.remove(os.path.join('electrical_test_automation', 'monitor', command_response))
    except FileNotFoundError:
        pass

    if command_response.endswith('_failure'):
        fatal_error('Electrical test report {0} failed, please perform this step manually'.format(action))

class ContentTypeRemover(urllib.request.BaseHandler):
    def http_request(self, req):
        if req.has_header('Content-type'):
            req.remove_header('Content-type')
        return req
    https_request = http_request

def factory_reset(ssid):
    host = ssid + ".local"
    print("Triggering factory reset")
    print("Connecting via ethernet to {}".format(host), end="")
    for i in range(45):
        start = time.monotonic()
        try:
            req = urllib.request.Request(f"http://{host}/factory_reset",
                                         data=json.dumps({"do_i_know_what_i_am_doing": True}).encode("utf-8"),
                                         method='PUT',
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=1) as f:
                f.read()
                break
        except Exception as e:
            pass
        t = max(0, 1 - (time.monotonic() - start))
        time.sleep(t)
        print(".", end="")
    else:
        fatal_error("Failed to connect via ethernet!")
    print(" Connected.")
    print("Factory reset triggered.. Waiting 10 seconds")
    time.sleep(10)

def connect_to_ethernet(ssid, url):
    host = ssid + ".local"
    print("Connecting via ethernet to {} [{}]".format(host, url), end="")
    for i in range(45):
        start = time.monotonic()
        try:
            with urllib.request.urlopen(f"http://{host}/{url}", timeout=1) as f:
                result = f.read()
                break
        except:
            pass
        t = max(0, 1 - (time.monotonic() - start))
        time.sleep(t)
        print(".", end="")
    else:
        fatal_error("Failed to connect via ethernet!")
    print(" Connected.")
    return result, host

def collect_nfc_tag_ids(stage3, getter, beep_notify):
    print(green("Waiting for NFC tags"), end="")
    seen_tags = []
    last_len = 0
    start_blink(3)
    while len(seen_tags) < 3:
        seen_tags = [x for x in getter() if any(y != 0 for y in x.tag_id)]
        if len(seen_tags) != last_len:
            if beep_notify:
                stage3.beep_notify()
            if len(seen_tags) == 0:
                start_blink(3)
            elif len(seen_tags) == 1:
                start_blink(2)
            elif len(seen_tags) == 2:
                start_blink(1)
            else:
                start_blink(0)
            last_len = len(seen_tags)
        print(green("\rWaiting for NFC tags. {} seen".format(len(seen_tags))), end="")
        blink_tick(stage3)
        time.sleep(0.1)
    stop_blink(stage3)
    print("\r3 NFC tags seen." + " " * 20)
    return seen_tags

def main(stage3, scanner):
    result = {"start": now()}

    github_reachable = True
    try:
        with urllib.request.urlopen('https://github.com/Tinkerforge/firmwares', timeout=5.0) as req:
            req.read()
    except:
        print("github.com not reachable: Will not pull firmwares git.")
        github_reachable = False

    if github_reachable:
        with ChangedDirectory(os.path.join("..", "..", "firmwares")):
            run(["git", "pull"])

    result["serial"] = scanner.qr_serial
    result["qr_code"] = scanner.qr_charger_code

    if scanner.qr_accessories_code != None:
        result["accessories_qr_code"] = scanner.qr_accessories_code

    global generation
    assert scanner.qr_gen in ("2", "3")
    generation = int(scanner.qr_gen)

    evse_directory = os.path.join("..", "..", "firmwares", "bricklets", "evse_v2")
    evse_path = os.readlink(os.path.join(evse_directory, "bricklet_evse_v2_firmware_latest.zbin"))
    evse_path = os.path.join(evse_directory, evse_path)

    firmware_directory = os.path.join("..", "..", "firmwares", "bricks", f"warp{scanner.qr_gen}_charger")
    firmware_path = os.readlink(os.path.join(firmware_directory, f"brick_warp{scanner.qr_gen}_charger_firmware_latest.bin"))
    firmware_path = os.path.join(firmware_directory, firmware_path)

    if scanner.qr_variant != "B":
        if (scanner.qr_stand != '0' and scanner.qr_stand_wiring != '0') or scanner.qr_supply_cable != 0 or scanner.qr_cee:
            stage3.power_on('CEE')
        elif scanner.qr_gen == "3":
            stage3.power_on('Smart')
        else:
            stage3.power_on({"S": "Smart", "P": "Pro"}[scanner.qr_variant])

        if scanner.qr_stand == '0' or scanner.qr_stand_wiring == '0':
            seen_tags = collect_nfc_tag_ids(stage3, stage3.get_nfc_tag_ids, False)

        result["uid"] = scanner.qr_esp_uid
        ssid = scanner.qr_hardware_type + "-" + scanner.qr_esp_uid
        host = ssid + ".local"
        event_log = connect_to_ethernet(ssid, "event_log")[0].decode('utf-8')

        m = re.search(r"WARP{gen} (?:CHARGER|Charger) V(\d+).(\d+).(\d+)".format(gen=scanner.qr_gen), event_log)
        if not m:
            fatal_error("Failed to find version number in event log!" + event_log)

        version = [int(x) for x in m.groups()]
        latest_version = [int(x) for x in re.search(r"warp{gen}_charger_firmware_(\d+)_(\d+)_(\d+).bin".format(gen=scanner.qr_gen), firmware_path).groups()]

        if version > latest_version:
            fatal_error("Flashed firmware {}.{}.{} is not released yet! Latest released is {}.{}.{}".format(*version, *latest_version))
        elif version < latest_version:
            print("Flashed firmware {}.{}.{} is outdated! Flashing {}.{}.{}...".format(*version, *latest_version))

            with open(firmware_path, "rb") as f:
                fw = f.read()

            opener = urllib.request.build_opener(ContentTypeRemover())
            for i in range(5):
                try:
                    req = urllib.request.Request("http://{}/flash_firmware".format(host), fw)
                    print(opener.open(req).read().decode())
                    break
                except urllib.error.HTTPError as e:
                    print("HTTP error", e)
                    if e.code == 423:
                        fatal_error("Wallbox blocked firmware update. Is the EVSE working correctly?")
                    else:
                        fatal_error(e.read().decode("utf-8"))
                except urllib.error.URLError as e:
                    print("URL error", e)
                    if i != 4:
                        print("Failed to flash firmware. Retrying...")
                        time.sleep(3)
                    else:
                        if isinstance(e.reason, ConnectionResetError):
                            fatal_error("Wallbox blocked firmware update. Is the EVSE working correctly?")
                        fatal_error("Can't flash firmware!")

            time.sleep(3)
            connect_to_ethernet(ssid, "firmware_update/validate")
            factory_reset(ssid)
        else:
            print("Flashed firmware is up-to-date.")

        result["firmware"] = firmware_path.split("/")[-1]

        host = connect_to_ethernet(ssid, "hidden_proxy/enable")[1]

        ipcon = IPConnection()
        try:
            ipcon.connect(host, 4223)
        except Exception as e:
            fatal_error("Failed to connect to ESP proxy. Is the router's DHCP cache full?")

        seen_tags2 = run_bricklet_tests(ipcon, result, scanner, ssid, stage3)

        if scanner.qr_stand != '0' and scanner.qr_stand_wiring != '0':
            seen_tags = seen_tags2

        try:
            with urllib.request.urlopen("http://{}/users/config".format(host), timeout=5) as f:
                user_config = json.loads(f.read())
        except Exception as e:
            fatal_error("Failed to get users config: {} {}!".format(e, e.read()))

        do_factory_reset = False
        do_configure_users = True
        if len(user_config["users"]) != 4:
            do_factory_reset = len(user_config["users"]) != 1
        else:
            for i, u in enumerate(user_config["users"][1:]):
                if u["roles"] != 2 ** 16 - 1 or \
                   u["current"] != 32000 or \
                   u["display_name"] != "Benutzer {}".format(i + 1) or \
                   u["username"] != "user{}".format(i + 1) or \
                   u["digest_hash"] != "":
                    do_factory_reset = True
                    break
            else:
                do_configure_users = False

        if do_factory_reset:
            print("Invalid user configuration.")
            factory_reset(ssid)
            connect_to_ethernet(ssid, "hidden_proxy/enable")

        if not do_configure_users:
            print("Users already configured")
        else:
            print("Configuring users")
            for i in range(3):
                data = json.dumps({
                    "id":i+1,
                    "roles": 2**16-1,
                    "current": 32000,
                    "display_name": "Benutzer {}".format(i+1),
                    "username": "user{}".format(i+1),
                    "digest_hash": ""
                }).encode("utf-8")
                req = urllib.request.Request("http://{}/users/add".format(host),
                                             data=json.dumps({
                                                 "id": i + 1,
                                                 "roles": 2 ** 16 - 1,
                                                 "current": 32000,
                                                 "display_name": "Benutzer {}".format(i + 1),
                                                 "username": "user{}".format(i + 1),
                                                 "digest_hash": ""
                                             }).encode("utf-8"),
                                             method='PUT',
                                             headers={"Content-Type": "application/json"})
                try:
                    with urllib.request.urlopen(req, timeout=6) as f:
                        print(f.read())
                except Exception as e:
                    fatal_error("Failed to configure user {}: {} {}!".format(i, e, e.read()))

        print("Configuring tags")
        req = urllib.request.Request("http://{}/nfc/config_update".format(host),
                                     data=json.dumps({
                                         "deadtime_post_start": None,
                                         "authorized_tags": [
                                             {
                                                 "user_id": 1,
                                                 "tag_type": seen_tags[0].tag_type,
                                                 "tag_id": ":".join("{:02x}".format(i) for i in seen_tags[0].tag_id)
                                             }, {
                                                 "user_id": 2,
                                                 "tag_type": seen_tags[1].tag_type,
                                                 "tag_id": ":".join("{:02x}".format(i) for i in seen_tags[1].tag_id)
                                             }, {
                                                 "user_id": 3,
                                                 "tag_type": seen_tags[2].tag_type,
                                                 "tag_id": ":".join("{:02x}".format(i) for i in seen_tags[2].tag_id)
                                             }
                                         ]
                                     }).encode("utf-8"),
                                     method='PUT',
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=1) as f:
                f.read()
        except Exception as e:
            fatal_error("Failed to configure NFC tags! {} {}!".format(e, e.read()))
        result["nfc_tags_configured"] = True
    else:
        if (scanner.qr_stand != '0' and scanner.qr_stand_wiring != '0') or scanner.qr_supply_cable != 0 or scanner.qr_cee:
            stage3.power_on('CEE')
        else:
            stage3.power_on('Basic')

        result["uid"] = None

        ipcon = IPConnection()
        ipcon.connect("localhost", 4223)

        enumerations = enumerate_devices(ipcon)
        evse_enum = next((e for e in enumerations if e.device_identifier == 2167), None)

        if evse_enum is None:
            fatal_error("No EVSE Bricklet found!")

        print("Flashing EVSE")
        run(["python3", "comcu_flasher.py", evse_enum.uid, evse_path])
        result["evse_firmware"] = evse_path.split("/")[-1]

        run_bricklet_tests(ipcon, result, scanner, None, stage3)

    print("Checking if EVSE was tested...")
    if not exists_evse_test_report(result["evse_uid"]):
        print("No test report found for EVSE {}. Checking for new test reports...".format(result["evse_uid"]))
        with ChangedDirectory(os.path.join("..", "..", "wallbox")):
            run(["git", "pull"])
        if not exists_evse_test_report(result["evse_uid"]):
            fatal_error("Still no test report found for EVSE {}.".format(result["evse_uid"]))

    print("EVSE test report found")
    result["evse_test_report_found"] = True

    if scanner.qr_variant == "B":
        ssid = f'warp{scanner.qr_gen}-{result["evse_uid"]}'
        host = ssid + ".local"

    browser = None
    try:
        if scanner.qr_variant != "B":
            browser = webdriver.Firefox()
            browser.get("http://{}/#evse".format(host))

        print("Performing the electrical tests")
        stage3.test_wallbox(has_phase_switch=generation >= 3)
    finally:
        if browser is not None:
            browser.quit()

    print("Electrical tests passed")
    result["electrical_tests_passed"] = True

    if scanner.qr_variant != "B":
        print("Removing tracked charges")
        host = ssid + ".local"
        print("Connecting via ethernet to {}".format(host), end="")
        for i in range(45):
            start = time.monotonic()
            try:
                req = urllib.request.Request(f"http://{host}/charge_tracker/remove_all_charges",
                                             data=json.dumps({"do_i_know_what_i_am_doing":True}).encode("utf-8"),
                                             method='PUT',
                                             headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=1) as f:
                    f.read()
                    break
            except Exception as e:
                pass
            t = max(0, 1 - (time.monotonic() - start))
            time.sleep(t)
            print(".", end="")
        else:
            fatal_error("Failed to connect via ethernet!")
        print(" Connected.")
        print("Tracked charges removed.")
    result["end"] = now()

    with open("{}_{}_report_stage_2.json".format(ssid, now().replace(":", "-")), "w") as f:
        json.dump(result, f, indent=4)

    stage3.beep_notify()

    while my_input(f'Connect electrical tester to USB and press y + return to continue') != "y":
        pass

    print('Uploading empty electrical test report for next test')
    handle_electrical_test_report('upload', None, 180)

    print('Downloading complete electrical test report from this test')
    handle_electrical_test_report('download', scanner.qr_serial, 300)

    print('Done!')

if __name__ == "__main__":
    try:
        led_wrap()
        input('Press return to exit ')
    except FatalError:
        input('Press return to exit ')
        sys.exit(1)
    except Exception as e:
        traceback.print_exc()
        input('Press return to exit ')
        sys.exit(1)
