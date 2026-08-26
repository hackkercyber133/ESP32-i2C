import 'dart:convert';
import 'package:shared_preferences/shared_preferences.dart';

/// ===== JADWAL OTOMATIS =====
/// Satu aturan = pada jam tertentu (di hari-hari tertentu dalam seminggu),
/// otomatis kirim perintah ganti voltase ke cooler terkait.
///
/// CATATAN PENTING: eksekusi jadwal (pengiriman perintah MQTT/BLE ke
/// hardware) hanya berjalan selama APLIKASI TERBUKA, karena mengirim
/// perintah butuh koneksi WiFi/Bluetooth yang hidup di dalam app —
/// bukan lewat server/cloud. Notifikasi lokal tetap muncul sesuai jadwal
/// sebagai pengingat, tapi voltase baru benar-benar berubah kalau app
/// aktif (foreground/background biasa) saat jadwal itu tiba.
class ScheduleRule {
  final String id;
  final String coolerId;
  final int hour; // 0-23
  final int minute; // 0-59
  final double voltage;
  final List<int> days; // DateTime.weekday: 1=Senin .. 7=Minggu
  bool enabled;
  String lastFiredDateKey; // "yyyy-M-d", cegah dobel-trigger di hari yang sama

  ScheduleRule({
    required this.id,
    required this.coolerId,
    required this.hour,
    required this.minute,
    required this.voltage,
    required this.days,
    this.enabled = true,
    this.lastFiredDateKey = "",
  });

  Map<String, dynamic> toJson() => {
        "id": id,
        "coolerId": coolerId,
        "hour": hour,
        "minute": minute,
        "voltage": voltage,
        "days": days,
        "enabled": enabled,
        "lastFiredDateKey": lastFiredDateKey,
      };

  factory ScheduleRule.fromJson(Map<String, dynamic> j) => ScheduleRule(
        id: j["id"].toString(),
        coolerId: j["coolerId"] ?? "",
        hour: j["hour"],
        minute: j["minute"],
        voltage: (j["voltage"] as num).toDouble(),
        days: (j["days"] as List).map((e) => e as int).toList(),
        enabled: j["enabled"] ?? true,
        lastFiredDateKey: j["lastFiredDateKey"] ?? "",
      );
}

class ScheduleService {
  ScheduleService._();
  static const _prefKey = "auto_schedules";

  static Future<List<ScheduleRule>> loadAll() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_prefKey);
    if (raw == null) return [];
    try {
      final list = jsonDecode(raw) as List;
      return list.map((e) => ScheduleRule.fromJson(e)).toList();
    } catch (_) {
      return [];
    }
  }

  static Future<void> saveAll(List<ScheduleRule> rules) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_prefKey, jsonEncode(rules.map((e) => e.toJson()).toList()));
  }
}
