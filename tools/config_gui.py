#!/usr/bin/env python3
"""Configurator for the VESC BLE Helper (XIAO ESP32-C3).

Connects over BLE to the "VESC-BLE-Helper" device and lets you:
  * bind the BLE button (HID shutter remote) and the cadence sensor;
  * tune PAS parameters and CAN identifiers;
  * watch live status (cadence, button, throttle, assist current);
  * update the helper firmware over BLE (OTA);
  * upload a LISP script to the VESC through the NUS bridge (VESC Tool
    protocol).

Dependencies:  pip install bleak
Run:           python3 config_gui.py
"""

import asyncio
import hashlib
import queue
import struct
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "VESC-BLE-Helper"

CFG_CTRL   = "ab1e0002-b1e5-4e15-8ac3-5e00c0de15b7"
CFG_STATUS = "ab1e0003-b1e5-4e15-8ac3-5e00c0de15b7"
CFG_SCAN   = "ab1e0004-b1e5-4e15-8ac3-5e00c0de15b7"
OTA_CTRL   = "ab1e0005-b1e5-4e15-8ac3-5e00c0de15b7"
OTA_DATA   = "ab1e0006-b1e5-4e15-8ac3-5e00c0de15b7"
NUS_RX     = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write → helper → CAN
NUS_TX     = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify ← VESC

# Config-service CTRL commands
CMD_SCAN, CMD_BIND_BUTTON, CMD_BIND_CADENCE, CMD_UNBIND, \
    CMD_GET_PARAMS, CMD_SET_PARAMS, CMD_SET_THROTTLE, \
    CMD_SET_BINDING, CMD_GET_BINDING = range(1, 10)
WHAT_BUTTON, WHAT_CADENCE = 1, 2

# OTA
OTA_OP_BEGIN, OTA_OP_END, OTA_OP_ABORT = 1, 2, 3
OTA_ST_READY, OTA_ST_PROGRESS, OTA_ST_DONE, OTA_ST_ERROR = 0x10, 0x11, 0x12, 0x1F

# VESC COMM ids (for the LISP upload over NUS)
COMM_LISP_WRITE_CODE  = 131
COMM_LISP_ERASE_CODE  = 132
COMM_LISP_SET_RUNNING = 133

PARAMS_FMT = "<7B4H2I2B8BH"  # ver..start_pct, delays/rpms, currents, ids,
                             # btn actions, can_kbps
PARAMS_LEN = struct.calcsize(PARAMS_FMT)
PARAMS_VER = 3
CAN_SPEEDS = ("125", "250", "500", "1000")
STATUS_FMT = "<BBhBBiBB"    # ver, flags, rpm, batt, level, assist, btn mask, btn count
STATUS_LEN = struct.calcsize(STATUS_FMT)

# btn_action_t (settings.h) — the GUI uses only BTN_ACT_CUSTOM_CAN: every
# button fires a configurable raw CAN frame; the semantics live in the LISP
# script on the VESC (event-can-sid, see lisp/main.lisp for the example).
BTN_ACT_CUSTOM_CAN = 4
BTN_UI_SLOTS = 8   # buttons A..H, learned across all bound remotes


def crc16(data: bytes) -> int:
    """CRC16-CCITT (XMODEM), same as VESC's crc.c."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def vesc_frame(payload: bytes) -> bytes:
    c = crc16(payload)
    if len(payload) <= 255:
        return bytes([2, len(payload)]) + payload + struct.pack(">H", c) + b"\x03"
    return bytes([3, len(payload) >> 8, len(payload) & 0xFF]) + payload + \
        struct.pack(">H", c) + b"\x03"


class VescStreamParser:
    """Parses VESC frames out of the NUS notification byte stream."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        self.buf += data
        out = []
        while True:
            pkt = self._try_parse()
            if pkt is None:
                break
            out.append(pkt)
        return out

    def _try_parse(self):
        b = self.buf
        while b and b[0] not in (2, 3):
            b.pop(0)
        if len(b) < 2:
            return None
        if b[0] == 2:
            need_hdr, ln = 2, b[1]
        else:
            if len(b) < 3:
                return None
            need_hdr, ln = 3, (b[1] << 8) | b[2]
        total = need_hdr + ln + 3
        if len(b) < total:
            return None
        payload = bytes(b[need_hdr:need_hdr + ln])
        crc = (b[need_hdr + ln] << 8) | b[need_hdr + ln + 1]
        ok = crc == crc16(payload) and b[total - 1] == 3
        del b[:total]
        return payload if ok else self._try_parse()


class BleWorker:
    """Async BLE client on a background thread; events go to the GUI queue."""

    def __init__(self, events: queue.Queue):
        self.events = events
        self.loop = asyncio.new_event_loop()
        self.client = None
        self.nus_parser = VescStreamParser()
        self.nus_packets = None   # asyncio.Queue, created inside the loop
        self.ota_status = None
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.nus_packets = asyncio.Queue()
        self.ota_status = asyncio.Queue()
        self.loop.run_forever()

    def submit(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self.loop)

    def emit(self, kind, **kw):
        self.events.put((kind, kw))

    # ---------- connection ----------

    async def _connect(self):
        self.emit("log", text="Searching for %s…" % DEVICE_NAME)
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)
        if dev is None:
            self.emit("log", text="Device not found")
            self.emit("connected", ok=False)
            return
        client = BleakClient(dev, disconnected_callback=self._on_disconnect)
        await client.connect()
        self.client = client
        await client.start_notify(CFG_CTRL, self._on_ctrl)
        await client.start_notify(CFG_STATUS, self._on_status)
        await client.start_notify(CFG_SCAN, self._on_scan)
        await client.start_notify(OTA_CTRL, self._on_ota)
        await client.start_notify(NUS_TX, self._on_nus)
        self.emit("log", text="Connected: %s" % dev.address)
        self.emit("connected", ok=True)

    def _on_disconnect(self, _client):
        self.client = None
        self.emit("connected", ok=False)
        self.emit("log", text="Connection lost")

    # ---------- notifications ----------

    def _on_ctrl(self, _h, data: bytearray):
        if not data:
            return
        rsp = data[0]
        if rsp == 0x85 and len(data) >= 1 + PARAMS_LEN:
            self.emit("params", blob=bytes(data[1:1 + PARAMS_LEN]))
        elif rsp == 0x89 and len(data) >= 16:
            idx, ext, ln = data[1], data[2], min(data[3], 8)
            can_id = struct.unpack("<I", data[4:8])[0]
            self.emit("binding", idx=idx, ext=ext,
                      can_id=can_id, data=bytes(data[8:8 + ln]))
        else:
            ok = len(data) >= 2 and data[1] == 0
            self.emit("ack", cmd=rsp & 0x7F, ok=ok)

    def _on_status(self, _h, data: bytearray):
        if len(data) < STATUS_LEN:
            return
        ver, flags, centi_rpm, batt, level, assist_ma, btn_mask, btn_count = \
            struct.unpack(STATUS_FMT, bytes(data[:STATUS_LEN]))
        if ver != 2:
            return
        self.emit("status", flags=flags, rpm=centi_rpm / 100.0,
                  batt=batt, level=level, assist_a=assist_ma / 1000.0,
                  btn_mask=btn_mask, btn_count=btn_count)

    def _on_scan(self, _h, data: bytearray):
        if len(data) < 10:
            return
        what, addr_type = data[0], data[1]
        addr = bytes(data[2:8])
        rssi = struct.unpack("b", data[8:9])[0]
        nlen = data[9]
        name = bytes(data[10:10 + nlen]).decode("utf-8", "replace")
        mac = ":".join("%02X" % b for b in reversed(addr))
        self.emit("scan_hit", what=what, addr=addr, addr_type=addr_type,
                  mac=mac, name=name, rssi=rssi)

    def _on_ota(self, _h, data: bytearray):
        if len(data) >= 5:
            st, detail = data[0], struct.unpack("<I", data[1:5])[0]
            self.loop.call_soon(self.ota_status.put_nowait, (st, detail))

    def _on_nus(self, _h, data: bytearray):
        for pkt in self.nus_parser.feed(bytes(data)):
            self.loop.call_soon(self.nus_packets.put_nowait, pkt)

    # ---------- commands ----------

    async def _ctrl(self, payload: bytes):
        if self.client:
            await self.client.write_gatt_char(CFG_CTRL, payload, response=True)

    def connect(self):            self.submit(self._connect())
    def scan(self, what):         self.submit(self._ctrl(bytes([CMD_SCAN, what])))
    def unbind(self, what):       self.submit(self._ctrl(bytes([CMD_UNBIND, what])))
    def get_params(self):         self.submit(self._ctrl(bytes([CMD_GET_PARAMS])))
    def set_params(self, blob):   self.submit(self._ctrl(bytes([CMD_SET_PARAMS]) + blob))
    def throttle(self, v):        self.submit(self._ctrl(bytes([CMD_SET_THROTTLE, v])))
    def get_binding(self, idx):   self.submit(self._ctrl(bytes([CMD_GET_BINDING, idx])))

    def set_binding(self, idx, ext, data, can_id):
        payload = bytes([CMD_SET_BINDING, idx, ext, len(data)]) + \
            struct.pack("<I", can_id) + data.ljust(8, b"\x00")[:8]
        self.submit(self._ctrl(payload))

    def bind(self, what, addr, addr_type):
        cmd = CMD_BIND_BUTTON if what == WHAT_BUTTON else CMD_BIND_CADENCE
        self.submit(self._ctrl(bytes([cmd, addr_type]) + addr))

    # ---------- OTA ----------

    async def _ota(self, path):
        try:
            with open(path, "rb") as f:
                image = f.read()
            sha = hashlib.sha256(image).digest()
            self.emit("log", text="OTA: %d bytes, erasing partition…" % len(image))
            while not self.ota_status.empty():
                self.ota_status.get_nowait()
            await self.client.write_gatt_char(
                OTA_CTRL, bytes([OTA_OP_BEGIN]) +
                struct.pack("<I", len(image)) + sha, response=True)
            st, detail = await asyncio.wait_for(self.ota_status.get(), 30)
            if st != OTA_ST_READY:
                raise RuntimeError("BEGIN rejected: 0x%02X/%d" % (st, detail))
            self.emit("log", text="OTA: transferring…")
            chunk = 244
            for off in range(0, len(image), chunk):
                await self.client.write_gatt_char(
                    OTA_DATA, image[off:off + chunk], response=False)
                if off % (32 * chunk) == 0:
                    self.emit("ota_progress", done=off, total=len(image))
                    await asyncio.sleep(0)   # let notifications through
            self.emit("ota_progress", done=len(image), total=len(image))
            await self.client.write_gatt_char(
                OTA_CTRL, bytes([OTA_OP_END]), response=True)
            while True:
                st, detail = await asyncio.wait_for(self.ota_status.get(), 30)
                if st == OTA_ST_PROGRESS:
                    continue
                if st == OTA_ST_DONE:
                    self.emit("log", text="OTA done — device is rebooting")
                    break
                raise RuntimeError("OTA error: 0x%02X/%d" % (st, detail))
        except Exception as e:
            self.emit("log", text="OTA: %s" % e)
            try:
                await self.client.write_gatt_char(
                    OTA_CTRL, bytes([OTA_OP_ABORT]), response=True)
            except Exception:
                pass

    def ota(self, path):
        self.submit(self._ota(path))

    # ---------- LISP over NUS ----------

    async def _nus_cmd(self, payload: bytes, expect: int, timeout=8.0):
        """Send a VESC packet and wait for a reply with the same command byte."""
        await self.client.write_gatt_char(NUS_RX, vesc_frame(payload),
                                          response=False)
        deadline = asyncio.get_event_loop().time() + timeout
        while True:
            left = deadline - asyncio.get_event_loop().time()
            if left <= 0:
                raise RuntimeError("no reply to cmd %d" % payload[0])
            pkt = await asyncio.wait_for(self.nus_packets.get(), left)
            if pkt and pkt[0] == expect:
                return pkt

    async def _lisp_upload(self, path):
        try:
            with open(path, "rb") as f:
                code = f.read()
            # Blob layout, same as VESC Tool:
            #   [u32 packed-2][u16 crc16(packed)]
            #   [u16 flags=0][code][NUL][i16 imports=0]
            packed = b"\x00\x00" + code + b"\x00" + b"\x00\x00"
            blob = struct.pack(">I", len(packed) - 2) + \
                struct.pack(">H", crc16(packed)) + packed
            self.emit("log", text="LISP: stopping script…")
            await self._nus_cmd(bytes([COMM_LISP_SET_RUNNING, 0]),
                                COMM_LISP_SET_RUNNING)
            self.emit("log", text="LISP: erasing (%d bytes)…" % len(blob))
            await self._nus_cmd(
                bytes([COMM_LISP_ERASE_CODE]) +
                struct.pack(">i", len(blob) + 100),
                COMM_LISP_ERASE_CODE, timeout=15.0)
            chunk = 240
            for off in range(0, len(blob), chunk):
                part = blob[off:off + chunk]
                await self._nus_cmd(
                    bytes([COMM_LISP_WRITE_CODE]) + struct.pack(">I", off) +
                    part, COMM_LISP_WRITE_CODE)
                self.emit("ota_progress", done=off, total=len(blob))
            self.emit("ota_progress", done=len(blob), total=len(blob))
            self.emit("log", text="LISP: starting…")
            await self._nus_cmd(bytes([COMM_LISP_SET_RUNNING, 1]),
                                COMM_LISP_SET_RUNNING)
            self.emit("log", text="LISP: script uploaded and running")
        except Exception as e:
            self.emit("log", text="LISP: error — %s" % e)

    def lisp_upload(self, path):
        self.submit(self._lisp_upload(path))


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("VESC BLE Helper — Configurator")
        root.geometry("680x680")
        self.events = queue.Queue()
        self.ble = BleWorker(self.events)
        self.scan_rows = []      # (what, addr, addr_type)

        top = ttk.Frame(root); top.pack(fill="x", padx=8, pady=4)
        self.btn_connect = ttk.Button(top, text="Connect",
                                      command=self.ble.connect)
        self.btn_connect.pack(side="left")
        self.lbl_conn = ttk.Label(top, text="disconnected", foreground="red")
        self.lbl_conn.pack(side="left", padx=10)

        nb = ttk.Notebook(root); nb.pack(fill="both", expand=True, padx=8, pady=4)
        self._tab_bind(nb)
        self._tab_params(nb)
        self._tab_status(nb)
        self._tab_fw(nb)
        self._tab_lisp(nb)

        self.log = tk.Text(root, height=6, state="disabled")
        self.log.pack(fill="x", padx=8, pady=4)

        root.after(100, self._poll)

    # ---------- tabs ----------

    def _tab_bind(self, nb):
        f = ttk.Frame(nb); nb.add(f, text="Binding")
        row = ttk.Frame(f); row.pack(fill="x", pady=4)
        ttk.Button(row, text="Scan for button",
                   command=lambda: self._start_scan(WHAT_BUTTON)).pack(side="left", padx=4)
        ttk.Button(row, text="Scan for cadence sensor",
                   command=lambda: self._start_scan(WHAT_CADENCE)).pack(side="left", padx=4)
        ttk.Button(row, text="Unbind remotes (all)",
                   command=lambda: self.ble.unbind(WHAT_BUTTON)).pack(side="right", padx=4)
        ttk.Button(row, text="Unbind sensor",
                   command=lambda: self.ble.unbind(WHAT_CADENCE)).pack(side="right", padx=4)
        self.scan_list = ttk.Treeview(
            f, columns=("type", "mac", "name", "rssi"), show="headings", height=10)
        for col, txt, w in (("type", "Type", 80), ("mac", "MAC", 160),
                            ("name", "Name", 220), ("rssi", "RSSI", 60)):
            self.scan_list.heading(col, text=txt)
            self.scan_list.column(col, width=w)
        self.scan_list.pack(fill="both", expand=True, pady=4)
        ttk.Button(f, text="Bind selected",
                   command=self._bind_selected).pack(pady=4)

    def _tab_params(self, nb):
        f = ttk.Frame(nb); nb.add(f, text="Parameters")

        # CAN bus settings — their own frame so they don't get lost among
        # the PAS tuning fields.
        can = ttk.LabelFrame(f, text="CAN bus"); can.pack(fill="x",
                                                          padx=8, pady=6)
        ttk.Label(can, text="Helper ID").grid(row=0, column=0, padx=4, pady=4)
        self.ctrl_id = ttk.Entry(can, width=6)
        self.ctrl_id.grid(row=0, column=1, padx=4)
        ttk.Label(can, text="VESC ID").grid(row=0, column=2, padx=4)
        self.tgt_id = ttk.Entry(can, width=6)
        self.tgt_id.grid(row=0, column=3, padx=4)
        ttk.Label(can, text="Bitrate, kbps").grid(row=0, column=4, padx=4)
        self.can_kbps = ttk.Combobox(can, values=CAN_SPEEDS,
                                     state="readonly", width=6)
        self.can_kbps.set("500")
        self.can_kbps.grid(row=0, column=5, padx=4)

        self.par = {}
        fields = [
            ("enabled", "PAS enabled (0/1)"),
            ("reverse", "Sensor reversed (0/1)"),
            ("level", "Assist level"),
            ("level_count", "Level count"),
            ("mode", "Mode (0=switch,1=proportional)"),
            ("start_current_pct", "Start current, %"),
            ("start_delay_ms", "Start delay, ms"),
            ("stop_delay_ms", "Stop delay, ms"),
            ("min_cadence_rpm", "Min cadence, rpm"),
            ("full_cadence_rpm", "Full cadence, rpm"),
            ("max_current_a", "Max current, A"),
            ("ramp_up_aps", "Ramp, A/s"),
        ]
        grid = ttk.LabelFrame(f, text="PAS"); grid.pack(padx=8, pady=6)
        for i, (key, label) in enumerate(fields):
            ttk.Label(grid, text=label).grid(row=i % 6, column=(i // 6) * 2,
                                             sticky="e", padx=4, pady=2)
            e = ttk.Entry(grid, width=10)
            e.grid(row=i % 6, column=(i // 6) * 2 + 1, padx=4, pady=2)
            self.par[key] = e
        # Per-button CAN command: on every press of button A/B the helper
        # transmits this raw CAN frame (standard 11-bit id for id <= 0x7FF,
        # extended otherwise). The action semantics live in the LISP script
        # on the VESC — it receives the frame via event-can-sid.
        # Buttons are learned by first press: the first button you ever press
        # on the remote becomes A, the next B (watch the Status tab).
        # Two columns of four buttons each to keep the tab compact.
        acts = ttk.LabelFrame(f, text="Button CAN commands"); acts.pack(pady=4)
        self.btn_canid, self.btn_data = [], []
        for col in (0, 4):
            ttk.Label(acts, text="CAN ID (hex)").grid(row=0, column=col + 1)
            ttk.Label(acts, text="data (hex)").grid(row=0, column=col + 2)
        for i in range(BTN_UI_SLOTS):
            row, col = i % 4 + 1, (i // 4) * 4
            ttk.Label(acts, text="Button %s" % chr(ord("A") + i)).grid(
                row=row, column=col, sticky="e", padx=4, pady=2)
            eid = ttk.Entry(acts, width=8); eid.insert(0, "123")
            eid.grid(row=row, column=col + 1, padx=4)
            self.btn_canid.append(eid)
            edat = ttk.Entry(acts, width=12); edat.insert(0, "%04X" % (i + 1))
            edat.grid(row=row, column=col + 2, padx=4)
            self.btn_data.append(edat)
        ttk.Label(acts, foreground="gray", justify="left", text=(
            "Buttons are learned by first press across ALL bound remotes\n"
            "(first ever pressed = A, next = B, …; watch the Status tab).\n"
            "With lisp/main.lisp: ID=123 data=0001 toggles the throttle,\n"
            "data=0002 switches the speed profile; add your own commands\n"
            "in proc-helper-btn (or see lisp/can_button_skeleton.lisp)."
        )).grid(row=5, column=0, columnspan=8, padx=4, pady=4)
        row = ttk.Frame(f); row.pack(pady=6)
        ttk.Button(row, text="Read", command=self.ble.get_params).pack(side="left", padx=6)
        ttk.Button(row, text="Write", command=self._write_params).pack(side="left", padx=6)

    def _tab_status(self, nb):
        f = ttk.Frame(nb); nb.add(f, text="Status")
        self.st = {}
        for key, label in [("cad", "Cadence sensor"), ("btn", "Remote"),
                           ("btns", "Buttons"),
                           ("rpm", "Cadence, rpm"), ("assist", "Assist current, A"),
                           ("throttle", "Throttle (throttle-on)"),
                           ("pas", "PAS enabled"), ("batt", "Sensor battery"),
                           ("level", "Level")]:
            row = ttk.Frame(f); row.pack(fill="x", padx=20, pady=2)
            ttk.Label(row, text=label + ":", width=22, anchor="e").pack(side="left")
            lbl = ttk.Label(row, text="—"); lbl.pack(side="left", padx=8)
            self.st[key] = lbl
        row = ttk.Frame(f); row.pack(pady=10)
        ttk.Button(row, text="Throttle ON", command=lambda: self.ble.throttle(1)).pack(side="left", padx=4)
        ttk.Button(row, text="Throttle OFF", command=lambda: self.ble.throttle(0)).pack(side="left", padx=4)
        ttk.Button(row, text="Toggle", command=lambda: self.ble.throttle(0xFF)).pack(side="left", padx=4)

    def _tab_fw(self, nb):
        f = ttk.Frame(nb); nb.add(f, text="Firmware")
        ttk.Label(f, text="Helper firmware OTA update\n"
                          "(build/esp32c3_ble_helper.bin)").pack(pady=8)
        self.pb = ttk.Progressbar(f, length=400); self.pb.pack(pady=8)
        ttk.Button(f, text="Choose .bin and update",
                   command=self._pick_fw).pack(pady=8)

    def _tab_lisp(self, nb):
        f = ttk.Frame(nb); nb.add(f, text="LISP")
        ttk.Label(f, text="Upload a LISP script to the VESC via the NUS bridge\n"
                          "(lisp/main.lisp — motor arbiter + PAS + throttle toggle)").pack(pady=8)
        ttk.Button(f, text="Choose .lisp and upload",
                   command=self._pick_lisp).pack(pady=8)

    # ---------- actions ----------

    def _start_scan(self, what):
        for i in self.scan_list.get_children():
            self.scan_list.delete(i)
        self.scan_rows.clear()
        self.ble.scan(what)
        self._log("Scanning (6 s)…")

    def _bind_selected(self):
        sel = self.scan_list.selection()
        if not sel:
            messagebox.showinfo("Binding", "Select a device in the list")
            return
        idx = self.scan_list.index(sel[0])
        what, addr, addr_type = self.scan_rows[idx]
        self.ble.bind(what, addr, addr_type)

    def _write_params(self):
        try:
            g = lambda k: self.par[k].get().strip()
            acts = [BTN_ACT_CUSTOM_CAN] * 8
            bindings = []
            for i in range(BTN_UI_SLOTS):
                can_id = int(self.btn_canid[i].get().strip(), 16)
                data = bytes.fromhex(
                    self.btn_data[i].get().replace(" ", ""))[:8]
                ext = 1 if can_id > 0x7FF else 0
                bindings.append((i, ext, data, can_id))
            blob = struct.pack(
                PARAMS_FMT, PARAMS_VER,
                int(g("enabled")), int(g("reverse")), int(g("level")),
                int(g("level_count")), int(g("mode")),
                int(g("start_current_pct")),
                int(g("start_delay_ms")), int(g("stop_delay_ms")),
                int(g("min_cadence_rpm")), int(g("full_cadence_rpm")),
                int(float(g("max_current_a")) * 1000),
                int(float(g("ramp_up_aps")) * 1000),
                int(self.ctrl_id.get().strip()),
                int(self.tgt_id.get().strip()),
                *acts, int(self.can_kbps.get()))
        except ValueError:
            messagebox.showerror("Parameters", "Check the field values")
            return
        self.ble.set_params(blob)
        for idx, ext, data, can_id in bindings:
            self.ble.set_binding(idx, ext, data, can_id)

    def _pick_fw(self):
        path = filedialog.askopenfilename(filetypes=[("Firmware", "*.bin")])
        if path:
            self.pb["value"] = 0
            self.ble.ota(path)

    def _pick_lisp(self):
        path = filedialog.askopenfilename(
            filetypes=[("LISP", "*.lisp"), ("All files", "*")])
        if path:
            self.pb["value"] = 0
            self.ble.lisp_upload(path)

    # ---------- events from the BLE thread ----------

    def _poll(self):
        try:
            while True:
                kind, kw = self.events.get_nowait()
                getattr(self, "_ev_" + kind)(**kw)
        except queue.Empty:
            pass
        self.root.after(100, self._poll)

    def _ev_log(self, text):
        self._log(text)

    def _ev_connected(self, ok):
        self.lbl_conn.config(text="connected" if ok else "disconnected",
                             foreground="green" if ok else "red")
        if ok:
            self.ble.get_params()

    def _ev_ack(self, cmd, ok):
        self._log("Command %d: %s" % (cmd, "OK" if ok else "error"))
        if cmd in (CMD_BIND_BUTTON, CMD_BIND_CADENCE, CMD_SET_PARAMS):
            self.ble.get_params()

    def _ev_params(self, blob):
        v = struct.unpack(PARAMS_FMT, blob)
        (ver, enabled, reverse, level, level_count, mode, start_pct,
         start_delay, stop_delay, min_rpm, full_rpm,
         max_ma, ramp_maps, ctrl_id, tgt_id) = v[:15]
        if ver != PARAMS_VER:
            self._log("Unsupported params version %d" % ver)
            return
        vals = dict(enabled=enabled, reverse=reverse, level=level,
                    level_count=level_count, mode=mode,
                    start_current_pct=start_pct, start_delay_ms=start_delay,
                    stop_delay_ms=stop_delay, min_cadence_rpm=min_rpm,
                    full_cadence_rpm=full_rpm, max_current_a=max_ma / 1000.0,
                    ramp_up_aps=ramp_maps / 1000.0)
        for k, val in vals.items():
            self.par[k].delete(0, "end")
            self.par[k].insert(0, str(val))
        self.ctrl_id.delete(0, "end"); self.ctrl_id.insert(0, str(ctrl_id))
        self.tgt_id.delete(0, "end");  self.tgt_id.insert(0, str(tgt_id))
        self.can_kbps.set(str(v[-1]))
        for i in range(BTN_UI_SLOTS):
            self.ble.get_binding(i)
        self._log("Parameters read")

    def _ev_binding(self, idx, ext, can_id, data):
        if idx < len(self.btn_canid):
            self.btn_canid[idx].delete(0, "end")
            self.btn_canid[idx].insert(0, "%X" % can_id)
            self.btn_data[idx].delete(0, "end")
            self.btn_data[idx].insert(0, data.hex().upper())

    def _ev_status(self, flags, rpm, batt, level, assist_a, btn_mask, btn_count):
        def onoff(bit):
            return "✓" if flags & (1 << bit) else "—"
        self.st["cad"].config(text="bound %s, connected %s"
                              % (onoff(0), onoff(1)))
        self.st["btn"].config(text="bound %s, connected %s"
                              % (onoff(2), onoff(3)))
        if btn_count:
            parts = ["%s: %s" % (chr(ord("A") + i),
                                 "PRESSED" if btn_mask & (1 << i) else "—")
                     for i in range(min(btn_count, 8))]
            self.st["btns"].config(text=",  ".join(parts))
        else:
            self.st["btns"].config(text="— (none learned yet)")
        self.st["rpm"].config(text="%.1f" % rpm)
        self.st["assist"].config(text="%.2f" % assist_a)
        thr = "ON" if flags & (1 << 5) else "off"
        if not flags & (1 << 6):
            thr += " (no link to VESC?)"
        self.st["throttle"].config(text=thr)
        self.st["pas"].config(text="yes" if flags & (1 << 7) else "no")
        self.st["batt"].config(text="?" if batt == 0xFF else "%d%%" % batt)
        self.st["level"].config(text=str(level))

    def _ev_scan_hit(self, what, addr, addr_type, mac, name, rssi):
        self.scan_rows.append((what, addr, addr_type))
        self.scan_list.insert("", "end", values=(
            "button" if what == WHAT_BUTTON else "cadence", mac, name, rssi))

    def _ev_ota_progress(self, done, total):
        self.pb["maximum"] = total
        self.pb["value"] = done

    def _log(self, text):
        self.log.config(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.config(state="disabled")


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
