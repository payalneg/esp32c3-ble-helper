/// Protocol constants shared with the firmware and tools/config_gui.py.
library;

const kDeviceName = 'VESC-BLE-Helper';

/// Advertised service (the name only appears in the scan response, so the
/// phone-side scan filters on this UUID instead).
const kNusServiceUuid = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';

const kCfgCtrlUuid = 'ab1e0002-b1e5-4e15-8ac3-5e00c0de15b7';
const kCfgStatusUuid = 'ab1e0003-b1e5-4e15-8ac3-5e00c0de15b7';
const kCfgScanUuid = 'ab1e0004-b1e5-4e15-8ac3-5e00c0de15b7';
const kOtaCtrlUuid = 'ab1e0005-b1e5-4e15-8ac3-5e00c0de15b7';
const kOtaDataUuid = 'ab1e0006-b1e5-4e15-8ac3-5e00c0de15b7';
const kNusRxUuid = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // write → helper → CAN
const kNusTxUuid = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // notify ← VESC

/// Device Information Service: Firmware Revision String. Read on connect and
/// compared with the bundled image version; absent on firmware < 1.0.1.
const kFwRevisionUuid = '2a26';

/// Config-service CTRL commands.
abstract final class Cmd {
  static const scan = 1;
  static const bindButton = 2;
  static const bindCadence = 3;
  static const unbind = 4;
  static const getParams = 5;
  static const setParams = 6;
  static const setThrottle = 7;
  static const setBinding = 8;
  static const getBinding = 9;
}

const kWhatButton = 1;
const kWhatCadence = 2;

/// OTA ops and notification statuses.
abstract final class Ota {
  static const opBegin = 1;
  static const opEnd = 2;
  static const opAbort = 3;
  static const stReady = 0x10;
  static const stProgress = 0x11;
  static const stDone = 0x12;
  static const stError = 0x1F;
}

/// VESC COMM ids used by the LISP upload over NUS.
abstract final class VescComm {
  static const lispWriteCode = 131;
  static const lispEraseCode = 132;
  static const lispSetRunning = 133;
}

/// btn_action_t (settings.h): every button fires a configurable raw CAN
/// frame; the semantics live in the LISP script on the VESC.
const kBtnActCustomCan = 4;
const kBtnUiSlots = 8;

const kCanSpeeds = [125, 250, 500, 1000];
