import 'dart:convert';
import 'dart:io';
import 'package:file_picker/file_picker.dart';
import 'package:path_provider/path_provider.dart';

/// ===== EXPORT / IMPORT KONFIGURASI =====
/// Membungkus daftar cooler + tema + jadwal otomatis jadi satu file
/// `.json`, supaya gampang dipindah pas ganti HP atau install ulang app.
class BackupService {
  BackupService._();

  static Map<String, dynamic> buildPayload({
    required List<Map<String, dynamic>> coolers,
    required int accentColorValue,
    required String themeMode, // "dark" | "light"
    required List<Map<String, dynamic>> schedules,
  }) {
    return {
      "app": "fan_cooler_app",
      "exportVersion": 1,
      "exportedAt": DateTime.now().toIso8601String(),
      "coolers": coolers,
      "accentColor": accentColorValue,
      "themeMode": themeMode,
      "schedules": schedules,
    };
  }

  /// Simpan payload ke file .json di penyimpanan lokal app (tidak butuh
  /// izin storage khusus). Kembalikan path lengkap file yang tersimpan.
  static Future<String> exportToFile(Map<String, dynamic> payload) async {
    final jsonStr = const JsonEncoder.withIndent('  ').convert(payload);
    Directory dir;
    try {
      dir = await getApplicationDocumentsDirectory();
    } catch (_) {
      dir = Directory.systemTemp;
    }
    final fileName = "cooler_config_backup_${DateTime.now().millisecondsSinceEpoch}.json";
    final file = File('${dir.path}/$fileName');
    await file.writeAsString(jsonStr);
    return file.path;
  }

  /// Buka file .json hasil export lewat file picker bawaan HP (bisa dari
  /// Downloads, Google Drive, dsb), kembalikan payload map-nya
  /// (null kalau dibatalkan/gagal dibaca).
  static Future<Map<String, dynamic>?> importFromFile() async {
    final result = await FilePicker.platform.pickFiles(
      type: FileType.custom,
      allowedExtensions: ["json"],
    );
    if (result == null || result.files.isEmpty) return null;
    final path = result.files.first.path;
    if (path == null) return null;
    try {
      final content = await File(path).readAsString();
      return jsonDecode(content) as Map<String, dynamic>;
    } catch (_) {
      return null;
    }
  }
}
