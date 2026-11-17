// Design tokens and ThemeData (owned by the core UI agent; token names are a
// contract: other screens use AppColors / AppSpace / AppRadius / AppText).
//
// Look: warm and soft. Coral for the heart, teal for breath, indigo as the
// primary, a sunny amber for highlights; an off-white "paper" background by day
// and a deep night blue by night; generous rounded cards; Nunito (bundled, OFL)
// for a friendly rounded voice, with tabular numerals for live numbers.
import 'package:material_ui/material_ui.dart';

abstract final class AppColors {
  static const heart = Color(0xFFFF6F61); // warm coral: heart rate, beats
  static const breath = Color(0xFF3FB4A6); // teal: breathing, calm
  static const indigo = Color(0xFF4B4FA6); // primary
  static const sun = Color(0xFFFFB547); // highlights, streaks
  static const lightBg = Color(0xFFF7F4EF);
  static const darkBg = Color(0xFF12131A);

  // Extra tones.
  static const lavender = Color(0xFF8A7FE0); // gradient partner of indigo
  static const peach = Color(0xFFFFA48A); // gradient partner of heart
  static const mint = Color(0xFF7FD9C9); // gradient partner of breath
  static const indigoNight = Color(0xFF8E92F2); // primary on dark backgrounds

  static const lightCard = Color(0xFFFFFFFF);
  static const darkCard = Color(0xFF1D1F2B);
  static const lightInk = Color(0xFF2B2A3A);
  static const darkInk = Color(0xFFF1EFF8);
  static const lightInkSoft = Color(0xFF6F6D80);
  static const darkInkSoft = Color(0xFFA3A2B8);
  static const lightLine = Color(0xFFEAE4DA);
  static const darkLine = Color(0xFF2D2F3F);
}

abstract final class AppSpace {
  static const xs = 4.0, s = 8.0, m = 16.0, l = 24.0, xl = 32.0;
  static const xxl = 48.0;

  /// Side gutter for screen content.
  static const page = 20.0;
}

abstract final class AppRadius {
  static const card = 24.0, pill = 999.0;
  static const small = 14.0, large = 32.0;
}

abstract final class AppMotion {
  static const fast = Duration(milliseconds: 180);
  static const medium = Duration(milliseconds: 320);
  static const slow = Duration(milliseconds: 600);
  static const curve = Curves.easeOutCubic;
}

abstract final class AppGradients {
  static const checkIn = LinearGradient(
    begin: Alignment.topLeft,
    end: Alignment.bottomRight,
    colors: [AppColors.indigo, AppColors.lavender],
  );
  static const heart = LinearGradient(
    begin: Alignment.topLeft,
    end: Alignment.bottomRight,
    colors: [AppColors.heart, AppColors.peach],
  );
  static const breath = LinearGradient(
    begin: Alignment.topLeft,
    end: Alignment.bottomRight,
    colors: [AppColors.breath, AppColors.mint],
  );
  static const sunrise = LinearGradient(
    begin: Alignment.topLeft,
    end: Alignment.bottomRight,
    colors: [AppColors.sun, AppColors.peach],
  );
}

/// Colours that differ between light and dark (read with `context.tones`).
@immutable
class AppTones extends ThemeExtension<AppTones> {
  const AppTones({
    required this.card,
    required this.ink,
    required this.inkSoft,
    required this.line,
    required this.primary,
    required this.shadow,
  });

  final Color card; // raised surfaces
  final Color ink; // main text
  final Color inkSoft; // secondary text
  final Color line; // hairlines, tracks, grids
  final Color primary; // indigo tuned for the background
  final Color shadow;

  /// [c] as a soft wash on a card (chips, icon wells, tracks).
  Color soft(Color c, [double amount = 0.14]) =>
      Color.alphaBlend(c.withValues(alpha: amount), card);

  static const light = AppTones(
    card: AppColors.lightCard,
    ink: AppColors.lightInk,
    inkSoft: AppColors.lightInkSoft,
    line: AppColors.lightLine,
    primary: AppColors.indigo,
    shadow: Color(0x14453A2E),
  );
  static const dark = AppTones(
    card: AppColors.darkCard,
    ink: AppColors.darkInk,
    inkSoft: AppColors.darkInkSoft,
    line: AppColors.darkLine,
    primary: AppColors.indigoNight,
    shadow: Color(0x33000000),
  );

  @override
  AppTones copyWith({
    Color? card,
    Color? ink,
    Color? inkSoft,
    Color? line,
    Color? primary,
    Color? shadow,
  }) => AppTones(
    card: card ?? this.card,
    ink: ink ?? this.ink,
    inkSoft: inkSoft ?? this.inkSoft,
    line: line ?? this.line,
    primary: primary ?? this.primary,
    shadow: shadow ?? this.shadow,
  );

  @override
  AppTones lerp(AppTones? other, double t) {
    if (other == null) return this;
    return AppTones(
      card: Color.lerp(card, other.card, t)!,
      ink: Color.lerp(ink, other.ink, t)!,
      inkSoft: Color.lerp(inkSoft, other.inkSoft, t)!,
      line: Color.lerp(line, other.line, t)!,
      primary: Color.lerp(primary, other.primary, t)!,
      shadow: Color.lerp(shadow, other.shadow, t)!,
    );
  }
}

extension AppThemeContext on BuildContext {
  AppTones get tones => Theme.of(this).extension<AppTones>() ?? AppTones.light;
  TextTheme get text => Theme.of(this).textTheme;
  bool get isDark => Theme.of(this).brightness == Brightness.dark;
}

/// Type helpers. The TextTheme carries the scale; these add the special cases.
abstract final class AppText {
  static const family = 'Nunito';

  /// Big live numbers (heart rate, timers): heavy, tabular, tight line height.
  static TextStyle numeral(double size, {Color? color, FontWeight weight = FontWeight.w800}) =>
      TextStyle(
        fontFamily: family,
        fontSize: size,
        fontWeight: weight,
        height: 1.0,
        letterSpacing: -size * 0.03,
        color: color,
        fontFeatures: const [FontFeature.tabularFigures()],
      );

  /// Small upper-case label above a section ("TODAY", "FOR THE CURIOUS").
  static TextStyle eyebrow(BuildContext context, {Color? color}) => TextStyle(
    fontFamily: family,
    fontSize: 12,
    fontWeight: FontWeight.w800,
    letterSpacing: 1.2,
    height: 1.2,
    color: color ?? context.tones.inkSoft,
  );

  static TextTheme textTheme(Color ink, Color inkSoft) {
    TextStyle s(
      double size,
      FontWeight w, {
      double height = 1.25,
      double spacing = 0,
      Color? color,
    }) => TextStyle(
      fontFamily: family,
      fontSize: size,
      fontWeight: w,
      height: height,
      letterSpacing: spacing,
      color: color ?? ink,
    );
    return TextTheme(
      displayLarge: s(64, FontWeight.w800, height: 1.0, spacing: -1.5),
      displayMedium: s(48, FontWeight.w800, height: 1.05, spacing: -1),
      displaySmall: s(38, FontWeight.w800, height: 1.1, spacing: -0.5),
      headlineLarge: s(32, FontWeight.w800, height: 1.15, spacing: -0.4),
      headlineMedium: s(27, FontWeight.w800, height: 1.2, spacing: -0.3),
      headlineSmall: s(23, FontWeight.w800, height: 1.2, spacing: -0.2),
      titleLarge: s(20, FontWeight.w800, height: 1.25),
      titleMedium: s(17, FontWeight.w700, height: 1.3),
      titleSmall: s(15, FontWeight.w700, height: 1.3),
      bodyLarge: s(17, FontWeight.w400, height: 1.45),
      bodyMedium: s(15, FontWeight.w400, height: 1.45, color: inkSoft),
      bodySmall: s(13, FontWeight.w400, height: 1.4, color: inkSoft),
      labelLarge: s(16, FontWeight.w800, height: 1.2, spacing: 0.1),
      labelMedium: s(14, FontWeight.w700, height: 1.2),
      labelSmall: s(12, FontWeight.w700, height: 1.2, spacing: 0.4, color: inkSoft),
    );
  }
}

abstract final class AppTheme {
  static ThemeData light() => _base(Brightness.light);
  static ThemeData dark() => _base(Brightness.dark);

  static ThemeData _base(Brightness b) {
    final dark = b == Brightness.dark;
    final tones = dark ? AppTones.dark : AppTones.light;
    final bg = dark ? AppColors.darkBg : AppColors.lightBg;
    final scheme = ColorScheme.fromSeed(seedColor: AppColors.indigo, brightness: b).copyWith(
      primary: tones.primary,
      onPrimary: dark ? AppColors.darkBg : Colors.white,
      secondary: AppColors.breath,
      onSecondary: Colors.white,
      tertiary: AppColors.heart,
      onTertiary: Colors.white,
      surface: bg,
      onSurface: tones.ink,
      onSurfaceVariant: tones.inkSoft,
      surfaceContainerLowest: tones.card,
      surfaceContainerLow: tones.card,
      surfaceContainer: tones.card,
      surfaceContainerHigh: tones.card,
      surfaceContainerHighest: tones.soft(tones.inkSoft, 0.10),
      outline: tones.inkSoft.withValues(alpha: 0.5),
      outlineVariant: tones.line,
      error: const Color(0xFFE0565B),
    );
    final text = AppText.textTheme(tones.ink, tones.inkSoft);
    final pillShape = const StadiumBorder();
    return ThemeData(
      useMaterial3: true,
      brightness: b,
      colorScheme: scheme,
      fontFamily: AppText.family,
      textTheme: text,
      scaffoldBackgroundColor: bg,
      canvasColor: bg,
      extensions: [tones],
      splashFactory: InkSparkle.splashFactory,
      appBarTheme: AppBarTheme(
        backgroundColor: bg,
        surfaceTintColor: Colors.transparent,
        foregroundColor: tones.ink,
        elevation: 0,
        scrolledUnderElevation: 0,
        centerTitle: true,
        titleTextStyle: text.titleMedium,
      ),
      cardTheme: CardThemeData(
        color: tones.card,
        elevation: 0,
        margin: EdgeInsets.zero,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(AppRadius.card)),
      ),
      filledButtonTheme: FilledButtonThemeData(
        style: FilledButton.styleFrom(
          shape: pillShape,
          minimumSize: const Size(64, 56),
          padding: const EdgeInsets.symmetric(horizontal: AppSpace.l),
          textStyle: text.labelLarge,
        ),
      ),
      outlinedButtonTheme: OutlinedButtonThemeData(
        style: OutlinedButton.styleFrom(
          shape: pillShape,
          minimumSize: const Size(64, 52),
          side: BorderSide(color: tones.line, width: 1.5),
          foregroundColor: tones.ink,
          textStyle: text.labelLarge,
        ),
      ),
      textButtonTheme: TextButtonThemeData(
        style: TextButton.styleFrom(
          shape: pillShape,
          foregroundColor: tones.primary,
          textStyle: text.labelLarge,
        ),
      ),
      inputDecorationTheme: InputDecorationTheme(
        filled: true,
        fillColor: tones.soft(tones.inkSoft, 0.06),
        hintStyle: text.bodyMedium?.copyWith(color: tones.inkSoft.withValues(alpha: 0.8)),
        contentPadding: const EdgeInsets.all(AppSpace.m),
        border: OutlineInputBorder(
          borderRadius: BorderRadius.circular(AppRadius.small),
          borderSide: BorderSide.none,
        ),
        focusedBorder: OutlineInputBorder(
          borderRadius: BorderRadius.circular(AppRadius.small),
          borderSide: BorderSide(color: tones.primary.withValues(alpha: 0.5), width: 1.5),
        ),
      ),
      dialogTheme: DialogThemeData(
        backgroundColor: tones.card,
        surfaceTintColor: Colors.transparent,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(AppRadius.card)),
        titleTextStyle: text.titleLarge,
        contentTextStyle: text.bodyMedium,
      ),
      bottomSheetTheme: BottomSheetThemeData(
        backgroundColor: tones.card,
        surfaceTintColor: Colors.transparent,
        showDragHandle: true,
        shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(AppRadius.large)),
        ),
      ),
      snackBarTheme: SnackBarThemeData(
        behavior: SnackBarBehavior.floating,
        backgroundColor: tones.ink,
        contentTextStyle: text.bodyMedium?.copyWith(color: bg),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(AppRadius.small)),
      ),
      progressIndicatorTheme: ProgressIndicatorThemeData(
        color: tones.primary,
        linearTrackColor: tones.line,
        circularTrackColor: tones.line,
      ),
      dividerTheme: DividerThemeData(color: tones.line, thickness: 1, space: 1),
      listTileTheme: ListTileThemeData(
        iconColor: tones.inkSoft,
        titleTextStyle: text.titleSmall,
        subtitleTextStyle: text.bodySmall,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(AppRadius.small)),
      ),
    );
  }
}
