import 'package:flutter/material.dart';

import '../ble/helper_client.dart';

/// Collapsible log strip pinned to the bottom of the screen. Collapsed it
/// shows the latest line as a one-line ticker; tap to expand the history.
class LogPanel extends StatefulWidget {
  final HelperBleClient client;
  const LogPanel({super.key, required this.client});

  @override
  State<LogPanel> createState() => _LogPanelState();
}

class _LogPanelState extends State<LogPanel> {
  bool _expanded = false;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final logStyle = TextStyle(
        fontFamily: 'monospace', fontSize: 12, color: scheme.onSurfaceVariant);
    return Container(
      decoration: BoxDecoration(
        color: scheme.surfaceContainer,
        border: Border(top: BorderSide(color: scheme.outlineVariant)),
      ),
      child: SafeArea(
        top: false,
        child: ListenableBuilder(
          listenable: widget.client,
          builder: (context, _) {
            final lines = widget.client.logLines;
            return Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                InkWell(
                  onTap: () => setState(() => _expanded = !_expanded),
                  child: Padding(
                    padding: const EdgeInsets.symmetric(
                        horizontal: 12, vertical: 6),
                    child: Row(
                      children: [
                        Icon(Icons.terminal,
                            size: 16, color: scheme.onSurfaceVariant),
                        const SizedBox(width: 8),
                        Expanded(
                          child: Text(
                            _expanded || lines.isEmpty ? 'Log' : lines.last,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: logStyle,
                          ),
                        ),
                        Icon(
                          _expanded ? Icons.expand_more : Icons.expand_less,
                          size: 18,
                          color: scheme.onSurfaceVariant,
                        ),
                      ],
                    ),
                  ),
                ),
                if (_expanded)
                  SizedBox(
                    height: 150,
                    child: ListView.builder(
                      // reverse:true keeps the newest line visible without
                      // manual scroll-to-end bookkeeping.
                      reverse: true,
                      padding: const EdgeInsets.fromLTRB(12, 0, 12, 6),
                      itemCount: lines.length,
                      itemBuilder: (context, i) => Text(
                          lines[lines.length - 1 - i],
                          style: logStyle),
                    ),
                  ),
              ],
            );
          },
        ),
      ),
    );
  }
}
