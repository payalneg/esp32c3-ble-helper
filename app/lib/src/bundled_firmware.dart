/// The OTA image + version baked into the APK by tools/sync_app_assets.sh.
library;

import 'dart:typed_data';

import 'package:flutter/services.dart' show rootBundle;

class BundledFirmware {
  final Uint8List image;

  /// PROJECT_VER of the bundled image (assets/firmware/version.txt), or null
  /// if the asset was not staged.
  final String? version;

  BundledFirmware._(this.image, this.version);

  static Future<BundledFirmware>? _cached;

  static Future<BundledFirmware> load() => _cached ??= _load();

  static Future<BundledFirmware> _load() async {
    final data =
        await rootBundle.load('assets/firmware/esp32c3_ble_helper.bin');
    String? version;
    try {
      version =
          (await rootBundle.loadString('assets/firmware/version.txt')).trim();
      if (version.isEmpty) version = null;
    } catch (_) {}
    return BundledFirmware._(data.buffer.asUint8List(), version);
  }
}
