import 'package:flutter_local_notifications/flutter_local_notifications.dart';

/// ===== NOTIFIKASI LOKAL =====
/// Dipakai untuk 2 hal:
///  1) notifikasi saat Jadwal Otomatis terpicu (voltase berubah otomatis)
///  2) notifikasi saat cooler terdeteksi offline lebih dari X menit
/// Semua notifikasi ini LOKAL di HP (tidak lewat push server manapun).
class NotificationService {
  NotificationService._();
  static final FlutterLocalNotificationsPlugin _plugin = FlutterLocalNotificationsPlugin();
  static bool _inited = false;

  static Future<void> init() async {
    if (_inited) return;
    const androidInit = AndroidInitializationSettings('@mipmap/ic_launcher');
    const initSettings = InitializationSettings(android: androidInit);
    try {
      await _plugin.initialize(initSettings);
      // Android 13+ butuh izin notifikasi eksplisit dari user.
      await _plugin
          .resolvePlatformSpecificImplementation<AndroidFlutterLocalNotificationsPlugin>()
          ?.requestNotificationsPermission();
      _inited = true;
    } catch (_) {
      // Kalau gagal init (mis. platform belum didukung), diamkan saja —
      // fitur lain di app tetap jalan normal tanpa notifikasi.
    }
  }

  static Future<void> show({
    required int id,
    required String title,
    required String body,
  }) async {
    if (!_inited) await init();
    const androidDetails = AndroidNotificationDetails(
      'cooler_channel',
      'Notifikasi Cooler',
      channelDescription: 'Notifikasi jadwal otomatis & status offline cooler',
      importance: Importance.high,
      priority: Priority.high,
    );
    const details = NotificationDetails(android: androidDetails);
    try {
      await _plugin.show(id, title, body, details);
    } catch (_) {}
  }
}
