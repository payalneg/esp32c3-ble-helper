import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../ble/helper_client.dart';
import '../protocol/codec.dart';
import 'common.dart';

/// Live dashboard: cadence gauge, assist stats, link health, learned
/// buttons and the throttle override.
class StatusTab extends StatelessWidget {
  final HelperBleClient client;
  const StatusTab({super.key, required this.client});

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: client,
      builder: (context, _) {
        final s = client.status;
        if (s == null) return _EmptyState(connected: client.connected);
        return ListView(
          padding: const EdgeInsets.all(12),
          children: [
            _CadenceCard(s: s),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: _StatTile(
                      label: 'Assist',
                      value: s.assistA.toStringAsFixed(1),
                      unit: 'A'),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: _StatTile(label: 'Level', value: '${s.level}'),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: _StatTile(
                      label: 'Battery',
                      value: s.batt == 0xFF ? '—' : '${s.batt}',
                      unit: s.batt == 0xFF ? null : '%'),
                ),
              ],
            ),
            const SizedBox(height: 12),
            SectionCard(
              title: 'Links',
              child: Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  _LinkChip(
                    icon: Icons.pedal_bike,
                    label: 'Sensor',
                    detail: !s.cadenceBound
                        ? 'not bound'
                        : s.cadenceConnected
                            ? 'connected'
                            : 'offline',
                    state: !s.cadenceBound
                        ? _Link.off
                        : s.cadenceConnected
                            ? _Link.ok
                            : _Link.warn,
                  ),
                  _LinkChip(
                    icon: Icons.settings_remote,
                    label: 'Remote',
                    detail: !s.remoteBound
                        ? 'not bound'
                        : s.remoteConnected
                            ? 'connected'
                            : 'offline',
                    state: !s.remoteBound
                        ? _Link.off
                        : s.remoteConnected
                            ? _Link.ok
                            : _Link.warn,
                  ),
                  _LinkChip(
                    icon: Icons.cable,
                    label: 'VESC',
                    detail: s.vescLink ? 'link ok' : 'no data',
                    state: s.vescLink ? _Link.ok : _Link.warn,
                  ),
                  _LinkChip(
                    icon: Icons.directions_bike,
                    label: 'PAS',
                    detail: s.pasEnabled ? 'on' : 'off',
                    state: s.pasEnabled ? _Link.ok : _Link.off,
                  ),
                ],
              ),
            ),
            const SizedBox(height: 12),
            SectionCard(title: 'Remote buttons', child: _ButtonsRow(s: s)),
            const SizedBox(height: 12),
            _ThrottleCard(client: client, s: s),
          ],
        );
      },
    );
  }
}

/// Hero cadence figure inside a 270° meter arc; the track is a lighter
/// step of the same hue so the gauge reads as one unit.
class _CadenceCard extends StatelessWidget {
  final StatusFrame s;
  const _CadenceCard({required this.s});

  static const _maxRpm = 120.0;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    final active = s.cadenceConnected;
    return SectionCard(
      child: Column(
        children: [
          Row(
            children: [
              Text('Cadence',
                  style: theme.textTheme.titleSmall!
                      .copyWith(color: scheme.onSurfaceVariant)),
              const Spacer(),
              if (!active)
                Text('sensor offline',
                    style: theme.textTheme.labelMedium!
                        .copyWith(color: scheme.onSurfaceVariant)),
            ],
          ),
          SizedBox(
            height: 200,
            child: TweenAnimationBuilder<double>(
              tween: Tween(end: active ? s.rpm.clamp(0.0, _maxRpm) : 0.0),
              duration: const Duration(milliseconds: 350),
              curve: Curves.easeOutCubic,
              builder: (context, rpm, _) => CustomPaint(
                painter: _GaugePainter(
                  fraction: rpm / _maxRpm,
                  track: active
                      ? scheme.primaryContainer
                      : scheme.surfaceContainerHighest,
                  fill: scheme.primary,
                ),
                child: Center(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Text(
                        active ? '${rpm.round()}' : '—',
                        style: TextStyle(
                          fontSize: 56,
                          height: 1.0,
                          fontWeight: FontWeight.w600,
                          color: active ? scheme.onSurface : scheme.outline,
                        ),
                      ),
                      const SizedBox(height: 4),
                      Text('RPM',
                          style: theme.textTheme.labelLarge!.copyWith(
                              color: scheme.onSurfaceVariant,
                              letterSpacing: 3)),
                    ],
                  ),
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _GaugePainter extends CustomPainter {
  final double fraction;
  final Color track;
  final Color fill;
  const _GaugePainter(
      {required this.fraction, required this.track, required this.fill});

  static const _stroke = 18.0;
  static final _start = 135 * math.pi / 180;
  static final _sweep = 270 * math.pi / 180;

  @override
  void paint(Canvas canvas, Size size) {
    final side = math.min(size.width, size.height);
    final rect = Rect.fromCenter(
      center: Offset(size.width / 2, size.height / 2),
      width: side - _stroke,
      height: side - _stroke,
    );
    final paint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = _stroke
      ..strokeCap = StrokeCap.round;
    canvas.drawArc(rect, _start, _sweep, false, paint..color = track);
    final f = fraction.clamp(0.0, 1.0);
    if (f > 0.004) {
      canvas.drawArc(rect, _start, _sweep * f, false, paint..color = fill);
    }
  }

  @override
  bool shouldRepaint(_GaugePainter old) =>
      old.fraction != fraction || old.track != track || old.fill != fill;
}

/// Stat tile: muted label over a prominent value (+ optional unit).
class _StatTile extends StatelessWidget {
  final String label;
  final String value;
  final String? unit;
  const _StatTile({required this.label, required this.value, this.unit});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    return SectionCard(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: theme.textTheme.labelMedium!
                  .copyWith(color: scheme.onSurfaceVariant)),
          const SizedBox(height: 6),
          Text.rich(
            TextSpan(
              text: value,
              style: theme.textTheme.headlineSmall!
                  .copyWith(fontWeight: FontWeight.w600),
              children: [
                if (unit != null)
                  TextSpan(
                    text: ' $unit',
                    style: theme.textTheme.bodySmall!
                        .copyWith(color: scheme.onSurfaceVariant),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

enum _Link { ok, warn, off }

/// State chip: icon + wording carry the state, color only reinforces it.
class _LinkChip extends StatelessWidget {
  final IconData icon;
  final String label;
  final String detail;
  final _Link state;
  const _LinkChip(
      {required this.icon,
      required this.label,
      required this.detail,
      required this.state});

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final (bg, fg) = switch (state) {
      _Link.ok => (scheme.secondaryContainer, scheme.onSecondaryContainer),
      _Link.warn => (scheme.errorContainer, scheme.onErrorContainer),
      _Link.off => (scheme.surfaceContainerHighest, scheme.onSurfaceVariant),
    };
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: bg,
        borderRadius: BorderRadius.circular(20),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 16, color: fg),
          const SizedBox(width: 6),
          Text('$label · $detail',
              style: Theme.of(context)
                  .textTheme
                  .labelMedium!
                  .copyWith(color: fg, fontWeight: FontWeight.w600)),
        ],
      ),
    );
  }
}

class _ButtonsRow extends StatelessWidget {
  final StatusFrame s;
  const _ButtonsRow({required this.s});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    if (s.btnCount == 0) {
      return Text(
        'No buttons learned yet — press any button on a bound remote.',
        style: theme.textTheme.bodySmall!
            .copyWith(color: scheme.onSurfaceVariant),
      );
    }
    final count = math.min(s.btnCount, 8);
    return Wrap(
      spacing: 8,
      runSpacing: 8,
      children: [
        for (var i = 0; i < count; i++)
          AnimatedContainer(
            duration: const Duration(milliseconds: 120),
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              color: s.btnMask & (1 << i) != 0
                  ? scheme.primary
                  : scheme.surfaceContainerHighest,
              borderRadius: BorderRadius.circular(12),
            ),
            child: Center(
              child: Text(
                String.fromCharCode('A'.codeUnitAt(0) + i),
                style: TextStyle(
                  fontWeight: FontWeight.w600,
                  color: s.btnMask & (1 << i) != 0
                      ? scheme.onPrimary
                      : scheme.onSurfaceVariant,
                ),
              ),
            ),
          ),
      ],
    );
  }
}

class _ThrottleCard extends StatelessWidget {
  final HelperBleClient client;
  final StatusFrame s;
  const _ThrottleCard({required this.client, required this.s});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    return SectionCard(
      title: 'Throttle override',
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Wrap(
            spacing: 8,
            runSpacing: 8,
            crossAxisAlignment: WrapCrossAlignment.center,
            children: [
              SegmentedButton<bool>(
                segments: const [
                  ButtonSegment(
                      value: true,
                      label: Text('On'),
                      icon: Icon(Icons.bolt)),
                  ButtonSegment(
                      value: false,
                      label: Text('Off'),
                      icon: Icon(Icons.power_settings_new)),
                ],
                selected: {s.throttleOn},
                onSelectionChanged: (sel) =>
                    client.throttle(sel.first ? 1 : 0),
              ),
              OutlinedButton(
                onPressed: () => client.throttle(0xFF),
                child: const Text('Toggle'),
              ),
            ],
          ),
          if (!s.vescLink) ...[
            const SizedBox(height: 10),
            Row(
              children: [
                Icon(Icons.warning_amber, size: 16, color: scheme.error),
                const SizedBox(width: 6),
                Expanded(
                  child: Text(
                    'No link to VESC — the override may not reach the motor.',
                    style: theme.textTheme.bodySmall!
                        .copyWith(color: scheme.error),
                  ),
                ),
              ],
            ),
          ],
        ],
      ),
    );
  }
}

class _EmptyState extends StatelessWidget {
  final bool connected;
  const _EmptyState({required this.connected});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(
              connected ? Icons.hourglass_top : Icons.bluetooth_disabled,
              size: 48,
              color: scheme.outline,
            ),
            const SizedBox(height: 12),
            Text(connected ? 'Waiting for status…' : 'Not connected',
                style: theme.textTheme.titleMedium),
            const SizedBox(height: 4),
            Text(
              connected
                  ? 'The helper streams live data once it is ready.'
                  : 'Connect to the helper to see the live dashboard.',
              textAlign: TextAlign.center,
              style: theme.textTheme.bodySmall!
                  .copyWith(color: scheme.onSurfaceVariant),
            ),
          ],
        ),
      ),
    );
  }
}
