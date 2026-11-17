// Session database on the web: IndexedDB.
import 'package:sembast_web/sembast_web.dart';

Future<Database> openSessionDatabase() => databaseFactoryWeb.openDatabase('ecg_card_sessions');
