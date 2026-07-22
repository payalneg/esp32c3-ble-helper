import 'dart:async';

import 'package:flutter/material.dart';

import '../ble/helper_client.dart';
import '../protocol/codec.dart';
import '../protocol/constants.dart';

/// Scan for / bind the BLE button remote and the cadence sensor.
class BindTab extends StatefulWidget {
  final HelperBleClient client;
  const BindTab({super.key, required this.client});

  @override
  State<BindTab> createState() => _BindTabState();
}

class _BindTabState extends State<BindTab>
    with AutomaticKeepAliveClientMixin {
  final hits = <ScanHit>[];
  int? selected;
  bool _scanned = false;
  late final StreamSubscription<ScanHit> _sub;

  @override
  bool get wantKeepAlive => true;

  @override
  void initState() {
    super.initState();
    _sub = widget.client.onScanHit.listen((hit) {
      // The helper streams every advertisement; merge repeats by address so
      // a device shows once (its name often arrives in a later report).
      setState(() {
        final i =
            hits.indexWhere((h) => h.what == hit.what && h.mac == hit.mac);
        if (i < 0) {
          hits.add(hit);
        } else if (hit.name.isNotEmpty || hits[i].name.isEmpty) {
          hits[i] = hit;
        }
      });
    });
  }

  @override
  void dispose() {
    _sub.cancel();
    super.dispose();
  }

  void _startScan(int what) {
    setState(() {
      hits.clear();
      selected = null;
      _scanned = true;
    });
    widget.client.scanRemotes(what);
  }

  static IconData _rssiIcon(int rssi) => rssi >= -60
      ? Icons.signal_cellular_alt
      : rssi >= -75
          ? Icons.signal_cellular_alt_2_bar
          : Icons.signal_cellular_alt_1_bar;

  @override
  Widget build(BuildContext context) {
    super.build(context);
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 12, 12, 8),
          child: Wrap(
            spacing: 8,
            runSpacing: 4,
            children: [
              FilledButton.tonalIcon(
                icon: const Icon(Icons.settings_remote),
                label: const Text('Scan for button'),
                onPressed: () => _startScan(kWhatButton),
              ),
              FilledButton.tonalIcon(
                icon: const Icon(Icons.pedal_bike),
                label: const Text('Scan for cadence sensor'),
                onPressed: () => _startScan(kWhatCadence),
              ),
            ],
          ),
        ),
        Expanded(
          child: hits.isEmpty
              ? Center(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Icon(Icons.bluetooth_searching,
                          size: 44, color: scheme.outline),
                      const SizedBox(height: 10),
                      Text(
                        _scanned
                            ? 'Nothing found yet — the helper scans for 6 s'
                            : 'No scan results yet',
                        style: theme.textTheme.bodyMedium!
                            .copyWith(color: scheme.onSurfaceVariant),
                      ),
                      if (!_scanned) ...[
                        const SizedBox(height: 4),
                        Text(
                          'Wake the device up and start a scan above.',
                          style: theme.textTheme.bodySmall!
                              .copyWith(color: scheme.onSurfaceVariant),
                        ),
                      ],
                    ],
                  ),
                )
              : ListView.separated(
                  padding: const EdgeInsets.symmetric(horizontal: 12),
                  itemCount: hits.length,
                  separatorBuilder: (_, _) => const SizedBox(height: 6),
                  itemBuilder: (context, i) {
                    final hit = hits[i];
                    final isSelected = selected == i;
                    return ListTile(
                      shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(14)),
                      tileColor: scheme.surfaceContainerLow,
                      selectedTileColor: scheme.secondaryContainer,
                      selected: isSelected,
                      leading: CircleAvatar(
                        backgroundColor: isSelected
                            ? scheme.secondary
                            : scheme.surfaceContainerHighest,
                        foregroundColor: isSelected
                            ? scheme.onSecondary
                            : scheme.onSurfaceVariant,
                        child: Icon(
                          hit.what == kWhatButton
                              ? Icons.settings_remote
                              : Icons.pedal_bike,
                          size: 20,
                        ),
                      ),
                      title: Text(hit.name.isEmpty ? '(unnamed)' : hit.name),
                      subtitle: Text(
                          '${hit.what == kWhatButton ? "button" : "cadence"}'
                          ' · ${hit.mac}'),
                      trailing: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          Icon(_rssiIcon(hit.rssi),
                              size: 18, color: scheme.onSurfaceVariant),
                          Text('${hit.rssi} dBm',
                              style: theme.textTheme.labelSmall!
                                  .copyWith(color: scheme.onSurfaceVariant)),
                        ],
                      ),
                      onTap: () => setState(() => selected = i),
                    );
                  },
                ),
        ),
        Padding(
          padding: const EdgeInsets.all(12),
          child: Wrap(
            spacing: 8,
            runSpacing: 4,
            alignment: WrapAlignment.center,
            children: [
              FilledButton.icon(
                icon: const Icon(Icons.link),
                label: const Text('Bind selected'),
                onPressed: selected == null
                    ? null
                    : () => widget.client.bind(hits[selected!]),
              ),
              OutlinedButton(
                onPressed: () => widget.client.unbind(kWhatCadence),
                child: const Text('Unbind sensor'),
              ),
              OutlinedButton(
                onPressed: () => widget.client.unbind(kWhatButton),
                child: const Text('Unbind remotes (all)'),
              ),
            ],
          ),
        ),
      ],
    );
  }
}
