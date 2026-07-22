// Screenshot smoke test — renders the tabs with a fake status frame and
// writes goldens/*.png when run with --update-goldens. Not a regression
// gate; a quick way to eyeball the UI without a device.
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show FontLoader;
import 'package:flutter_test/flutter_test.dart';

import 'package:vesc_ble_helper/main.dart';
import 'package:vesc_ble_helper/src/ble/helper_client.dart';
import 'package:vesc_ble_helper/src/protocol/codec.dart';
import 'package:vesc_ble_helper/src/ui/params_tab.dart';
import 'package:vesc_ble_helper/src/ui/status_tab.dart';
import 'package:vesc_ble_helper/src/ui/transfer_progress.dart';

Future<void> _loadRealFonts() async {
  final flutterRoot = Platform.environment['FLUTTER_ROOT'] ??
      '/Users/alexey/flutter'; // fallback; overridden by the env in CI
  final fonts = '$flutterRoot/bin/cache/artifacts/material_fonts';
  Future<void> load(String family, List<String> files) async {
    final loader = FontLoader(family);
    for (final f in files) {
      final file = File('$fonts/$f');
      if (!file.existsSync()) return;
      loader.addFont(Future.value(
          ByteData.sublistView(Uint8List.fromList(file.readAsBytesSync()))));
    }
    await loader.load();
  }

  await load('Roboto', [
    'Roboto-Regular.ttf',
    'Roboto-Medium.ttf',
    'Roboto-Bold.ttf',
  ]);
  await load('MaterialIcons', ['MaterialIcons-Regular.otf']);
}

StatusFrame _fakeStatus() {
  // version 2, flags: cadence bound+connected, remote bound (offline),
  // throttle on, vesc link ok, pas enabled; 72.3 rpm, 85 % battery,
  // level 2, 5.23 A assist, buttons A+C pressed of 4.
  final b = ByteData(12);
  b.setUint8(0, 2);
  b.setUint8(1, 0xE7 & ~(1 << 3));
  b.setInt16(2, 7230, Endian.little);
  b.setUint8(4, 85);
  b.setUint8(5, 2);
  b.setInt32(6, 5230, Endian.little);
  b.setUint8(10, 0x05);
  b.setUint8(11, 4);
  return StatusFrame.decode(b.buffer.asUint8List())!;
}

void main() {
  testWidgets('status dashboard screenshot', (tester) async {
    await _loadRealFonts();
    tester.view.physicalSize = const Size(1080, 2280);
    tester.view.devicePixelRatio = 2.625; // Pixel-ish 412×869 dp
    addTearDown(tester.view.reset);

    final client = HelperBleClient();
    client.connected = true;
    client.status = _fakeStatus();
    client.busy = true; // show the global OTA/LISP transfer banner
    client.progress = (done: 262144, total: 633856);

    await tester.pumpWidget(MaterialApp(
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.teal),
      ),
      home: Scaffold(
        appBar: AppBar(title: const Text('VESC BLE Helper')),
        body: Column(
          children: [
            Expanded(child: StatusTab(client: client)),
            TransferBanner(client: client),
          ],
        ),
      ),
    ));
    await tester.pump(const Duration(seconds: 1)); // settle the gauge tween

    await expectLater(
        find.byType(MaterialApp), matchesGoldenFile('goldens/status_tab.png'));
  });

  testWidgets('params tab screenshot', (tester) async {
    await _loadRealFonts();
    tester.view.physicalSize = const Size(1080, 2280);
    tester.view.devicePixelRatio = 2.625;
    addTearDown(tester.view.reset);

    await tester.pumpWidget(MaterialApp(
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.teal),
        inputDecorationTheme: InputDecorationTheme(
          isDense: true,
          border: OutlineInputBorder(borderRadius: BorderRadius.circular(10)),
        ),
      ),
      home: Scaffold(
        appBar: AppBar(title: const Text('Parameters')),
        body: ParamsTab(client: HelperBleClient()),
      ),
    ));
    await tester.pump(const Duration(milliseconds: 300));

    await expectLater(
        find.byType(MaterialApp), matchesGoldenFile('goldens/params_tab.png'));
  });

  testWidgets('full app screenshot (disconnected)', (tester) async {
    await _loadRealFonts();
    tester.view.physicalSize = const Size(1080, 2280);
    tester.view.devicePixelRatio = 2.625;
    addTearDown(tester.view.reset);

    await tester.pumpWidget(const VescHelperApp());
    await tester.pump(const Duration(milliseconds: 300));

    await expectLater(
        find.byType(MaterialApp), matchesGoldenFile('goldens/app_home.png'));
  });
}
