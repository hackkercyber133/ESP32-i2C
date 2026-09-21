import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// ===== KONTROL TEMA TERANG/GELAP =====
/// Dipisah dari main.dart supaya bisa dipakai global (MyApp) dan dari
/// drawer. Pilihan user disimpan persist ke SharedPreferences supaya
/// tidak balik ke default tiap buka app lagi.
class ThemeController {
  ThemeController._();

  static const _prefKey = "app_theme_mode";
  static final ValueNotifier<ThemeMode> mode = ValueNotifier(ThemeMode.dark);

  static Future<void> load() async {
    final prefs = await SharedPreferences.getInstance();
    final saved = prefs.getString(_prefKey);
    mode.value = saved == "light" ? ThemeMode.light : ThemeMode.dark;
  }

  static Future<void> toggle() async {
    await setDark(mode.value != ThemeMode.dark);
  }

  static Future<void> setDark(bool dark) async {
    mode.value = dark ? ThemeMode.dark : ThemeMode.light;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_prefKey, dark ? "dark" : "light");
  }

  static bool get isDark => mode.value == ThemeMode.dark;
}

/// Palet warna dasar yang dipakai berulang di seluruh app (background,
/// permukaan card, teks) supaya konsisten antara mode gelap & terang.
///
/// CATATAN: SplashScreen sengaja TETAP gelap penuh (nuansa neon/gaming),
/// tidak dipengaruhi toggle ini — durasinya singkat & memang didesain gelap.
/// Beberapa dialog kecil (about, wifi setup) juga masih pakai warna gelap
/// tetap supaya scope perubahan tidak melebar ke seluruh 1800+ baris kode;
/// layar utama, drawer, Riwayat Pemakaian, dan Jadwal Otomatis sudah full
/// mendukung mode terang.
class AppColors {
  AppColors._();
  static Color bg(bool isDark) => isDark ? const Color(0xFF090d14) : const Color(0xFFF4F6F9);
  static Color surface(bool isDark) => isDark ? const Color(0xFF0d1219) : Colors.white;
  static Color card(bool isDark) =>
      isDark ? Colors.white.withOpacity(0.05) : Colors.black.withOpacity(0.045);
  static Color text(bool isDark) => isDark ? Colors.white : const Color(0xFF1a1f27);
  static Color textFaint(bool isDark) => isDark ? Colors.white38 : Colors.black45;
  static Color divider(bool isDark) => isDark ? Colors.white12 : Colors.black12;
}
