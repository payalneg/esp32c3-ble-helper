/// Binary codecs for the config-service payloads. Layouts mirror the Python
/// struct formats in tools/config_gui.py (little-endian, no padding).
library;

import 'dart:convert';
import 'dart:typed_data';

import 'constants.dart';

/// PAS/CAN parameter blob, Python `"<7B4H2I2B8BH"`, version 3.
class PasParams {
  static const version = 3;
  static const length = 35;

  int enabled = 1;
  int reverse = 0;
  int level = 1;
  int levelCount = 3;
  int mode = 0;
  int startCurrentPct = 0;
  int startDelayMs = 0;
  int stopDelayMs = 0;
  int minCadenceRpm = 0;
  int fullCadenceRpm = 0;
  int maxCurrentMa = 0;
  int rampUpMaps = 0; // mA per second
  int controllerId = 0;
  int targetVescId = 0;
  List<int> btnActions = List.filled(kBtnUiSlots, kBtnActCustomCan);
  int canKbps = 500;

  Uint8List encode() {
    final b = ByteData(length);
    b.setUint8(0, version);
    b.setUint8(1, enabled);
    b.setUint8(2, reverse);
    b.setUint8(3, level);
    b.setUint8(4, levelCount);
    b.setUint8(5, mode);
    b.setUint8(6, startCurrentPct);
    b.setUint16(7, startDelayMs, Endian.little);
    b.setUint16(9, stopDelayMs, Endian.little);
    b.setUint16(11, minCadenceRpm, Endian.little);
    b.setUint16(13, fullCadenceRpm, Endian.little);
    b.setUint32(15, maxCurrentMa, Endian.little);
    b.setUint32(19, rampUpMaps, Endian.little);
    b.setUint8(23, controllerId);
    b.setUint8(24, targetVescId);
    for (var i = 0; i < kBtnUiSlots; i++) {
      b.setUint8(25 + i, btnActions[i]);
    }
    b.setUint16(33, canKbps, Endian.little);
    return b.buffer.asUint8List();
  }

  /// Returns null when the blob is short or carries an unknown version.
  static PasParams? decode(Uint8List blob) {
    if (blob.length < length) return null;
    final b = ByteData.sublistView(blob);
    if (b.getUint8(0) != version) return null;
    return PasParams()
      ..enabled = b.getUint8(1)
      ..reverse = b.getUint8(2)
      ..level = b.getUint8(3)
      ..levelCount = b.getUint8(4)
      ..mode = b.getUint8(5)
      ..startCurrentPct = b.getUint8(6)
      ..startDelayMs = b.getUint16(7, Endian.little)
      ..stopDelayMs = b.getUint16(9, Endian.little)
      ..minCadenceRpm = b.getUint16(11, Endian.little)
      ..fullCadenceRpm = b.getUint16(13, Endian.little)
      ..maxCurrentMa = b.getUint32(15, Endian.little)
      ..rampUpMaps = b.getUint32(19, Endian.little)
      ..controllerId = b.getUint8(23)
      ..targetVescId = b.getUint8(24)
      ..btnActions = [for (var i = 0; i < kBtnUiSlots; i++) b.getUint8(25 + i)]
      ..canKbps = b.getUint16(33, Endian.little);
  }
}

/// Live status notification, Python `"<BBhBBiBB"`, version 2.
class StatusFrame {
  static const version = 2;
  static const length = 12;

  final int flags;
  final double rpm;
  final int batt;
  final int level;
  final double assistA;
  final int btnMask;
  final int btnCount;

  StatusFrame._(this.flags, this.rpm, this.batt, this.level, this.assistA,
      this.btnMask, this.btnCount);

  bool get cadenceBound => flags & (1 << 0) != 0;
  bool get cadenceConnected => flags & (1 << 1) != 0;
  bool get remoteBound => flags & (1 << 2) != 0;
  bool get remoteConnected => flags & (1 << 3) != 0;
  bool get throttleOn => flags & (1 << 5) != 0;
  bool get vescLink => flags & (1 << 6) != 0;
  bool get pasEnabled => flags & (1 << 7) != 0;

  static StatusFrame? decode(Uint8List data) {
    if (data.length < length) return null;
    final b = ByteData.sublistView(data);
    if (b.getUint8(0) != version) return null;
    return StatusFrame._(
      b.getUint8(1),
      b.getInt16(2, Endian.little) / 100.0,
      b.getUint8(4),
      b.getUint8(5),
      b.getInt32(6, Endian.little) / 1000.0,
      b.getUint8(10),
      b.getUint8(11),
    );
  }
}

/// One hit from the firmware-side BLE scan (CFG_SCAN notification).
class ScanHit {
  final int what; // kWhatButton / kWhatCadence
  final int addrType;
  final Uint8List addr; // raw NimBLE byte order, passed back verbatim on bind
  final int rssi;
  final String name;

  ScanHit._(this.what, this.addrType, this.addr, this.rssi, this.name);

  /// NimBLE stores the address least-significant byte first.
  String get mac => addr.reversed
      .map((b) => b.toRadixString(16).padLeft(2, '0').toUpperCase())
      .join(':');

  static ScanHit? decode(Uint8List data) {
    if (data.length < 10) return null;
    final nameLen = data[9];
    final name = utf8.decode(
        data.sublist(10, (10 + nameLen).clamp(0, data.length)),
        allowMalformed: true);
    return ScanHit._(data[0], data[1], Uint8List.fromList(data.sublist(2, 8)),
        ByteData.sublistView(data).getInt8(8), name);
  }
}

/// Per-button custom CAN frame (CTRL response 0x89 / CMD_SET_BINDING).
class ButtonBinding {
  final int idx;
  final int ext;
  final int canId;
  final Uint8List data;

  ButtonBinding(this.idx, this.ext, this.canId, this.data);

  static ButtonBinding? decode(Uint8List d) {
    if (d.length < 16) return null;
    final len = d[3] > 8 ? 8 : d[3];
    final canId = ByteData.sublistView(d).getUint32(4, Endian.little);
    return ButtonBinding(
        d[1], d[2], canId, Uint8List.fromList(d.sublist(8, 8 + len)));
  }

  /// CMD_SET_BINDING payload: [cmd, idx, ext, len] + u32le id + data.pad(8).
  Uint8List encodeSet() {
    final out = Uint8List(4 + 4 + 8);
    out[0] = Cmd.setBinding;
    out[1] = idx;
    out[2] = ext;
    out[3] = data.length > 8 ? 8 : data.length;
    ByteData.sublistView(out).setUint32(4, canId, Endian.little);
    out.setRange(8, 8 + out[3], data);
    return out;
  }
}

String hexBytes(List<int> data) => data
    .map((b) => b.toRadixString(16).padLeft(2, '0').toUpperCase())
    .join();

/// Strict hex → bytes (like Python's bytes.fromhex): spaces allowed,
/// throws FormatException on odd length or bad digits.
Uint8List bytesFromHex(String hex) {
  final clean = hex.replaceAll(' ', '');
  if (clean.length.isOdd) {
    throw const FormatException('odd-length hex string');
  }
  return Uint8List.fromList([
    for (var i = 0; i < clean.length; i += 2)
      int.parse(clean.substring(i, i + 2), radix: 16),
  ]);
}
