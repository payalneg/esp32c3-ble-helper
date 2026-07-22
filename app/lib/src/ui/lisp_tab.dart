import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;

import '../ble/helper_client.dart';
import 'common.dart';

/// Upload the bundled LISP script to the VESC through the NUS bridge.
class LispTab extends StatefulWidget {
  final HelperBleClient client;
  const LispTab({super.key, required this.client});

  @override
  State<LispTab> createState() => _LispTabState();
}

class _LispTabState extends State<LispTab>
    with AutomaticKeepAliveClientMixin {
  Uint8List? _code;

  @override
  bool get wantKeepAlive => true;

  @override
  void initState() {
    super.initState();
    rootBundle.load('assets/lisp/main.lisp').then((data) {
      setState(() => _code = data.buffer.asUint8List());
    });
  }

  @override
  Widget build(BuildContext context) {
    super.build(context);
    final client = widget.client;
    return ListenableBuilder(
      listenable: client,
      builder: (context, _) {
        final theme = Theme.of(context);
        final scheme = theme.colorScheme;
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
                        child: const Icon(Icons.code),
                      ),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text('VESC LISP script',
                                style: theme.textTheme.titleMedium!
                                    .copyWith(fontWeight: FontWeight.w600)),
                            Text('Upload via the NUS bridge',
                                style: theme.textTheme.bodySmall!.copyWith(
                                    color: scheme.onSurfaceVariant)),
                          ],
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 14),
                  InfoRow(
                    label: 'Bundled script',
                    value: _code == null
                        ? 'loading…'
                        : 'main.lisp · ${_code!.length} bytes',
                  ),
                  const SizedBox(height: 8),
                  Text(
                    'lisp/main.lisp — motor arbiter + PAS + throttle toggle. '
                    'The script is written to the VESC and started right away.',
                    style: theme.textTheme.bodySmall!
                        .copyWith(color: scheme.onSurfaceVariant),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 16),
            Center(
              child: FilledButton.icon(
                icon: const Icon(Icons.upload),
                label: const Text('Upload bundled main.lisp'),
                onPressed: client.connected && !client.busy && _code != null
                    ? () => client.uploadLisp(_code!)
                    : null,
              ),
            ),
          ],
        );
      },
    );
  }
}
