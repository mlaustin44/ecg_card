// App shell: theme and routes. Every screen has its own file in ui/screens.
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:material_ui/material_ui.dart';

import 'ui/screens/breathe_screen.dart';
import 'ui/screens/card_screen.dart';
import 'ui/screens/checkin_screen.dart';
import 'ui/screens/connect_screen.dart';
import 'ui/screens/developer_screen.dart';
import 'ui/screens/history_screen.dart';
import 'ui/screens/home_screen.dart';
import 'ui/screens/results_screen.dart';
import 'ui/screens/welcome_screen.dart';
import 'ui/theme.dart';

/// Route paths (use these names, not literals).
abstract final class Routes {
  static const home = '/';
  static const welcome = '/welcome';
  static const connect = '/connect';
  static const checkIn = '/checkin';
  static const breathe = '/breathe';
  static const history = '/history';
  static String session(String id) => '/session/$id'; // results / history detail
  static const card = '/card';
  static const developer = '/developer';
}

GoRouter buildRouter({String initialLocation = Routes.home}) => GoRouter(
  initialLocation: initialLocation,
  routes: [
    GoRoute(path: Routes.home, builder: (_, _) => const HomeScreen()),
    GoRoute(path: Routes.welcome, builder: (_, _) => const WelcomeScreen()),
    GoRoute(path: Routes.connect, builder: (_, _) => const ConnectScreen()),
    GoRoute(path: Routes.checkIn, builder: (_, _) => const CheckInScreen()),
    GoRoute(path: Routes.breathe, builder: (_, _) => const BreatheScreen()),
    GoRoute(path: Routes.history, builder: (_, _) => const HistoryScreen()),
    GoRoute(
      path: '/session/:id',
      builder: (_, s) => ResultsScreen(sessionId: s.pathParameters['id']!),
    ),
    GoRoute(path: Routes.card, builder: (_, _) => const CardScreen()),
    GoRoute(path: Routes.developer, builder: (_, _) => const DeveloperScreen()),
  ],
);

class EcgCardApp extends ConsumerStatefulWidget {
  const EcgCardApp({super.key, this.initialLocation = Routes.home});
  final String initialLocation;

  @override
  ConsumerState<EcgCardApp> createState() => _EcgCardAppState();
}

class _EcgCardAppState extends ConsumerState<EcgCardApp> {
  late final GoRouter _router = buildRouter(initialLocation: widget.initialLocation);

  @override
  Widget build(BuildContext context) => MaterialApp.router(
    title: 'ECG Card',
    debugShowCheckedModeBanner: false,
    theme: AppTheme.light(),
    darkTheme: AppTheme.dark(),
    routerConfig: _router,
  );
}
