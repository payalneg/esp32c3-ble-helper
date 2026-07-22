/// Helper-firmware OTA over BLE — port of BleWorker._ota.
part of 'helper_client.dart';

extension OtaService on HelperBleClient {
  Future<void> flashFirmware(Uint8List image) async {
    final ctrl = _otaCtrl;
    final data = _otaData;
    if (ctrl == null || data == null) {
      _log('Not connected');
      return;
    }
    if (busy) return;
    _setBusy(true);
    try {
      final sha = sha256.convert(image).bytes;
      _log('OTA: ${image.length} bytes, erasing partition…');
      _otaEvents.clear();
      final begin = Uint8List(1 + 4 + 32);
      begin[0] = Ota.opBegin;
      ByteData.sublistView(begin).setUint32(1, image.length, Endian.little);
      begin.setRange(5, 37, sha);
      await ctrl.write(begin);
      final ready = await _otaEvents.next(const Duration(seconds: 30));
      if (ready.st != Ota.stReady) {
        throw ProtocolException(
            'BEGIN rejected: 0x${ready.st.toRadixString(16)}/${ready.detail}');
      }
      _log('OTA: transferring…');
      final chunk = min(244, _mtuChunk);
      for (var off = 0; off < image.length; off += chunk) {
        await data.write(image.sublist(off, min(off + chunk, image.length)),
            withoutResponse: true);
        if (off % (32 * chunk) == 0) _setProgress(off, image.length);
      }
      _setProgress(image.length, image.length);
      await ctrl.write([Ota.opEnd]);
      while (true) {
        final ev = await _otaEvents.next(const Duration(seconds: 30));
        if (ev.st == Ota.stProgress) continue;
        if (ev.st == Ota.stDone) {
          _log('OTA done — device is rebooting');
          break;
        }
        throw ProtocolException(
            'OTA error: 0x${ev.st.toRadixString(16)}/${ev.detail}');
      }
    } catch (e) {
      _log('OTA: $e');
      try {
        await _otaCtrl?.write([Ota.opAbort]);
      } catch (_) {}
    } finally {
      _setBusy(false);
    }
  }
}
