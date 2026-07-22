/// LISP script upload to the VESC through the NUS bridge — port of
/// BleWorker._lisp_upload / _nus_cmd (VESC Tool protocol).
part of 'helper_client.dart';

extension LispService on HelperBleClient {
  Future<void> uploadLisp(Uint8List code) async {
    if (_nusRx == null) {
      _log('Not connected');
      return;
    }
    if (busy) return;
    _setBusy(true);
    try {
      final blob = buildLispBlob(code);
      _log('LISP: stopping script…');
      await _nusCmd([VescComm.lispSetRunning, 0], VescComm.lispSetRunning);
      _log('LISP: erasing (${blob.length} bytes)…');
      final erase = ByteData(5)
        ..setUint8(0, VescComm.lispEraseCode)
        ..setInt32(1, blob.length + 100, Endian.big);
      await _nusCmd(erase.buffer.asUint8List(), VescComm.lispEraseCode,
          timeout: const Duration(seconds: 15));
      const chunk = 240;
      for (var off = 0; off < blob.length; off += chunk) {
        final header = ByteData(5)
          ..setUint8(0, VescComm.lispWriteCode)
          ..setUint32(1, off, Endian.big);
        await _nusCmd([
          ...header.buffer.asUint8List(),
          ...blob.sublist(off, min(off + chunk, blob.length)),
        ], VescComm.lispWriteCode);
        _setProgress(off, blob.length);
      }
      _setProgress(blob.length, blob.length);
      _log('LISP: starting…');
      await _nusCmd([VescComm.lispSetRunning, 1], VescComm.lispSetRunning);
      _log('LISP: script uploaded and running');
    } catch (e) {
      _log('LISP: error — $e');
    } finally {
      _setBusy(false);
    }
  }

  /// Send a VESC packet and wait for a reply with the same command byte.
  Future<Uint8List> _nusCmd(List<int> payload, int expect,
      {Duration timeout = const Duration(seconds: 8)}) async {
    final rx = _nusRx;
    if (rx == null) throw ProtocolException('not connected');
    final frame = vescFrame(payload);
    // The firmware's NUS RX is a stream parser — fragmenting to MTU is safe.
    for (var off = 0; off < frame.length; off += _mtuChunk) {
      await rx.write(frame.sublist(off, min(off + _mtuChunk, frame.length)),
          withoutResponse: true);
    }
    final deadline = DateTime.now().add(timeout);
    try {
      while (true) {
        final pkt = await _nusPackets.next(deadline.difference(DateTime.now()));
        if (pkt.isNotEmpty && pkt[0] == expect) return pkt;
      }
    } on TimeoutException {
      throw ProtocolException('no reply to cmd ${payload[0]}');
    }
  }
}
