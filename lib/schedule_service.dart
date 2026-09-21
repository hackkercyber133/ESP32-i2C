import 'dart:convert';
import 'package:shared_preferences/shared_preferences.dart';

/// ===== JADWAL OTOMATIS =====
/// Satu aturan = pada jam tertentu (di hari-hari tertentu dalam seminggu),
/// otomatis ganti voltase cooler terkait.
///
/// Eksekusi sebenarnya sekarang dilakukan MANDIRI oleh firmware ESP32 sendiri
/// (disimpan di flash/NVS-nya), bukan oleh app lagi — supaya tetap jalan
/// walau app ditutup atau BLE/WiFi ke ESP32 terputus. App di sini cuma jadi
/// editor UI + pengirim salinan jadwal ke ESP32 setiap kali ada perubahan,
/// lewat callback [onSaved] di bawah (didaftarkan oleh main.dart).
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

  /// Didaftarkan sekali oleh main.dart (yang punya akses koneksi BLE/WiFi ke
  /// ESP32). Dipanggil otomatis setiap [saveAll] selesai, supaya SETIAP jalur
  /// penyimpanan jadwal (tambah/edit/hapus/toggle/import) otomatis ikut
  /// mendorong salinan terbaru ke ESP32 tanpa perlu dipanggil manual di
  /// masing-masing tempat.
  static Future<void> Function(List<ScheduleRule> all)? onSaved;

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
    if (onSaved != null) {
      try {
        await onSaved!(rules);
      } catch (_) {
        // Gagal kirim ke ESP32 (mis. lagi offline) tidak boleh menghalangi
        // penyimpanan lokal - nanti disinkronkan lagi saat konek berikutnya.
      }
    }
  }
}
