import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:vesc_ble_helper/src/protocol/codec.dart';
import 'package:vesc_ble_helper/src/protocol/vesc_frame.dart';

// Golden vectors generated with Python struct.pack — the same code path
// tools/config_gui.py uses (see the pack formats there).
final kGoldenParams = bytesFromHex(
    '03010002050128fa002c0114005a00d4300000401f00002a070404040404040404f401');
final kGoldenStatus = bytesFromHex('02aa2efb58033cf6ffff0502');
final kGoldenLispBlob = bytesFromHex('0000000cf4e80000287072696e74203129000000');
final kGoldenFrame = bytesFromHex('02028501f44c03'); // vesc_frame([133, 1])

void main() {
  test('crc16 XMODEM check value', () {
    expect(crc16('123456789'.codeUnits), 0x31C3);
  });

  test('vescFrame matches Python framing', () {
    expect(vescFrame([133, 1]), kGoldenFrame);
  });

  test('vescFrame long form for >255-byte payloads', () {
    final payload = List<int>.filled(300, 0xAB);
    final frame = vescFrame(payload);
    expect(frame[0], 3);
    expect((frame[1] << 8) | frame[2], 300);
    expect(frame.last, 3);
    expect(frame.length, 3 + 300 + 3);
  });

  test('VescStreamParser round-trip, one byte at a time, with junk prefix',
      () {
    final payload = [131, 0, 0, 1, 0, 42, 17];
    final stream = [0xFF, 0x00, ...vescFrame(payload), 0x08];
    final parser = VescStreamParser();
    final packets = <Uint8List>[];
    for (final byte in stream) {
      packets.addAll(parser.feed([byte]));
    }
    expect(packets, hasLength(1));
    expect(packets.first, payload);
  });

  test('VescStreamParser drops a corrupt frame and recovers', () {
    final good = vescFrame([133, 1]);
    final bad = Uint8List.fromList(good);
    bad[3] ^= 0xFF; // corrupt the CRC region
    final parser = VescStreamParser();
    final packets = [...parser.feed(bad), ...parser.feed(good)];
    expect(packets, hasLength(1));
    expect(packets.first, [133, 1]);
  });

  test('PasParams decodes and re-encodes the golden blob', () {
    final p = PasParams.decode(kGoldenParams)!;
    expect(p.enabled, 1);
    expect(p.reverse, 0);
    expect(p.level, 2);
    expect(p.levelCount, 5);
    expect(p.mode, 1);
    expect(p.startCurrentPct, 40);
    expect(p.startDelayMs, 250);
    expect(p.stopDelayMs, 300);
    expect(p.minCadenceRpm, 20);
    expect(p.fullCadenceRpm, 90);
    expect(p.maxCurrentMa, 12500);
    expect(p.rampUpMaps, 8000);
    expect(p.controllerId, 42);
    expect(p.targetVescId, 7);
    expect(p.btnActions, List.filled(8, 4));
    expect(p.canKbps, 500);
    expect(p.encode(), kGoldenParams);
  });

  test('PasParams rejects wrong version', () {
    final blob = Uint8List.fromList(kGoldenParams);
    blob[0] = 2;
    expect(PasParams.decode(blob), isNull);
  });

  test('StatusFrame decodes negative rpm/assist', () {
    final s = StatusFrame.decode(kGoldenStatus)!;
    expect(s.flags, 0xAA);
    expect(s.rpm, closeTo(-12.34, 1e-9));
    expect(s.batt, 88);
    expect(s.level, 3);
    expect(s.assistA, closeTo(-2.5, 1e-9));
    expect(s.btnMask, 0x05);
    expect(s.btnCount, 2);
    expect(s.cadenceConnected, true); // bit 1 of 0xAA
    expect(s.cadenceBound, false); // bit 0
    expect(s.remoteConnected, true); // bit 3
    expect(s.throttleOn, true); // bit 5
    expect(s.vescLink, false); // bit 6
    expect(s.pasEnabled, true); // bit 7
  });

  test('buildLispBlob matches the VESC Tool layout', () {
    expect(buildLispBlob('(print 1)'.codeUnits), kGoldenLispBlob);
  });

  test('ScanHit decodes MAC reversed and UTF-8 name', () {
    final data = Uint8List.fromList([
      1, 0, // what=button, addr_type=0
      0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // addr LSB-first
      0xC6, // rssi = -58
      4, ...'BK6L'.codeUnits,
    ]);
    final hit = ScanHit.decode(data)!;
    expect(hit.mac, '11:22:33:44:55:66');
    expect(hit.rssi, -58);
    expect(hit.name, 'BK6L');
  });

  test('ButtonBinding decode/encodeSet round-trip', () {
    final notify = Uint8List.fromList([
      0x89, 3, 1, 2, // rsp, idx, ext, len
      0x23, 0x01, 0x00, 0x00, // can_id 0x123 LE
      0x00, 0x02, 0, 0, 0, 0, 0, 0, // data
    ]);
    final b = ButtonBinding.decode(notify)!;
    expect(b.idx, 3);
    expect(b.ext, 1);
    expect(b.canId, 0x123);
    expect(b.data, [0x00, 0x02]);
    expect(b.encodeSet(), [
      8, 3, 1, 2, // CMD_SET_BINDING, idx, ext, len
      0x23, 0x01, 0x00, 0x00,
      0x00, 0x02, 0, 0, 0, 0, 0, 0,
    ]);
  });
}
