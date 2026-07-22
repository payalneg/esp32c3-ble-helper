/// Awaitable FIFO with timeout — replaces asyncio.Queue from the Python GUI
/// for OTA status and NUS reply round-trips.
library;

import 'dart:async';
import 'dart:collection';

class EventQueue<T> {
  final _items = Queue<T>();
  final _waiters = Queue<Completer<T>>();

  void add(T value) {
    while (_waiters.isNotEmpty) {
      final waiter = _waiters.removeFirst();
      if (waiter.isCompleted) continue; // timed out, already failed
      waiter.complete(value);
      return;
    }
    _items.add(value);
  }

  Future<T> next(Duration timeout) {
    if (_items.isNotEmpty) return Future.value(_items.removeFirst());
    if (timeout <= Duration.zero) {
      return Future.error(TimeoutException('event queue', timeout));
    }
    final completer = Completer<T>();
    _waiters.add(completer);
    return completer.future.timeout(timeout, onTimeout: () {
      _waiters.remove(completer);
      throw TimeoutException('event queue', timeout);
    });
  }

  void clear() => _items.clear();
}
