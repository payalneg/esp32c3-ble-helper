/// VESC packet framing for the NUS bridge (same protocol as VESC Tool).
library;

import 'dart:typed_data';

/// CRC16-CCITT (XMODEM), same as VESC's crc.c.
int crc16(List<int> data) {
  var crc = 0;
  for (final byte in data) {
    crc ^= (byte & 0xFF) << 8;
    for (var i = 0; i < 8; i++) {
      crc = (crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1;
      crc &= 0xFFFF;
    }
  }
  return crc;
}

/// [2, len] (or [3, len_hi, len_lo]) + payload + crc16 BE + 0x03.
Uint8List vescFrame(List<int> payload) {
  final crc = crc16(payload);
  final b = BytesBuilder();
  if (payload.length <= 255) {
    b.addByte(2);
    b.addByte(payload.length);
  } else {
    b.addByte(3);
    b.addByte(payload.length >> 8);
    b.addByte(payload.length & 0xFF);
  }
  b.add(payload);
  b.addByte(crc >> 8);
  b.addByte(crc & 0xFF);
  b.addByte(3);
  return b.toBytes();
}

/// Parses VESC frames out of the NUS notification byte stream.
class VescStreamParser {
  final List<int> _buf = [];

  List<Uint8List> feed(List<int> data) {
    _buf.addAll(data);
    final out = <Uint8List>[];
    while (true) {
      final pkt = _tryParse();
      if (pkt == null) break;
      out.add(pkt);
    }
    return out;
  }

  Uint8List? _tryParse() {
    while (true) {
      while (_buf.isNotEmpty && _buf[0] != 2 && _buf[0] != 3) {
        _buf.removeAt(0);
      }
      if (_buf.length < 2) return null;
      final int headerLen;
      final int payloadLen;
      if (_buf[0] == 2) {
        headerLen = 2;
        payloadLen = _buf[1];
      } else {
        if (_buf.length < 3) return null;
        headerLen = 3;
        payloadLen = (_buf[1] << 8) | _buf[2];
      }
      final total = headerLen + payloadLen + 3;
      if (_buf.length < total) return null;
      final payload =
          Uint8List.fromList(_buf.sublist(headerLen, headerLen + payloadLen));
      final crc = (_buf[headerLen + payloadLen] << 8) |
          _buf[headerLen + payloadLen + 1];
      final ok = crc == crc16(payload) && _buf[total - 1] == 3;
      _buf.removeRange(0, total);
      if (ok) return payload;
      // Bad CRC: the removeRange above dropped the corrupt frame; retry on
      // whatever follows (loop instead of Python's recursion).
    }
  }
}

/// LISP code blob, same layout as VESC Tool:
///   [u32be packed_len-2][u16be crc16(packed)]
///   packed = [u16 flags=0][code][NUL][i16 imports=0]
Uint8List buildLispBlob(List<int> code) {
  final packed = <int>[0, 0, ...code, 0, 0, 0];
  final b = BytesBuilder();
  final header = ByteData(6);
  header.setUint32(0, packed.length - 2);
  header.setUint16(4, crc16(packed));
  b.add(header.buffer.asUint8List());
  b.add(packed);
  return b.toBytes();
}
