import 'dart:convert';
import 'package:shared_preferences/shared_preferences.dart';

/// ===== RIWAYAT PEMAKAIAN =====
/// Satu "sesi" dicatat setiap kali voltase berubah atau device jadi
/// online/offline. Durasi sesi = selisih waktu sampai sesi berikutnya
/// dimulai (atau sampai sekarang, kalau sesi masih berjalan).
///
/// Data yang dicatat murni dari status ASLI yang sudah diterima app dari
/// device (lewat MQTT/BLE) — bukan simulasi — dan disimpan LOKAL saja di
/// HP (SharedPreferences), tidak dikirim ke server manapun.
class UsageSession {
  final String coolerId;
  final double voltage;
  final DateTime start;
  DateTime end;

  UsageSession({
    required this.coolerId,
    required this.voltage,
    required this.start,
    required this.end,
  });

  Duration get duration => end.difference(start);

  Map<String, dynamic> toJson() => {
        "coolerId": coolerId,
        "voltage": voltage,
        "start": start.toIso8601String(),
        "end": end.toIso8601String(),
      };

  factory UsageSession.fromJson(Map<String, dynamic> j) => UsageSession(
        coolerId: j["coolerId"] ?? "",
        voltage: (j["voltage"] as num).toDouble(),
        start: DateTime.parse(j["start"]),
        end: DateTime.parse(j["end"]),
      );
}

class HistoryService {
  HistoryService._();

  static const _prefKey = "usage_history_sessions";
  // Simpan maksimal 45 hari ke belakang supaya penyimpanan lokal tidak
  // membengkak terus-menerus.
  static const int _maxAgeDays = 45;

  static UsageSession? _open; // sesi yang sedang berjalan (belum ditutup)

  /// Dipanggil setiap kali status baru diterima dari device (MQTT/BLE).
  /// Kalau voltase / status online berubah dari sesi terakhir, sesi lama
  /// ditutup & dicatat, sesi baru dimulai.
  static Future<void> recordStatus({
    required String coolerId,
    required bool online,
    required double voltage,
  }) async {
    if (coolerId.isEmpty) return;
    final now = DateTime.now();
    if (!online) {
      await _closeOpen();
      return;
    }
    if (_open != null && _open!.coolerId == coolerId && _open!.voltage == voltage) {
      _open!.end = now; // masih sesi yang sama, cukup update waktu akhir
      return;
    }
    await _closeOpen();
    _open = UsageSession(coolerId: coolerId, voltage: voltage, start: now, end: now);
  }

  static Future<void> _closeOpen() async {
    if (_open == null) return;
    final s = _open!;
    _open = null;
    if (s.duration.inSeconds < 5) return; // buang sesi super pendek (noise)
    final sessions = await loadAll();
    sessions.add(s);
    await _saveAll(sessions);
  }

  /// Panggil berkala (mis. tiap menit lewat timer) supaya sesi yang sedang
  /// berjalan lama tetap ter-update waktu akhirnya — jadi kalau app
  /// ditutup paksa tiba-tiba, data yang sudah tersimpan tidak basi/hilang.
  static void touchOpenSession() {
    if (_open == null) return;
    _open!.end = DateTime.now();
  }

  static Future<List<UsageSession>> loadAll() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_prefKey);
    if (raw == null) return [];
    try {
      final list = jsonDecode(raw) as List;
      final cutoff = DateTime.now().subtract(const Duration(days: _maxAgeDays));
      return list
          .map((e) => UsageSession.fromJson(e))
          .where((s) => s.end.isAfter(cutoff))
          .toList();
    } catch (_) {
      return [];
    }
  }

  static Future<void> _saveAll(List<UsageSession> sessions) async {
    final prefs = await SharedPreferences.getInstance();
    final cutoff = DateTime.now().subtract(const Duration(days: _maxAgeDays));
    final trimmed = sessions.where((s) => s.end.isAfter(cutoff)).toList();
    await prefs.setString(_prefKey, jsonEncode(trimmed.map((e) => e.toJson()).toList()));
  }

  static Future<void> clearAll() async {
    _open = null;
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_prefKey);
  }

  /// Total durasi nyala per hari (7 hari terakhir termasuk hari ini) dalam
  /// satuan jam, untuk cooler tertentu. Dipakai untuk grafik batang.
  static Future<Map<DateTime, double>> dailyHoursLast7Days(String coolerId) async {
    touchOpenSession();
    final sessions = (await loadAll()).where((s) => s.coolerId == coolerId).toList();
    if (_open != null && _open!.coolerId == coolerId) sessions.add(_open!);
    final today = DateTime.now();
    final Map<DateTime, double> result = {};
    for (int i = 6; i >= 0; i--) {
      final day = DateTime(today.year, today.month, today.day).subtract(Duration(days: i));
      result[day] = 0;
    }
    for (final s in sessions) {
      final day = DateTime(s.start.year, s.start.month, s.start.day);
      if (result.containsKey(day)) {
        result[day] = (result[day] ?? 0) + s.duration.inMinutes / 60.0;
      }
    }
    return result;
  }

  /// Statistik ringkas: total jam nyala & voltase paling sering dipakai
  /// (berdasar total durasi terkumpul, bukan jumlah kejadian).

  /// Rata-rata voltase berbobot durasi per hari (7 hari terakhir).
  /// Dipakai untuk grafik voltase harian.
  static Future<Map<DateTime, double>> dailyAverageVoltageLast7Days(String coolerId) async {
    touchOpenSession();
    final sessions = (await loadAll()).where((s) => s.coolerId == coolerId).toList();
    if (_open != null && _open!.coolerId == coolerId) sessions.add(_open!);
    final today = DateTime.now();
    final Map<DateTime, double> weighted = {};
    final Map<DateTime, double> hours = {};
    for (int i = 6; i >= 0; i--) {
      final day = DateTime(today.year, today.month, today.day).subtract(Duration(days: i));
      weighted[day] = 0;
      hours[day] = 0;
    }
    for (final s in sessions) {
      var cursor = s.start;
      while (cursor.isBefore(s.end)) {
        final nextDay = DateTime(cursor.year, cursor.month, cursor.day).add(const Duration(days: 1));
        final end = s.end.isBefore(nextDay) ? s.end : nextDay;
        final h = end.difference(cursor).inSeconds / 3600.0;
        final key = DateTime(cursor.year, cursor.month, cursor.day);
        if (weighted.containsKey(key)) {
          weighted[key] = (weighted[key] ?? 0) + s.voltage * h;
          hours[key] = (hours[key] ?? 0) + h;
        }
        cursor = end;
      }
    }
    return {for (final e in weighted.entries) e.key: (hours[e.key] ?? 0) > 0 ? e.value / hours[e.key]! : 0};
  }

  static Future<Map<String, dynamic>> summary(String coolerId) async {
    touchOpenSession();
    final sessions = (await loadAll()).where((s) => s.coolerId == coolerId).toList();
    if (_open != null && _open!.coolerId == coolerId) sessions.add(_open!);

    double totalHours = 0;
    final Map<double, double> perVoltage = {};
    for (final s in sessions) {
      final h = s.duration.inMinutes / 60.0;
      totalHours += h;
      perVoltage[s.voltage] = (perVoltage[s.voltage] ?? 0) + h;
    }
    double? mostUsed;
    double bestHours = -1;
    perVoltage.forEach((v, h) {
      if (h > bestHours) {
        bestHours = h;
        mostUsed = v;
      }
    });
    return {
      "totalHours": totalHours,
      "mostUsedVoltage": mostUsed,
      "perVoltage": perVoltage,
    };
  }
}
