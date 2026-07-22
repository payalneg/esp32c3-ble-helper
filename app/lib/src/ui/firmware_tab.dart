import 'package:flutter/material.dart';

import '../ble/helper_client.dart';
import '../bundled_firmware.dart';
import 'common.dart';

/// OTA update of the helper firmware with the image bundled into the APK.
class FirmwareTab extends StatefulWidget {
  final HelperBleClient client;
  const FirmwareTab({super.key, required this.client});

  @override
  State<FirmwareTab> createState() => _FirmwareTabState();
}

class _FirmwareTabState extends State<FirmwareTab>
    with AutomaticKeepAliveClientMixin {
  BundledFirmware? _bundled;

  @override
  bool get wantKeepAlive => true;

  @override
  void initState() {
    super.initState();
    BundledFirmware.load().then((fw) {
      if (mounted) setState(() => _bundled = fw);
    });
  }

  Future<void> _flash() async {
    final bundled = _bundled;
    if (bundled == null) return;
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Flash firmware?'),
        content: Text('Write the bundled image (${bundled.image.length} '
            'bytes) to the helper over BLE. The device reboots when done.'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, true),
              child: const Text('Flash')),
        ],
      ),
    );
    if (confirmed == true) await widget.client.flashFirmware(bundled.image);
  }

  @override
  Widget build(BuildContext context) {
    super.build(context);
    final client = widget.client;
    return ListenableBuilder(
      listenable: client,
      builder: (context, _) {
        final bundled = _bundled;
        final running = client.connected ? client.fwVersion : null;
        final updatable = client.connected &&
            bundled?.version != null &&
            running != bundled!.version;
        final scheme = Theme.of(context).colorScheme;
        return ListView(
          padding: const EdgeInsets.all(12),
          children: [
            SectionCard(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      CircleAvatar(
                        backgroundColor: scheme.primaryContainer,
                        foregroundColor: scheme.onPrimaryContainer,
                        child: const Icon(Icons.system_update),
                      ),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text('Helper firmware',
                                style: Theme.of(context)
                                    .textTheme
                                    .titleMedium!
                                    .copyWith(fontWeight: FontWeight.w600)),
                            Text('OTA update over BLE',
                                style: Theme.of(context)
                                    .textTheme
                                    .bodySmall!
                                    .copyWith(
                                        color: scheme.onSurfaceVariant)),
                          ],
                        ),
                      ),
                      if (client.connected && bundled != null)
                        Container(
                          padding: const EdgeInsets.symmetric(
                              horizontal: 10, vertical: 6),
                          decoration: BoxDecoration(
                            color: updatable
                                ? scheme.primaryContainer
                                : scheme.secondaryContainer,
                            borderRadius: BorderRadius.circular(20),
                          ),
                          child: Text(
                            updatable ? 'Update available' : 'Up to date',
                            style: Theme.of(context)
                                .textTheme
                                .labelMedium!
                                .copyWith(
                                  fontWeight: FontWeight.w600,
                                  color: updatable
                                      ? scheme.onPrimaryContainer
                                      : scheme.onSecondaryContainer,
                                ),
                          ),
                        ),
                    ],
                  ),
                  const SizedBox(height: 14),
                  InfoRow(
                    label: 'Bundled image',
                    value: bundled == null
                        ? 'loading…'
                        : '${bundled.version == null ? "" : "v${bundled.version} · "}'
                            '${bundled.image.length} bytes',
                  ),
                  InfoRow(
                    label: 'Helper',
                    value: !client.connected
                        ? 'not connected'
                        : running == null
                            ? 'unknown version (pre-1.0.1 firmware)'
                            : 'v$running',
                  ),
                ],
              ),
            ),
            const SizedBox(height: 16),
            Center(
              child: FilledButton.icon(
                icon: const Icon(Icons.flash_on),
                label: const Text('Flash bundled firmware'),
                onPressed: client.connected && !client.busy && bundled != null
                    ? _flash
                    : null,
              ),
            ),
          ],
        );
      },
    );
  }
}
