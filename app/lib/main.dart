import 'package:flutter/material.dart';

import 'src/ble/helper_client.dart';
import 'src/bundled_firmware.dart';
import 'src/ui/bind_tab.dart';
import 'src/ui/firmware_tab.dart';
import 'src/ui/lisp_tab.dart';
import 'src/ui/log_panel.dart';
import 'src/ui/params_tab.dart';
import 'src/ui/status_tab.dart';
import 'src/ui/transfer_progress.dart';

void main() => runApp(const VescHelperApp());

class VescHelperApp extends StatelessWidget {
  const VescHelperApp({super.key});

  static ThemeData _theme(Brightness brightness) => ThemeData(
        useMaterial3: true,
        colorScheme:
            ColorScheme.fromSeed(seedColor: Colors.teal, brightness: brightness),
        inputDecorationTheme: InputDecorationTheme(
          isDense: true,
          border: OutlineInputBorder(borderRadius: BorderRadius.circular(10)),
        ),
        snackBarTheme:
            const SnackBarThemeData(behavior: SnackBarBehavior.floating),
      );

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'VESC BLE Helper',
      theme: _theme(Brightness.light),
      darkTheme: _theme(Brightness.dark),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final client = HelperBleClient();

  /// One OTA offer per connection — reset when the link drops.
  bool _otaOffered = false;

  @override
  void initState() {
    super.initState();
    client.addListener(_maybeOfferOta);
  }

  @override
  void dispose() {
    client.removeListener(_maybeOfferOta);
    client.disconnect();
    client.dispose();
    super.dispose();
  }

  /// Offer the bundled firmware when the connected helper runs a different
  /// (or unknown — pre-DIS) version.
  Future<void> _maybeOfferOta() async {
    if (!client.connected) {
      _otaOffered = false;
      return;
    }
    if (_otaOffered || client.busy) return;
    _otaOffered = true;
    final bundled = await BundledFirmware.load();
    if (bundled.version == null) return;
    final running = client.fwVersion;
    if (running == bundled.version) return;
    if (!mounted || !client.connected) return;
    final go = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Firmware update available'),
        content: Text(
            'Helper is running ${running == null ? "an old firmware "
                "(version unknown)" : "v$running"}; '
            'this app bundles v${bundled.version}.\n\n'
            'Flash it over BLE now? The helper reboots when done.'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: const Text('Later')),
          FilledButton(
              onPressed: () => Navigator.pop(context, true),
              child: const Text('Update')),
        ],
      ),
    );
    if (go == true && client.connected && !client.busy) {
      await client.flashFirmware(bundled.image);
    }
  }

  @override
  Widget build(BuildContext context) {
    return DefaultTabController(
      length: 5,
      child: Scaffold(
        appBar: AppBar(
          title: const Text('VESC BLE Helper'),
          actions: [_ConnectAction(client: client)],
          bottom: const TabBar(
            isScrollable: true,
            tabAlignment: TabAlignment.start,
            tabs: [
              Tab(icon: Icon(Icons.speed), text: 'Status'),
              Tab(icon: Icon(Icons.link), text: 'Binding'),
              Tab(icon: Icon(Icons.tune), text: 'Parameters'),
              Tab(icon: Icon(Icons.system_update_alt), text: 'Firmware'),
              Tab(icon: Icon(Icons.code), text: 'LISP'),
            ],
          ),
        ),
        body: Column(
          children: [
            Expanded(
              child: TabBarView(
                children: [
                  StatusTab(client: client),
                  BindTab(client: client),
                  ParamsTab(client: client),
                  FirmwareTab(client: client),
                  LispTab(client: client),
                ],
              ),
            ),
            TransferBanner(client: client),
            LogPanel(client: client),
          ],
        ),
      ),
    );
  }
}

class _ConnectAction extends StatelessWidget {
  final HelperBleClient client;
  const _ConnectAction({required this.client});

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: client,
      builder: (context, _) {
        final Widget icon = client.connecting
            ? const SizedBox(
                width: 16,
                height: 16,
                child: CircularProgressIndicator(strokeWidth: 2),
              )
            : Icon(
                client.connected
                    ? Icons.bluetooth_connected
                    : Icons.bluetooth_searching,
                size: 18,
              );
        final label = Text(client.connecting
            ? 'Connecting…'
            : client.connected
                ? 'Disconnect'
                : 'Connect');
        final onPressed = client.connecting
            ? null
            : () => client.connected ? client.disconnect() : client.connect();
        return Padding(
          padding: const EdgeInsets.only(right: 8),
          child: client.connected
              ? FilledButton.tonalIcon(
                  icon: icon, label: label, onPressed: onPressed)
              : FilledButton.icon(
                  icon: icon, label: label, onPressed: onPressed),
        );
      },
    );
  }
}
