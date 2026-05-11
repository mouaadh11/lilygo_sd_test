import 'package:flutter_test/flutter_test.dart';

import 'package:flutter_client/main.dart';

void main() {
  testWidgets('shows connection screen', (tester) async {
    await tester.pumpWidget(const EspNasApp());

    expect(find.text('ESP32 SD NAS'), findsOneWidget);
    expect(find.text('ESP32 API address'), findsOneWidget);
    expect(find.text('Connect'), findsOneWidget);
  });
}
