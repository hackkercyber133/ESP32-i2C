import 'package:flutter/material.dart';
import 'package:fl_chart/fl_chart.dart';
import 'history_service.dart';
import 'theme_controller.dart';

/// ===== HALAMAN RIWAYAT PEMAKAIAN =====
/// Menampilkan grafik durasi nyala per hari (7 hari terakhir) dan
/// statistik ringkas (total jam nyala, voltase paling sering dipakai).
/// Semua data berasal dari HistoryService, yang mencatat status ASLI
/// yang diterima app dari device (bukan simulasi), disimpan lokal di HP.
class HistoryPage extends StatefulWidget {
  final String coolerId;
  final String coolerName;
  final Color accentColor;

  const HistoryPage({
    required this.coolerId,
    required this.coolerName,
    required this.accentColor,
    Key? key,
  }) : super(key: key);

  @override
  State<HistoryPage> createState() => _HistoryPageState();
}

class _HistoryPageState extends State<HistoryPage> {
  Map<DateTime, double> daily = {};
  Map<String, dynamic> stats = {};
  Map<DateTime, double> dailyVoltage = {};
  bool loading = true;

  static const _dayLabels = ["Sen", "Sel", "Rab", "Kam", "Jum", "Sab", "Min"];

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final d = await HistoryService.dailyHoursLast7Days(widget.coolerId);
    final s = await HistoryService.summary(widget.coolerId);
    final v = await HistoryService.dailyAverageVoltageLast7Days(widget.coolerId);
    if (!mounted) return;
    setState(() {
      daily = d;
      stats = s;
      dailyVoltage = v;
      loading = false;
    });
  }

  @override
  Widget build(BuildContext context) {
    final isDark = ThemeController.isDark;
    final days = daily.keys.toList()..sort();
    final maxVal = daily.values.isEmpty
        ? 1.0
        : daily.values.reduce((a, b) => a > b ? a : b);
    final totalHours = (stats["totalHours"] ?? 0.0) as double;
    final mostUsed = stats["mostUsedVoltage"] as double?;

    return Scaffold(
      backgroundColor: AppColors.bg(isDark),
      appBar: AppBar(
        backgroundColor: AppColors.surface(isDark),
        elevation: 0,
        iconTheme: IconThemeData(color: AppColors.text(isDark)),
        title: Text("Riwayat Pemakaian", style: TextStyle(color: AppColors.text(isDark))),
      ),
      body: loading
          ? Center(child: CircularProgressIndicator(color: widget.accentColor))
          : RefreshIndicator(
              onRefresh: _load,
              color: widget.accentColor,
              child: ListView(
                padding: const EdgeInsets.all(16),
                children: [
                  Text(widget.coolerName,
                      style: TextStyle(
                          color: AppColors.text(isDark), fontSize: 16, fontWeight: FontWeight.bold)),
                  const SizedBox(height: 4),
                  Text("Durasi nyala per hari (7 hari terakhir)",
                      style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 12)),
                  const SizedBox(height: 16),
                  Container(
                    height: 230,
                    padding: const EdgeInsets.fromLTRB(8, 16, 16, 8),
                    decoration:
                        BoxDecoration(color: AppColors.card(isDark), borderRadius: BorderRadius.circular(16)),
                    child: days.isEmpty
                        ? Center(
                            child: Text("Belum ada data",
                                style: TextStyle(color: AppColors.textFaint(isDark))))
                        : BarChart(
                            BarChartData(
                              maxY: maxVal <= 0 ? 1 : maxVal * 1.25,
                              barTouchData: BarTouchData(
                                touchTooltipData: BarTouchTooltipData(
                                  getTooltipItem: (group, groupIdx, rod, rodIdx) => BarTooltipItem(
                                    "${rod.toY.toStringAsFixed(1)} jam",
                                    const TextStyle(color: Colors.white, fontSize: 11),
                                  ),
                                ),
                              ),
                              titlesData: FlTitlesData(
                                leftTitles: AxisTitles(
                                  sideTitles: SideTitles(
                                    showTitles: true,
                                    reservedSize: 32,
                                    getTitlesWidget: (v, meta) => Text("${v.toInt()}j",
                                        style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 10)),
                                  ),
                                ),
                                bottomTitles: AxisTitles(
                                  sideTitles: SideTitles(
                                    showTitles: true,
                                    getTitlesWidget: (v, meta) {
                                      final i = v.toInt();
                                      if (i < 0 || i >= days.length) return const SizedBox();
                                      final wd = days[i].weekday; // 1=Senin..7=Minggu
                                      return Padding(
                                        padding: const EdgeInsets.only(top: 4),
                                        child: Text(_dayLabels[wd - 1],
                                            style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 10)),
                                      );
                                    },
                                  ),
                                ),
                                rightTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                                topTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                              ),
                              gridData: const FlGridData(show: true, drawVerticalLine: false),
                              borderData: FlBorderData(show: false),
                              barGroups: List.generate(days.length, (i) {
                                final hours = daily[days[i]] ?? 0;
                                return BarChartGroupData(x: i, barRods: [
                                  BarChartRodData(
                                    toY: hours,
                                    color: widget.accentColor,
                                    width: 18,
                                    borderRadius: BorderRadius.circular(4),
                                  ),
                                ]);
                              }),
                            ),
                          ),
                  ),
                  const SizedBox(height: 20),
                  Text("Grafik Voltase Harian",
                      style: TextStyle(color: AppColors.text(isDark), fontSize: 14, fontWeight: FontWeight.bold)),
                  const SizedBox(height: 10),
                  Container(
                    height: 230,
                    padding: const EdgeInsets.fromLTRB(8, 16, 16, 8),
                    decoration: BoxDecoration(color: AppColors.card(isDark), borderRadius: BorderRadius.circular(16)),
                    child: LineChart(LineChartData(
                      minY: 0, maxY: 16,
                      gridData: const FlGridData(show: true, drawVerticalLine: false),
                      borderData: FlBorderData(show: false),
                      titlesData: FlTitlesData(
                        topTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                        rightTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
                        bottomTitles: AxisTitles(sideTitles: SideTitles(showTitles: true, getTitlesWidget: (value, meta) {
                          final i = value.toInt();
                          if (i < 0 || i >= days.length) return const SizedBox();
                          return Text(_dayLabels[days[i].weekday - 1], style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 9));
                        })),
                        leftTitles: AxisTitles(sideTitles: SideTitles(showTitles: true, reservedSize: 30, getTitlesWidget: (v, _) => Text('${v.toInt()}V', style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 9)))),
                      ),
                      lineBarsData: [LineChartBarData(
                        spots: List.generate(days.length, (i) => FlSpot(i.toDouble(), dailyVoltage[days[i]] ?? 0)),
                        isCurved: true, color: widget.accentColor, barWidth: 3, dotData: const FlDotData(show: true),
                        belowBarData: BarAreaData(show: true, color: widget.accentColor.withOpacity(.08)),
                      )],
                    )),
                  ),
                  const SizedBox(height: 20),
                  Text("Statistik Ringkas",
                      style: TextStyle(
                          color: AppColors.text(isDark), fontSize: 14, fontWeight: FontWeight.bold)),
                  const SizedBox(height: 10),
                  Row(
                    children: [
                      Expanded(
                        child: _statCard(isDark, "Total Jam Nyala",
                            "${totalHours.toStringAsFixed(1)} jam", Icons.timelapse),
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: _statCard(isDark, "Voltase Tersering",
                            mostUsed != null ? "${mostUsed.toStringAsFixed(0)}V" : "-", Icons.bolt),
                      ),
                    ],
                  ),
                  const SizedBox(height: 8),
                  Text(
                    "Grafik & statistik dihitung dari status asli yang diterima dari device, disimpan lokal di HP ini (45 hari terakhir).",
                    style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 11),
                  ),
                ],
              ),
            ),
    );
  }

  Widget _statCard(bool isDark, String label, String value, IconData icon) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(color: AppColors.card(isDark), borderRadius: BorderRadius.circular(14)),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(icon, color: widget.accentColor, size: 20),
          const SizedBox(height: 8),
          Text(value, style: TextStyle(color: AppColors.text(isDark), fontSize: 18, fontWeight: FontWeight.bold)),
          Text(label, style: TextStyle(color: AppColors.textFaint(isDark), fontSize: 11)),
        ],
      ),
    );
  }
}
