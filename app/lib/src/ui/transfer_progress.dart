import 'package:flutter/material.dart';

import '../ble/helper_client.dart';

/// Global transfer banner pinned above the log strip: visible on every tab
/// while an OTA / LISP transfer runs (they can start from the update dialog,
/// far from the Firmware tab). Indeterminate while the helper erases the
/// partition, then percent + bytes.
class TransferBanner extends StatelessWidget {
  final HelperBleClient client;
  const TransferBanner({super.key, required this.client});

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: client,
      builder: (context, _) {
        if (!client.busy) return const SizedBox.shrink();
        final theme = Theme.of(context);
        final scheme = theme.colorScheme;
        final p = client.progress;
        final value =
            p == null || p.total == 0 ? null : p.done / p.total;
        return Container(
          width: double.infinity,
          color: scheme.surfaceContainer,
          padding: const EdgeInsets.fromLTRB(12, 8, 12, 10),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Row(
                children: [
                  Icon(Icons.sync, size: 16, color: scheme.primary),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      value == null
                          ? 'Transfer: preparing…'
                          : 'Transferring… ${(value * 100).toStringAsFixed(0)} %',
                      style: theme.textTheme.labelMedium!
                          .copyWith(fontWeight: FontWeight.w600),
                    ),
                  ),
                  if (p != null)
                    Text(
                      '${p.done} / ${p.total} bytes',
                      style: theme.textTheme.labelSmall!
                          .copyWith(color: scheme.onSurfaceVariant),
                    ),
                ],
              ),
              const SizedBox(height: 8),
              ClipRRect(
                borderRadius: BorderRadius.circular(4),
                child: LinearProgressIndicator(value: value, minHeight: 6),
              ),
            ],
          ),
        );
      },
    );
  }
}
