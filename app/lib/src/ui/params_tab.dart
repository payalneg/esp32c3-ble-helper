import 'dart:async';

import 'package:flutter/material.dart';

import '../ble/helper_client.dart';
import '../protocol/codec.dart';
import '../protocol/constants.dart';
import 'common.dart';

/// PAS / CAN parameters + per-button CAN commands (Parameters tab).
class ParamsTab extends StatefulWidget {
  final HelperBleClient client;
  const ParamsTab({super.key, required this.client});

  @override
  State<ParamsTab> createState() => _ParamsTabState();
}

class _ParamsTabState extends State<ParamsTab>
    with AutomaticKeepAliveClientMixin {
  static const _pasFields = [
    ('start_current_pct', 'Start current, %'),
    ('start_delay_ms', 'Start delay, ms'),
    ('stop_delay_ms', 'Stop delay, ms'),
    ('min_cadence_rpm', 'Min cadence, rpm'),
    ('full_cadence_rpm', 'Full cadence, rpm'),
    ('max_current_a', 'Max current, A'),
    ('ramp_up_aps', 'Ramp, A/s'),
  ];

  // Booleans / enums are proper controls, not 0/1 text fields.
  bool _enabled = true;
  bool _reverse = false;
  int _mode = 0; // pas_mode_t: 0 switch, 1 proportional
  int _level = 1; // 0..level_count
  int _levelCount = 3; // 1..9

  final _pas = <String, TextEditingController>{};
  final _ctrlId = TextEditingController();
  final _tgtId = TextEditingController();
  int _kbps = 500;
  final _btnId = <TextEditingController>[];
  final _btnData = <TextEditingController>[];

  late final StreamSubscription<PasParams> _paramsSub;
  late final StreamSubscription<ButtonBinding> _bindingSub;

  @override
  bool get wantKeepAlive => true;

  @override
  void initState() {
    super.initState();
    for (final (key, _) in _pasFields) {
      _pas[key] = TextEditingController();
    }
    for (var i = 0; i < kBtnUiSlots; i++) {
      _btnId.add(TextEditingController(text: '123'));
      _btnData.add(TextEditingController(
          text: (i + 1).toRadixString(16).padLeft(4, '0').toUpperCase()));
    }
    _paramsSub = widget.client.onParams.listen(_fillParams);
    _bindingSub = widget.client.onBinding.listen(_fillBinding);
  }

  @override
  void dispose() {
    _paramsSub.cancel();
    _bindingSub.cancel();
    for (final c in _pas.values) {
      c.dispose();
    }
    for (final c in [..._btnId, ..._btnData, _ctrlId, _tgtId]) {
      c.dispose();
    }
    super.dispose();
  }

  void _fillParams(PasParams p) {
    _pas['start_current_pct']!.text = '${p.startCurrentPct}';
    _pas['start_delay_ms']!.text = '${p.startDelayMs}';
    _pas['stop_delay_ms']!.text = '${p.stopDelayMs}';
    _pas['min_cadence_rpm']!.text = '${p.minCadenceRpm}';
    _pas['full_cadence_rpm']!.text = '${p.fullCadenceRpm}';
    _pas['max_current_a']!.text = '${p.maxCurrentMa / 1000.0}';
    _pas['ramp_up_aps']!.text = '${p.rampUpMaps / 1000.0}';
    _ctrlId.text = '${p.controllerId}';
    _tgtId.text = '${p.targetVescId}';
    setState(() {
      _kbps = p.canKbps;
      _enabled = p.enabled != 0;
      _reverse = p.reverse != 0;
      _mode = p.mode == 1 ? 1 : 0;
      _levelCount = p.levelCount.clamp(1, 9);
      _level = p.level.clamp(0, _levelCount);
    });
  }

  void _fillBinding(ButtonBinding b) {
    if (b.idx >= kBtnUiSlots) return;
    _btnId[b.idx].text = b.canId.toRadixString(16).toUpperCase();
    _btnData[b.idx].text = hexBytes(b.data);
  }

  void _write() {
    final client = widget.client;
    try {
      int gi(String key) => int.parse(_pas[key]!.text.trim());
      double gd(String key) => double.parse(_pas[key]!.text.trim());

      final bindings = <ButtonBinding>[];
      for (var i = 0; i < kBtnUiSlots; i++) {
        final canId = int.parse(_btnId[i].text.trim(), radix: 16);
        var data = bytesFromHex(_btnData[i].text);
        if (data.length > 8) data = data.sublist(0, 8);
        bindings.add(ButtonBinding(i, canId > 0x7FF ? 1 : 0, canId, data));
      }
      final params = PasParams()
        ..enabled = _enabled ? 1 : 0
        ..reverse = _reverse ? 1 : 0
        ..level = _level
        ..levelCount = _levelCount
        ..mode = _mode
        ..startCurrentPct = gi('start_current_pct')
        ..startDelayMs = gi('start_delay_ms')
        ..stopDelayMs = gi('stop_delay_ms')
        ..minCadenceRpm = gi('min_cadence_rpm')
        ..fullCadenceRpm = gi('full_cadence_rpm')
        ..maxCurrentMa = (gd('max_current_a') * 1000).round()
        ..rampUpMaps = (gd('ramp_up_aps') * 1000).round()
        ..controllerId = int.parse(_ctrlId.text.trim())
        ..targetVescId = int.parse(_tgtId.text.trim())
        ..canKbps = _kbps;

      client.setParams(params.encode());
      for (final b in bindings) {
        client.setBinding(b);
      }
    } on FormatException {
      ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Check the field values')));
    }
  }

  Widget _numField(TextEditingController controller, String label,
      {double width = 170, bool hex = false}) {
    return SizedBox(
      width: width,
      child: TextField(
        controller: controller,
        keyboardType: hex
            ? TextInputType.text
            : const TextInputType.numberWithOptions(decimal: true),
        decoration: InputDecoration(labelText: label),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    super.build(context);
    return ListView(
      padding: const EdgeInsets.all(12),
      children: [
        SectionCard(
          title: 'CAN bus',
          child: Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              _numField(_ctrlId, 'Helper ID', width: 110),
              _numField(_tgtId, 'VESC ID', width: 110),
              SizedBox(
                width: 140,
                child: DropdownButtonFormField<int>(
                  value: _kbps,
                  decoration:
                      const InputDecoration(labelText: 'Bitrate, kbps'),
                  items: [
                    for (final speed in kCanSpeeds)
                      DropdownMenuItem(value: speed, child: Text('$speed')),
                  ],
                  onChanged: (v) => setState(() => _kbps = v ?? 500),
                ),
              ),
            ],
          ),
        ),
        const SizedBox(height: 12),
        SectionCard(
          title: 'PAS',
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              SwitchListTile(
                dense: true,
                contentPadding: EdgeInsets.zero,
                title: const Text('PAS enabled'),
                value: _enabled,
                onChanged: (v) => setState(() => _enabled = v),
              ),
              SwitchListTile(
                dense: true,
                contentPadding: EdgeInsets.zero,
                title: const Text('Sensor reversed'),
                subtitle: const Text('Flip if assist engages when back-pedaling'),
                value: _reverse,
                onChanged: (v) => setState(() => _reverse = v),
              ),
              const SizedBox(height: 8),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  SizedBox(
                    width: 240,
                    child: DropdownButtonFormField<int>(
                      value: _mode,
                      isExpanded: true,
                      decoration: const InputDecoration(labelText: 'Mode'),
                      items: const [
                        DropdownMenuItem(
                            value: 0, child: Text('Switch (levels)')),
                        DropdownMenuItem(
                            value: 1, child: Text('Proportional (cadence)')),
                      ],
                      onChanged: (v) => setState(() => _mode = v ?? 0),
                    ),
                  ),
                  SizedBox(
                    width: 130,
                    child: DropdownButtonFormField<int>(
                      value: _levelCount,
                      decoration:
                          const InputDecoration(labelText: 'Level count'),
                      items: [
                        for (var n = 1; n <= 9; n++)
                          DropdownMenuItem(value: n, child: Text('$n')),
                      ],
                      onChanged: (v) => setState(() {
                        _levelCount = v ?? _levelCount;
                        if (_level > _levelCount) _level = _levelCount;
                      }),
                    ),
                  ),
                  SizedBox(
                    width: 130,
                    child: DropdownButtonFormField<int>(
                      value: _level,
                      decoration:
                          const InputDecoration(labelText: 'Assist level'),
                      items: [
                        for (var n = 0; n <= _levelCount; n++)
                          DropdownMenuItem(
                              value: n, child: Text(n == 0 ? '0 — off' : '$n')),
                      ],
                      onChanged: (v) => setState(() => _level = v ?? _level),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 12),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  for (final (key, label) in _pasFields)
                    _numField(_pas[key]!, label),
                ],
              ),
            ],
          ),
        ),
        const SizedBox(height: 12),
        SectionCard(
          title: 'Button CAN commands',
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              for (var i = 0; i < kBtnUiSlots; i++)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 4),
                  child: Row(
                    children: [
                      SizedBox(
                          width: 68,
                          child:
                              Text('Button ${String.fromCharCode(65 + i)}')),
                      Expanded(
                          child: _numField(_btnId[i], 'CAN ID (hex)',
                              width: double.infinity, hex: true)),
                      const SizedBox(width: 8),
                      Expanded(
                          child: _numField(_btnData[i], 'data (hex)',
                              width: double.infinity, hex: true)),
                    ],
                  ),
                ),
              const SizedBox(height: 4),
              Text(
                'Buttons are learned by first press across ALL bound '
                'remotes (first ever pressed = A, next = B, …; watch the '
                'Status tab).\nWith lisp/main.lisp: ID=123 data=0001 '
                'toggles the throttle, data=0002 switches the speed '
                'profile; add your own commands in proc-helper-btn (or '
                'see lisp/can_button_skeleton.lisp).',
                style: Theme.of(context)
                    .textTheme
                    .bodySmall!
                    .copyWith(color: Theme.of(context).hintColor),
              ),
            ],
          ),
        ),
        const SizedBox(height: 12),
        Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            OutlinedButton.icon(
              icon: const Icon(Icons.refresh),
              label: const Text('Read'),
              onPressed: widget.client.getParams,
            ),
            const SizedBox(width: 16),
            FilledButton.icon(
              icon: const Icon(Icons.save_outlined),
              label: const Text('Write'),
              onPressed: _write,
            ),
          ],
        ),
        const SizedBox(height: 8),
      ],
    );
  }
}
