// Live ECG trace: a sweeping (monitor style) or scrolling line, fed by
// LiveSessionApi.ecgUv (microvolts, 500 Hz, in ~20 ms chunks).
//
// Samples go into a ring buffer as they arrive; a ticker plays them out at
// the real sample rate a few tens of ms behind, so the line moves smoothly at
// the display rate rather than in 20 ms steps. The vertical scale follows the
// QRS height of what is on screen (fast to grow, slow to shrink). The ticker
// sleeps when no data arrives, so idle screens cost nothing.
import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/scheduler.dart';
import 'package:material_ui/material_ui.dart';

import '../theme.dart';

enum EcgTraceMode { sweep, scroll }

class EcgTraceView extends StatefulWidget {
  const EcgTraceView({
    super.key,
    required this.samples,
    this.sampleHz = 500,
    this.window = const Duration(seconds: 4),
    this.mode = EcgTraceMode.sweep,
    this.color = AppColors.heart,
    this.strokeWidth = 2.4,
    this.latency = const Duration(milliseconds: 60),
    this.minRangeUv = 400,
  });

  final Stream<Float32List> samples;
  final int sampleHz;

  /// Time across the full width.
  final Duration window;
  final EcgTraceMode mode;
  final Color color;
  final double strokeWidth;

  /// Play-out delay that absorbs notification jitter.
  final Duration latency;

  /// The scale never zooms in further than this peak-to-peak range, so a
  /// quiet (no contact) signal stays a calm flat line.
  final double minRangeUv;

  @override
  State<EcgTraceView> createState() => _EcgTraceViewState();
}

/// Ring buffer + play-out position + scale; the painter listens to it.
class EcgTraceModel extends ChangeNotifier {
  EcgTraceModel({required this.sampleHz, required this.windowSamples, required this.minRangeUv})
    : _buf = Float32List(windowSamples + sampleHz);

  final int sampleHz;
  final int windowSamples;
  final double minRangeUv;
  final Float32List _buf;

  int received = 0; // samples written in total
  double shown = 0; // samples played out (fractional)
  double center = 0; // µV at the vertical middle
  double range = 0; // µV peak-to-peak mapped to the drawing band (0 = not set)

  int get capacity => _buf.length;
  double at(int k) => _buf[k % _buf.length];

  void add(Float32List chunk) {
    for (var i = 0; i < chunk.length; i++) {
      _buf[(received + i) % _buf.length] = chunk[i];
    }
    received += chunk.length;
  }

  /// Advance play-out by [dt] seconds, aiming to stay [lagSamples] behind.
  void advance(double dt, int lagSamples) {
    final target = (received - lagSamples).toDouble();
    final behind = target - shown;
    if (behind > sampleHz * 0.5 || shown == 0 && received > 0) {
      shown = math.max(0, target); // far behind (first data, app resumed): jump
    } else {
      // Gently speed up or slow down to hold the lag.
      final rate = sampleHz * (1 + (behind / sampleHz).clamp(-0.5, 0.5));
      shown = math.min(received.toDouble(), shown + rate * dt);
    }
    _updateScale(dt);
    notifyListeners();
  }

  void _updateScale(double dt) {
    final end = shown.floor();
    final start = math.max(0, end - windowSamples + 1);
    if (end <= start) return;
    var lo = double.infinity, hi = -double.infinity;
    for (var k = start; k <= end; k += 2) {
      final v = at(k);
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    final want = math.max(minRangeUv, hi - lo);
    final wantCenter = (hi + lo) / 2;
    if (range == 0) {
      range = want;
      center = wantCenter;
      return;
    }
    // Grow within ~0.15 s, shrink over ~1.5 s; centre follows in ~0.6 s.
    final tau = want > range ? 0.15 : 1.5;
    range += (want - range) * (1 - math.exp(-dt / tau));
    center += (wantCenter - center) * (1 - math.exp(-dt / 0.6));
  }
}

class _EcgTraceViewState extends State<EcgTraceView> with SingleTickerProviderStateMixin {
  late EcgTraceModel _model = _newModel();
  late final Ticker _ticker = createTicker(_tick);
  StreamSubscription<Float32List>? _sub;
  Duration _lastTick = Duration.zero;
  Duration _idle = Duration.zero;

  EcgTraceModel _newModel() => EcgTraceModel(
    sampleHz: widget.sampleHz,
    windowSamples: (widget.window.inMicroseconds * widget.sampleHz / 1e6).round(),
    minRangeUv: widget.minRangeUv,
  );

  @override
  void initState() {
    super.initState();
    _listen();
  }

  void _listen() {
    _sub?.cancel();
    _sub = widget.samples.listen((chunk) {
      _model.add(chunk);
      _idle = Duration.zero;
      if (!_ticker.isActive) {
        _lastTick = Duration.zero;
        _ticker.start();
      }
    });
  }

  void _tick(Duration elapsed) {
    final dt = (elapsed - _lastTick).inMicroseconds / 1e6;
    _lastTick = elapsed;
    final lag = (widget.latency.inMicroseconds * widget.sampleHz / 1e6).round();
    _model.advance(dt.clamp(0, 0.1), lag);
    _idle += Duration(microseconds: (dt * 1e6).round());
    if (_idle > const Duration(milliseconds: 800) && _model.shown >= _model.received) {
      _ticker.stop(); // no data for a while: sleep until the next chunk
    }
  }

  @override
  void didUpdateWidget(EcgTraceView old) {
    super.didUpdateWidget(old);
    if (old.sampleHz != widget.sampleHz ||
        old.window != widget.window ||
        old.minRangeUv != widget.minRangeUv) {
      _model.dispose();
      _model = _newModel();
    }
    if (old.samples != widget.samples) _listen();
  }

  @override
  void dispose() {
    _sub?.cancel();
    _ticker.dispose();
    _model.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => RepaintBoundary(
    child: CustomPaint(
      painter: EcgTracePainter(
        model: _model,
        mode: widget.mode,
        color: widget.color,
        strokeWidth: widget.strokeWidth,
        baseline: context.tones.line,
      ),
      size: Size.infinite,
    ),
  );
}

class EcgTracePainter extends CustomPainter {
  EcgTracePainter({
    required this.model,
    required this.mode,
    required this.color,
    required this.strokeWidth,
    required this.baseline,
  }) : super(repaint: model);

  final EcgTraceModel model;
  final EcgTraceMode mode;
  final Color color;
  final double strokeWidth;
  final Color baseline;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height;
    final n = model.windowSamples;
    final end = model.shown.floor();
    final line = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = strokeWidth
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round
      ..isAntiAlias = true;

    if (end <= 0 || model.range == 0) {
      // Waiting for signal: a soft dotted baseline.
      final dots = Paint()..color = baseline;
      for (var x = 6.0; x < w; x += 12) {
        canvas.drawCircle(Offset(x, h / 2), 1.6, dots);
      }
      return;
    }

    final band = h * 0.80;
    double y(double uv) => h / 2 - (uv - model.center) / model.range * band;
    final pxPerSample = w / n;

    Path build(int from, int to, double Function(int k) xOf) {
      final path = Path();
      if (to < from) return path;
      // About two points per pixel: min and max of each column, in order.
      final step = math.max(1, (1 / pxPerSample / 2).floor());
      var first = true;
      for (var k = from; k <= to; k += step) {
        final last = math.min(to, k + step - 1);
        var lo = model.at(k), hi = lo;
        var loK = k, hiK = k;
        for (var j = k + 1; j <= last; j++) {
          final v = model.at(j);
          if (v < lo) {
            lo = v;
            loK = j;
          } else if (v > hi) {
            hi = v;
            hiK = j;
          }
        }
        final a = loK <= hiK ? lo : hi, b = loK <= hiK ? hi : lo;
        final x = xOf(k);
        if (first) {
          path.moveTo(x, y(a));
          first = false;
        } else {
          path.lineTo(x, y(a));
        }
        if (step > 1) path.lineTo(x + pxPerSample * (step - 1), y(b));
      }
      return path;
    }

    if (mode == EcgTraceMode.scroll) {
      final from = math.max(0, end - n + 1);
      final path = build(from, end, (k) => w - (end - k) * pxPerSample);
      line.shader = LinearGradient(
        colors: [color.withValues(alpha: 0.0), color.withValues(alpha: 0.9), color],
        stops: const [0, 0.25, 1],
      ).createShader(Offset.zero & size);
      canvas.drawPath(path, line);
      _drawHead(canvas, Offset(w, y(model.at(end))));
      return;
    }

    // Sweep: the current pass runs from the left edge to the head; the previous
    // pass shows to the right of a small gap and fades towards the gap.
    final head = end % n;
    final sweepStart = end - head;
    final gap = (n * 0.05).round();
    double xOf(int k) => (k % n) * pxPerSample;

    final fresh = build(sweepStart, end, xOf);
    line.color = color;
    canvas.drawPath(fresh, line);

    final oldFrom = math.max(0, end - n + gap);
    final oldTo = sweepStart - 1;
    if (oldTo > oldFrom) {
      final old = build(oldFrom, oldTo, xOf);
      final gx = (head + gap) * pxPerSample;
      line
        ..color = color
        ..shader = LinearGradient(
          colors: [color.withValues(alpha: 0.05), color.withValues(alpha: 0.55)],
        ).createShader(Rect.fromLTRB(gx, 0, math.max(gx + 1, w), h));
      canvas.drawPath(old, line);
    }
    _drawHead(canvas, Offset(head * pxPerSample, y(model.at(end))));
  }

  void _drawHead(Canvas canvas, Offset p) {
    canvas.drawCircle(
      p,
      strokeWidth * 3.2,
      Paint()
        ..color = color.withValues(alpha: 0.25)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 4),
    );
    canvas.drawCircle(p, strokeWidth * 1.4, Paint()..color = color);
  }

  @override
  bool shouldRepaint(EcgTracePainter old) =>
      old.model != model ||
      old.mode != mode ||
      old.color != color ||
      old.strokeWidth != strokeWidth ||
      old.baseline != baseline;
}
