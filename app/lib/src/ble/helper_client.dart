/// BLE client for the VESC BLE Helper — the Dart port of BleWorker in
/// tools/config_gui.py. One instance drives the whole app: it owns the
/// connection, routes notifications, and exposes UI state (ChangeNotifier)
/// plus typed streams for scan hits / params / bindings.
library;

import 'dart:async';
import 'dart:math';

import 'package:crypto/crypto.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../protocol/codec.dart';
import '../protocol/constants.dart';
import '../protocol/vesc_frame.dart';
import 'event_queue.dart';

part 'lisp_service.dart';
part 'ota_service.dart';

class ProtocolException implements Exception {
  final String message;
  ProtocolException(this.message);
  @override
  String toString() => message;
}

class HelperBleClient extends ChangeNotifier {
  // ---------- UI-facing state ----------

  bool connected = false;
  bool connecting = false;

  /// An OTA or LISP transfer is running; other tabs disable their actions.
  bool busy = false;

  /// Helper's running firmware version (DIS Firmware Revision String); null
  /// while disconnected and on firmwares that predate the characteristic.
  String? fwVersion;

  StatusFrame? status;
  ({int done, int total})? progress;

  final List<String> logLines = [];
  static const _logLimit = 300;

  final _params = StreamController<PasParams>.broadcast();
  final _bindings = StreamController<ButtonBinding>.broadcast();
  final _scanHits = StreamController<ScanHit>.broadcast();
  Stream<PasParams> get onParams => _params.stream;
  Stream<ButtonBinding> get onBinding => _bindings.stream;
  Stream<ScanHit> get onScanHit => _scanHits.stream;

  // ---------- internals ----------

  BluetoothDevice? _device;
  StreamSubscription<BluetoothConnectionState>? _connSub;
  BluetoothCharacteristic? _cfgCtrl, _otaCtrl, _otaData, _nusRx;

  final _nusParser = VescStreamParser();
  final _nusPackets = EventQueue<Uint8List>();
  final _otaEvents = EventQueue<({int st, int detail})>();

  int get _mtu => max(23, _device?.mtuNow ?? 23);
  int get _mtuChunk => _mtu - 3;

  void _log(String text) {
    logLines.add(text);
    if (logLines.length > _logLimit) {
      logLines.removeRange(0, logLines.length - _logLimit);
    }
    notifyListeners();
  }

  void _setProgress(int done, int total) {
    progress = (done: done, total: total);
    notifyListeners();
  }

  /// Transfer guard for the OTA/LISP extensions (they cannot call the
  /// protected notifyListeners themselves).
  void _setBusy(bool value) {
    busy = value;
    if (value) progress = null;
    notifyListeners();
  }

  // ---------- connection ----------

  Future<void> connect() async {
    if (connected || connecting) return;
    connecting = true;
    notifyListeners();
    try {
      await _waitAdapterOn();
      _log('Searching for $kDeviceName…');
      final device = await _findDevice();
      if (device == null) {
        _log('Device not found');
        return;
      }
      await device.connect(mtu: 512, timeout: const Duration(seconds: 20));
      _device = device;
      _connSub = device.connectionState.listen((s) {
        if (s == BluetoothConnectionState.disconnected) _onDisconnected();
      });
      await _discoverAndSubscribe(device);
      connected = true;
      _log('Connected: ${device.remoteId} (MTU $_mtu, '
          'fw ${fwVersion ?? "unknown"})');
      notifyListeners();
      getParams();
    } catch (e) {
      _log('Connect failed: $e');
      await disconnect();
    } finally {
      connecting = false;
      notifyListeners();
    }
  }

  Future<void> disconnect() async {
    final device = _device;
    try {
      await device?.disconnect();
    } catch (_) {}
    if (_device != null) _onDisconnected();
  }

  Future<void> _waitAdapterOn() async {
    if (await FlutterBluePlus.adapterState.first == BluetoothAdapterState.on) {
      return;
    }
    try {
      await FlutterBluePlus.turnOn(); // Android: asks the user to enable BT
    } catch (_) {}
    await FlutterBluePlus.adapterState
        .where((s) => s == BluetoothAdapterState.on)
        .first
        .timeout(const Duration(seconds: 15),
            onTimeout: () => throw ProtocolException('Bluetooth is off'));
  }

  /// The helper advertises the NUS service UUID; its name only appears in
  /// the scan response — so filter by UUID and confirm the name here.
  Future<BluetoothDevice?> _findDevice() async {
    final found = Completer<BluetoothDevice>();
    final sub = FlutterBluePlus.onScanResults.listen((results) {
      for (final r in results) {
        if ((r.advertisementData.advName == kDeviceName ||
                r.device.platformName == kDeviceName) &&
            !found.isCompleted) {
          found.complete(r.device);
        }
      }
    });
    try {
      await FlutterBluePlus.startScan(
          withServices: [Guid(kNusServiceUuid)],
          timeout: const Duration(seconds: 10));
      return await found.future
          .timeout(const Duration(seconds: 10), onTimeout: () => throw '');
    } catch (_) {
      return null;
    } finally {
      await sub.cancel();
      try {
        await FlutterBluePlus.stopScan();
      } catch (_) {}
    }
  }

  Future<void> _discoverAndSubscribe(BluetoothDevice device) async {
    final chars = <Guid, BluetoothCharacteristic>{};
    for (final svc in await device.discoverServices()) {
      for (final c in svc.characteristics) {
        chars[c.uuid] = c;
      }
    }
    BluetoothCharacteristic need(String uuid) {
      final c = chars[Guid(uuid)];
      if (c == null) throw ProtocolException('missing characteristic $uuid');
      return c;
    }

    _cfgCtrl = need(kCfgCtrlUuid);
    _otaCtrl = need(kOtaCtrlUuid);
    _otaData = need(kOtaDataUuid);
    _nusRx = need(kNusRxUuid);

    Future<void> subscribe(BluetoothCharacteristic c,
        void Function(Uint8List data) handler) async {
      final sub = c.onValueReceived
          .listen((value) => handler(Uint8List.fromList(value)));
      device.cancelWhenDisconnected(sub);
      await c.setNotifyValue(true);
    }

    await subscribe(_cfgCtrl!, _onCtrl);
    await subscribe(need(kCfgStatusUuid), _onStatus);
    await subscribe(need(kCfgScanUuid), _onScan);
    await subscribe(_otaCtrl!, _onOta);
    await subscribe(need(kNusTxUuid), _onNus);

    final fwRev = chars[Guid(kFwRevisionUuid)];
    if (fwRev != null) {
      try {
        fwVersion = String.fromCharCodes(await fwRev.read()).trim();
      } catch (_) {}
    }
  }

  void _onDisconnected() {
    if (_device == null) return;
    _device = null;
    _connSub?.cancel();
    _connSub = null;
    _cfgCtrl = _otaCtrl = _otaData = _nusRx = null;
    fwVersion = null;
    connected = false;
    _log('Connection lost');
    notifyListeners();
  }

  // ---------- notifications ----------

  void _onCtrl(Uint8List data) {
    if (data.isEmpty) return;
    final rsp = data[0];
    if (rsp == 0x85 && data.length >= 1 + PasParams.length) {
      final params =
          PasParams.decode(data.sublist(1, 1 + PasParams.length));
      if (params == null) {
        _log('Unsupported params version ${data[1]}');
        return;
      }
      _params.add(params);
      _log('Parameters read');
      for (var i = 0; i < kBtnUiSlots; i++) {
        getBinding(i);
      }
    } else if (rsp == 0x89 && data.length >= 16) {
      final binding = ButtonBinding.decode(data);
      if (binding != null) _bindings.add(binding);
    } else {
      final cmd = rsp & 0x7F;
      final ok = data.length >= 2 && data[1] == 0;
      _log('Command $cmd: ${ok ? "OK" : "error"}');
      if (cmd == Cmd.bindButton ||
          cmd == Cmd.bindCadence ||
          cmd == Cmd.setParams) {
        getParams();
      }
    }
  }

  void _onStatus(Uint8List data) {
    final frame = StatusFrame.decode(data);
    if (frame == null) return;
    status = frame;
    notifyListeners();
  }

  void _onScan(Uint8List data) {
    final hit = ScanHit.decode(data);
    if (hit != null) _scanHits.add(hit);
  }

  void _onOta(Uint8List data) {
    if (data.length < 5) return;
    _otaEvents.add((
      st: data[0],
      detail: ByteData.sublistView(data).getUint32(1, Endian.little),
    ));
  }

  void _onNus(Uint8List data) {
    for (final pkt in _nusParser.feed(data)) {
      _nusPackets.add(pkt);
    }
  }

  // ---------- config-service commands ----------

  Future<void> _ctrl(List<int> payload) async {
    final c = _cfgCtrl;
    if (c == null) {
      _log('Not connected');
      return;
    }
    try {
      await c.write(payload); // with response, as in the Python GUI
    } catch (e) {
      _log('Write failed: $e');
    }
  }

  void scanRemotes(int what) {
    _log('Scanning (6 s)…'); // the scan runs on the ESP32, not the phone
    unawaited(_ctrl([Cmd.scan, what]));
  }
  void unbind(int what) => unawaited(_ctrl([Cmd.unbind, what]));
  void getParams() => unawaited(_ctrl([Cmd.getParams]));
  void setParams(Uint8List blob) =>
      unawaited(_ctrl([Cmd.setParams, ...blob]));
  void throttle(int v) => unawaited(_ctrl([Cmd.setThrottle, v]));
  void getBinding(int idx) => unawaited(_ctrl([Cmd.getBinding, idx]));
  void setBinding(ButtonBinding binding) =>
      unawaited(_ctrl(binding.encodeSet()));

  void bind(ScanHit hit) {
    final cmd = hit.what == kWhatButton ? Cmd.bindButton : Cmd.bindCadence;
    unawaited(_ctrl([cmd, hit.addrType, ...hit.addr]));
  }
}
