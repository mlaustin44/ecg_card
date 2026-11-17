// Live session state (contract) shared by the check-in and breathing screens.
// The controller (state/session_controller.dart) is implemented by the data /
// analysis layer: it starts the card, feeds events to the analysis, keeps a
// trace buffer, and on finish builds and saves a SessionRecord.
import 'dart:typed_data';

import '../analysis/models.dart';
import '../data/session_record.dart';
import '../protocol/link_status.dart';

enum LivePhase {
  idle,
  starting, // start command sent
  waitContact, // "place your thumbs"
  settling, // contact made, signal settling (progress 0..1)
  running,
  finished,
  error,
}

class LiveSession {
  const LiveSession({
    this.phase = LivePhase.idle,
    this.kind = SessionKind.checkIn,
    this.settleProgress = 0,
    this.elapsed = Duration.zero,
    this.target,
    this.hrBpm,
    this.fastBpm,
    this.rr = const [],
    this.coherence,
    this.status,
    this.error,
    this.record,
    this.saved = false,
    this.linkLost = false,
  });

  final LivePhase phase;
  final SessionKind kind;
  final double settleProgress; // 0..1 while settling
  /// Measured (running) time; pauses while contact is lost.
  final Duration elapsed;
  final Duration? target; // planned length; null = open-ended
  final int? hrBpm; // card's median HR
  final double? fastBpm; // 2-beat mean, follows breathing
  final List<RrInterval> rr;
  final CoherenceResult? coherence; // breathing sessions, updated every few s
  final CardStatus? status;
  final String? error; // friendly text for LivePhase.error

  /// The finished session (LivePhase.finished), also after an automatic
  /// finish at the target length.
  final SessionRecord? record;

  /// Whether [record] was saved to the store (enough was measured).
  final bool saved;

  /// The Bluetooth link dropped mid-session and is being re-established: the
  /// timer is paused (no ECG arrives) and the session carries on when the
  /// card is back (the card keeps measuring meanwhile).
  final bool linkLost;

  LiveSession copyWith({
    LivePhase? phase,
    SessionKind? kind,
    double? settleProgress,
    Duration? elapsed,
    Duration? target,
    int? hrBpm,
    double? fastBpm,
    List<RrInterval>? rr,
    CoherenceResult? coherence,
    CardStatus? status,
    String? error,
    SessionRecord? record,
    bool? saved,
    bool? linkLost,
  }) => LiveSession(
    phase: phase ?? this.phase,
    kind: kind ?? this.kind,
    settleProgress: settleProgress ?? this.settleProgress,
    elapsed: elapsed ?? this.elapsed,
    target: target ?? this.target,
    hrBpm: hrBpm ?? this.hrBpm,
    fastBpm: fastBpm ?? this.fastBpm,
    rr: rr ?? this.rr,
    coherence: coherence ?? this.coherence,
    status: status ?? this.status,
    error: error ?? this.error,
    record: record ?? this.record,
    saved: saved ?? this.saved,
    linkLost: linkLost ?? this.linkLost,
  );
}

/// What the session controller offers the UI (implemented in session_controller.dart).
abstract class LiveSessionApi {
  LiveSession get state;
  Stream<LiveSession> get changes;

  /// ECG for the live trace: microvolts, display-filtered, 500 Hz, in chunks.
  Stream<Float32List> get ecgUv;

  Future<void> begin(SessionKind kind, {Duration? target, double? breathsPerMin});

  /// Stops the card; saves when [save] and enough was measured. Returns the record.
  Future<SessionRecord?> finish({bool save = true});
  Future<void> cancel();
}
