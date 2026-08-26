import 'package:flutter/material.dart';
import 'schedule_service.dart';
import 'theme_controller.dart';

/// ===== HALAMAN JADWAL OTOMATIS =====
/// List aturan jadwal untuk cooler yang sedang aktif, bisa tambah/edit/
/// hapus/aktif-nonaktifkan. Eksekusi sebenarnya (kirim perintah voltase)
/// dilakukan di ControllerPage lewat timer berkala yang membaca data
/// yang disimpan lewat halaman ini.
class SchedulePage extends StatefulWidget {
  final String coolerId;
  final Color accentColor;
  final List<double> availableVoltages;

  const SchedulePage({
    required this.coolerId,
    required this.accentColor,
    required this.availableVoltages,
    Key? key,
  }) : super(key: key);

  @override
  State<SchedulePage> createState() => _SchedulePageState();
}

class _SchedulePageState extends State<SchedulePage> {
  List<ScheduleRule> rules = [];
  bool loading = true;
  static const _dayLabels = ["Sen", "Sel", "Rab", "Kam", "Jum", "Sab", "Min"];

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final all = await ScheduleService.loadAll();
    if (!mounted) return;
    setState(() {
      rules = all.where((r) => r.coolerId == widget.coolerId).toList();
      loading = false;
    });
  }

  Future<void> _persist() async {
    // Simpan balik: aturan cooler lain (kalau ada) tetap dipertahankan.
    final all = await ScheduleService.loadAll();
    all.removeWhere((r) => r.coolerId == widget.coolerId);
    all.addAll(rules);
    await ScheduleService.saveAll(all);
  }

  void _openEditor({ScheduleRule? existing}) async {
    final isDark = ThemeController.isDark;
    final result = await showModalBottomSheet<ScheduleRule>(
      context: context,
      backgroundColor: AppColors.surface(isDark),
      isScrollControlled: true,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(20))),
      builder: (ctx) => _ScheduleEditorSheet(
        coolerId: widget.coolerId,
        accentColor: widget.accentColor,
        availableVoltages: widget.availableVoltages,
        existing: existing,
      ),
    );
    if (result != null) {
      setState(() {
        rules.removeWhere((r) => r.id == result.id);
        rules.add(result);
      });
      await _persist();
    }
  }

  void _delete(ScheduleRule r) async {
    setState(() => rules.removeWhere((x) => x.id == r.id));
    await _persist();
  }

  @override
  Widget build(BuildContext context) {
    final isDark = ThemeController.isDark;
    return Scaffold(
      backgroundColor: AppColors.bg(isDark),
      appBar: AppBar(
        backgroundColor: AppColors.surface(isDark),
        elevation: 0,
        iconTheme: IconThemeData(color: AppColors.text(isDark)),
        title: Text("Jadwal Otomatis", style: TextStyle(color: AppColors.text(isDark))),
      ),
      floatingActionButton: FloatingActionButton(
        backgroundColor: widget.accentColor,
        onPressed: () => _openEditor(),
        child: const Icon(Icons.add, color: Colors.black),
      ),
      body: loading
          ? Center(child: CircularProgressIndicator(color: widget.accentColor))
          : rules.isEmpty
              ? Center(
                  child: Padding(
                    padding: const EdgeInsets.all(24),
                    child: Text(
                      "Belum ada jadwal. Tambah jadwal supaya voltase ganti otomatis di jam tertentu (mis. jam 22:00 turun ke 5V).\n\nCatatan: jadwal berjalan selama app ini masih terbuka, karena perintah dikirim lewat koneksi WiFi/Bluetooth yang aktif di app.",
                      textAlign: TextAlign.center,
                      style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 13),
                    ),
                  ),
                )
              : ListView.builder(
                  padding: const EdgeInsets.all(12),
                  itemCount: rules.length,
                  itemBuilder: (ctx, i) {
                    final r = rules[i];
                    final time =
                        "${r.hour.toString().padLeft(2, '0')}:${r.minute.toString().padLeft(2, '0')}";
                    final daysText =
                        r.days.length == 7 ? "Setiap hari" : r.days.map((d) => _dayLabels[d - 1]).join(", ");
                    return Container(
                      margin: const EdgeInsets.only(bottom: 10),
                      padding: const EdgeInsets.all(14),
                      decoration:
                          BoxDecoration(color: AppColors.card(isDark), borderRadius: BorderRadius.circular(14)),
                      child: Row(
                        children: [
                          Icon(Icons.schedule, color: widget.accentColor),
                          const SizedBox(width: 12),
                          Expanded(
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text("$time  →  ${r.voltage.toStringAsFixed(0)}V",
                                    style: TextStyle(color: AppColors.text(isDark), fontWeight: FontWeight.bold)),
                                Text(daysText, style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 11)),
                              ],
                            ),
                          ),
                          Switch(
                            value: r.enabled,
                            activeColor: widget.accentColor,
                            onChanged: (v) async {
                              setState(() => r.enabled = v);
                              await _persist();
                            },
                          ),
                          IconButton(
                            icon: Icon(Icons.edit, color: AppColors.textFaint(isDark), size: 20),
                            onPressed: () => _openEditor(existing: r),
                          ),
                          IconButton(
                            icon: Icon(Icons.delete_outline, color: AppColors.textFaint(isDark), size: 20),
                            onPressed: () => _delete(r),
                          ),
                        ],
                      ),
                    );
                  },
                ),
    );
  }
}

class _ScheduleEditorSheet extends StatefulWidget {
  final String coolerId;
  final Color accentColor;
  final List<double> availableVoltages;
  final ScheduleRule? existing;

  const _ScheduleEditorSheet({
    required this.coolerId,
    required this.accentColor,
    required this.availableVoltages,
    this.existing,
  });

  @override
  State<_ScheduleEditorSheet> createState() => _ScheduleEditorSheetState();
}

class _ScheduleEditorSheetState extends State<_ScheduleEditorSheet> {
  late TimeOfDay time;
  late double voltage;
  late Set<int> days;

  static const _dayLabels = ["Sen", "Sel", "Rab", "Kam", "Jum", "Sab", "Min"];

  @override
  void initState() {
    super.initState();
    final e = widget.existing;
    time = TimeOfDay(hour: e?.hour ?? 22, minute: e?.minute ?? 0);
    voltage = e?.voltage ?? widget.availableVoltages.first;
    days = (e?.days ?? [1, 2, 3, 4, 5, 6, 7]).toSet();
  }

  @override
  Widget build(BuildContext context) {
    final isDark = ThemeController.isDark;
    return Padding(
      padding: EdgeInsets.fromLTRB(20, 20, 20, MediaQuery.of(context).viewInsets.bottom + 20),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(widget.existing == null ? "Tambah Jadwal" : "Edit Jadwal",
              style: TextStyle(color: AppColors.text(isDark), fontSize: 16, fontWeight: FontWeight.bold)),
          const SizedBox(height: 16),
          ListTile(
            contentPadding: EdgeInsets.zero,
            leading: Icon(Icons.access_time, color: widget.accentColor),
            title: Text("Jam", style: TextStyle(color: AppColors.text(isDark))),
            trailing: Text(time.format(context),
                style: TextStyle(color: widget.accentColor, fontSize: 16, fontWeight: FontWeight.bold)),
            onTap: () async {
              final picked = await showTimePicker(context: context, initialTime: time);
              if (picked != null) setState(() => time = picked);
            },
          ),
          const SizedBox(height: 8),
          Text("Ubah ke voltase", style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 12)),
          const SizedBox(height: 8),
          Wrap(
            spacing: 8,
            children: widget.availableVoltages.map((v) {
              final selected = v == voltage;
              return ChoiceChip(
                label: Text("${v.toStringAsFixed(0)}V"),
                selected: selected,
                selectedColor: widget.accentColor,
                labelStyle: TextStyle(color: selected ? Colors.black : AppColors.text(isDark)),
                backgroundColor: AppColors.card(isDark),
                onSelected: (_) => setState(() => voltage = v),
              );
            }).toList(),
          ),
          const SizedBox(height: 16),
          Text("Hari", style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 12)),
          const SizedBox(height: 8),
          Wrap(
            spacing: 8,
            children: List.generate(7, (i) {
              final d = i + 1;
              final selected = days.contains(d);
              return FilterChip(
                label: Text(_dayLabels[i]),
                selected: selected,
                selectedColor: widget.accentColor,
                labelStyle: TextStyle(color: selected ? Colors.black : AppColors.text(isDark)),
                backgroundColor: AppColors.card(isDark),
                onSelected: (v) => setState(() => v ? days.add(d) : days.remove(d)),
              );
            }),
          ),
          const SizedBox(height: 20),
          SizedBox(
            width: double.infinity,
            child: ElevatedButton(
              style: ElevatedButton.styleFrom(
                  backgroundColor: widget.accentColor, padding: const EdgeInsets.symmetric(vertical: 14)),
              onPressed: days.isEmpty
                  ? null
                  : () {
                      final rule = ScheduleRule(
                        id: widget.existing?.id ?? DateTime.now().millisecondsSinceEpoch.toString(),
                        coolerId: widget.coolerId,
                        hour: time.hour,
                        minute: time.minute,
                        voltage: voltage,
                        days: days.toList()..sort(),
                        enabled: widget.existing?.enabled ?? true,
                        lastFiredDateKey: widget.existing?.lastFiredDateKey ?? "",
                      );
                      Navigator.pop(context, rule);
                    },
              child: const Text("Simpan", style: TextStyle(color: Colors.black, fontWeight: FontWeight.bold)),
            ),
          ),
        ],
      ),
    );
  }
}
