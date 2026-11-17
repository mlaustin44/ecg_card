// Session database on iOS / Android / desktop: a file in the documents dir.
import 'package:path_provider/path_provider.dart';
import 'package:sembast/sembast_io.dart';

Future<Database> openSessionDatabase() async {
  final dir = await getApplicationDocumentsDirectory();
  return databaseFactoryIo.openDatabase('${dir.path}/ecg_card_sessions.db');
}
