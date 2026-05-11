import 'dart:async';
import 'dart:convert';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';
import 'package:url_launcher/url_launcher.dart';

const defaultBaseUrl = 'http://192.168.4.1';
const pageSize = 50;
const largePreviewBytes = 5 * 1024 * 1024;

void main() {
  runApp(const EspNasApp());
}

class EspNasApp extends StatelessWidget {
  const EspNasApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ESP32 SD NAS',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xff1f6f68)),
        useMaterial3: true,
        visualDensity: VisualDensity.compact,
      ),
      home: const BrowserPage(),
    );
  }
}

class EspFileItem {
  const EspFileItem({
    required this.name,
    required this.type,
    required this.size,
  });

  factory EspFileItem.fromJson(Map<String, dynamic> json) {
    return EspFileItem(
      name: json['name'] as String? ?? '',
      type: json['type'] as String? ?? 'file',
      size: (json['size'] as num?)?.toInt() ?? 0,
    );
  }

  final String name;
  final String type;
  final int size;

  bool get isDir => type == 'dir';
}

class ListPage {
  const ListPage({
    required this.items,
    required this.count,
    required this.hasMore,
  });

  factory ListPage.fromJson(Map<String, dynamic> json) {
    final rawItems = (json['items'] as List<dynamic>? ?? const []);
    return ListPage(
      items: rawItems
          .whereType<Map<String, dynamic>>()
          .map(EspFileItem.fromJson)
          .where((item) => item.name.isNotEmpty)
          .toList(growable: false),
      count: (json['count'] as num?)?.toInt() ?? rawItems.length,
      hasMore: json['hasMore'] == true,
    );
  }

  final List<EspFileItem> items;
  final int count;
  final bool hasMore;
}

class PreviewImage {
  const PreviewImage({required this.bytes, required this.source});

  final Uint8List bytes;
  final String source;
}

class ApiException implements Exception {
  ApiException(this.message);

  final String message;

  @override
  String toString() => message;
}

class EspApi {
  EspApi(String baseUrl, {this.debugLog}) : baseUrl = _cleanBaseUrl(baseUrl);

  final String baseUrl;
  final void Function(String message)? debugLog;

  static String _cleanBaseUrl(String value) {
    var cleaned = value.trim();
    while (cleaned.endsWith('/')) {
      cleaned = cleaned.substring(0, cleaned.length - 1);
    }
    return cleaned.isEmpty ? defaultBaseUrl : cleaned;
  }

  Uri uri(String path, [Map<String, String>? query]) {
    return Uri.parse('$baseUrl$path').replace(queryParameters: query);
  }

  Future<Map<String, dynamic>> _getJson(
    String path, [
    Map<String, String>? query,
  ]) async {
    final requestUri = uri(path, query);
    final stopwatch = Stopwatch()..start();
    debugLog?.call('GET $requestUri');
    final response = await http
        .get(requestUri)
        .timeout(const Duration(seconds: 12));
    debugLog?.call(
      'GET ${requestUri.path} -> ${response.statusCode} '
      '${response.bodyBytes.length}B in ${stopwatch.elapsedMilliseconds}ms',
    );
    return _decodeJson(response);
  }

  Future<Map<String, dynamic>> _postJson(
    String path, [
    Map<String, String>? query,
  ]) async {
    final requestUri = uri(path, query);
    final stopwatch = Stopwatch()..start();
    debugLog?.call('POST $requestUri');
    final response = await http
        .post(requestUri)
        .timeout(const Duration(seconds: 20));
    debugLog?.call(
      'POST ${requestUri.path} -> ${response.statusCode} '
      '${response.bodyBytes.length}B in ${stopwatch.elapsedMilliseconds}ms',
    );
    return _decodeJson(response);
  }

  Map<String, dynamic> _decodeJson(http.Response response) {
    Map<String, dynamic> body;
    try {
      body = jsonDecode(response.body) as Map<String, dynamic>;
    } catch (_) {
      throw ApiException('Unexpected response ${response.statusCode}');
    }

    if (response.statusCode < 200 ||
        response.statusCode >= 300 ||
        body['ok'] != true) {
      throw ApiException(body['error'] as String? ?? 'Request failed');
    }

    return body;
  }

  Future<Map<String, dynamic>> info() => _getJson('/api/info');

  Future<ListPage> list({
    required String path,
    required int offset,
    int limit = pageSize,
  }) async {
    final json = await _getJson('/api/list', {
      'path': path,
      'offset': '$offset',
      'limit': '$limit',
      'sort': 'none',
    });
    return ListPage.fromJson(json);
  }

  Uri downloadUri(String path, {required bool inline}) {
    return uri('/api/download', {'path': path, if (inline) 'inline': '1'});
  }

  Uri previewUri(String path) {
    return uri('/api/preview', {'path': path});
  }

  Future<PreviewImage> preview(String path) async {
    final requestUri = previewUri(path);
    final stopwatch = Stopwatch()..start();
    debugLog?.call('GET $requestUri');
    final response = await http
        .get(requestUri, headers: const {'Accept': 'image/jpeg'})
        .timeout(const Duration(seconds: 12));
    debugLog?.call(
      'GET ${requestUri.path} -> ${response.statusCode} '
      '${response.bodyBytes.length}B in ${stopwatch.elapsedMilliseconds}ms '
      'source=${response.headers['x-preview-source'] ?? '-'}',
    );

    if (response.statusCode < 200 || response.statusCode >= 300) {
      String message = 'No fast preview available';
      try {
        final json = jsonDecode(response.body) as Map<String, dynamic>;
        message = json['error'] as String? ?? message;
      } catch (_) {
        message = 'Preview failed with HTTP ${response.statusCode}';
      }
      throw ApiException(message);
    }

    return PreviewImage(
      bytes: response.bodyBytes,
      source: response.headers['x-preview-source'] ?? 'thumbnail',
    );
  }

  Future<void> mkdir(String path) async {
    await _postJson('/api/mkdir', {'path': path});
  }

  Future<void> delete(String path) async {
    await _postJson('/api/delete', {'path': path});
  }

  Future<void> rename(String from, String to) async {
    await _postJson('/api/rename', {'from': from, 'to': to});
  }

  Future<void> upload(String folderPath, PlatformFile file) async {
    final requestUri = uri('/api/upload', {'path': folderPath});
    final request = http.MultipartRequest('POST', requestUri);
    final stopwatch = Stopwatch()..start();
    debugLog?.call('UPLOAD ${file.name} ${file.size}B -> $requestUri');

    if (!kIsWeb && file.path != null) {
      request.files.add(
        await http.MultipartFile.fromPath(
          'file',
          file.path!,
          filename: file.name,
        ),
      );
    } else if (file.bytes != null) {
      request.files.add(
        http.MultipartFile.fromBytes('file', file.bytes!, filename: file.name),
      );
    } else {
      throw ApiException('Could not read selected file');
    }

    final streamed = await request.send();
    final response = await http.Response.fromStream(streamed);
    debugLog?.call(
      'UPLOAD ${file.name} -> ${response.statusCode} '
      '${response.bodyBytes.length}B in ${stopwatch.elapsedMilliseconds}ms',
    );
    _decodeJson(response);
  }
}

class BrowserPage extends StatefulWidget {
  const BrowserPage({super.key});

  @override
  State<BrowserPage> createState() => _BrowserPageState();
}

class _BrowserPageState extends State<BrowserPage> {
  final _baseUrlController = TextEditingController(text: defaultBaseUrl);
  EspApi? _api;
  Map<String, dynamic>? _info;
  String _currentPath = '/';
  String _status = 'Enter the ESP32 API address to connect.';
  List<EspFileItem> _items = [];
  int _offset = 0;
  bool _hasMore = false;
  bool _loading = false;
  bool _connected = false;
  bool _showDebug = true;
  final List<String> _debugMessages = [];

  @override
  void initState() {
    super.initState();
    _loadSavedBaseUrl();
  }

  @override
  void dispose() {
    _baseUrlController.dispose();
    super.dispose();
  }

  Future<void> _loadSavedBaseUrl() async {
    final prefs = await SharedPreferences.getInstance();
    final saved = prefs.getString('esp32_base_url');
    if (saved != null && saved.isNotEmpty) {
      _baseUrlController.text = saved;
    }
  }

  void _debug(String message) {
    final timestamp = DateTime.now().toIso8601String().substring(11, 19);
    final line = '$timestamp  $message';
    debugPrint(line);

    if (!mounted) {
      return;
    }

    setState(() {
      _debugMessages.insert(0, line);
      if (_debugMessages.length > 60) {
        _debugMessages.removeRange(60, _debugMessages.length);
      }
    });
  }

  Future<void> _connect() async {
    final api = EspApi(_baseUrlController.text, debugLog: _debug);
    setState(() {
      _loading = true;
      _status = 'Connecting...';
    });
    _debug('Connect start baseUrl=${api.baseUrl}');

    try {
      final info = await api.info();
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString('esp32_base_url', api.baseUrl);

      setState(() {
        _api = api;
        _info = info;
        _connected = true;
        _currentPath = '/';
        _items = [];
        _offset = 0;
        _hasMore = false;
        _status = 'Connected. Loading / ...';
      });

      _debug('Connected to ${api.baseUrl}; loading root folder');
      await _refresh(force: true);
    } catch (error) {
      _debug('Connect failed: $error');
      setState(() {
        _connected = false;
        _status = 'Connection failed: $error';
      });
    } finally {
      if (mounted) {
        setState(() => _loading = false);
      }
    }
  }

  Future<void> _refresh({bool force = false}) async {
    _debug('Refresh path=$_currentPath force=$force');
    setState(() {
      _items = [];
      _offset = 0;
      _hasMore = false;
    });
    await _loadMore(force: force);
  }

  Future<void> _loadMore({bool force = false}) async {
    final api = _api;
    if (api == null) {
      _debug('Load skipped: API is not connected');
      return;
    }

    if (_loading && !force) {
      _debug('Load skipped: another operation is busy');
      return;
    }

    setState(() {
      _loading = true;
      _status = _offset == 0 ? 'Loading folder...' : 'Loading more...';
    });

    try {
      _debug('List request path=$_currentPath offset=$_offset limit=$pageSize');
      final page = await api.list(path: _currentPath, offset: _offset);
      _debug(
        'List response items=${page.items.length} count=${page.count} '
        'hasMore=${page.hasMore}',
      );
      setState(() {
        _items = [..._items, ...page.items];
        _offset += page.count;
        _hasMore = page.hasMore;
        _status = _items.isEmpty
            ? 'Empty folder'
            : 'Loaded ${_items.length} item(s)';
      });
    } catch (error) {
      _debug('List failed: $error');
      setState(() => _status = 'List failed: $error');
    } finally {
      if (mounted) {
        setState(() => _loading = false);
      }
    }
  }

  Future<void> _openFolder(String path) async {
    setState(() => _currentPath = path);
    await _refresh();
  }

  Future<void> _goBack() async {
    if (_currentPath == '/') {
      return;
    }
    final parts = _currentPath
        .split('/')
        .where((part) => part.isNotEmpty)
        .toList();
    parts.removeLast();
    await _openFolder(parts.isEmpty ? '/' : '/${parts.join('/')}');
  }

  String _joinPath(String base, String name) {
    return base == '/' ? '/$name' : '$base/$name';
  }

  String _formatBytes(int bytes) {
    if (bytes < 1024) return '$bytes B';
    if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} KB';
    if (bytes < 1024 * 1024 * 1024) {
      return '${(bytes / 1024 / 1024).toStringAsFixed(1)} MB';
    }
    return '${(bytes / 1024 / 1024 / 1024).toStringAsFixed(1)} GB';
  }

  bool _isJpeg(String name) {
    final lower = name.toLowerCase();
    return lower.endsWith('.jpg') || lower.endsWith('.jpeg');
  }

  bool _isVideo(String name) {
    final lower = name.toLowerCase();
    return lower.endsWith('.mp4') ||
        lower.endsWith('.mov') ||
        lower.endsWith('.m4v') ||
        lower.endsWith('.3gp') ||
        lower.endsWith('.avi') ||
        lower.endsWith('.mkv') ||
        lower.endsWith('.webm');
  }

  bool _supportsFastPreview(String name) {
    final lower = name.toLowerCase();
    return _isJpeg(name) ||
        _isVideo(name) ||
        lower.endsWith('.png') ||
        lower.endsWith('.webp') ||
        lower.endsWith('.gif') ||
        lower.endsWith('.heic') ||
        lower.endsWith('.heif');
  }

  Future<void> _showFastPreview(EspFileItem item) async {
    final api = _api;
    if (api == null) return;

    final path = _joinPath(_currentPath, item.name);
    _debug('Fast preview $path');

    PreviewImage? preview;
    Object? previewError;

    setState(() {
      _loading = true;
      _status = 'Loading fast preview...';
    });

    try {
      preview = await api.preview(path);
      _debug(
        'Fast preview loaded ${_formatBytes(preview.bytes.length)} '
        'source=${preview.source}',
      );
    } catch (error) {
      previewError = error;
      _debug('Fast preview failed for $path: $error');
    } finally {
      if (mounted) {
        setState(() {
          _loading = false;
          _status = preview == null
              ? 'No fast preview for ${item.name}'
              : 'Fast preview loaded';
        });
      }
    }

    if (!mounted) return;

    await showDialog<void>(
      context: context,
      builder: (context) {
        final loadedPreview = preview;
        return Dialog(
          insetPadding: const EdgeInsets.all(14),
          clipBehavior: Clip.antiAlias,
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 760, maxHeight: 760),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                AppBar(
                  automaticallyImplyLeading: false,
                  title: Text(
                    item.name,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                  ),
                  actions: [
                    IconButton(
                      tooltip: 'Close',
                      onPressed: () => Navigator.of(context).pop(),
                      icon: const Icon(Icons.close),
                    ),
                  ],
                ),
                Flexible(
                  child: Container(
                    color: Colors.black,
                    alignment: Alignment.center,
                    child: loadedPreview == null
                        ? Padding(
                            padding: const EdgeInsets.all(24),
                            child: Column(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                const Icon(
                                  Icons.image_not_supported,
                                  color: Colors.white,
                                  size: 42,
                                ),
                                const SizedBox(height: 12),
                                Text(
                                  previewError?.toString() ??
                                      'No fast thumbnail is available for this file.',
                                  textAlign: TextAlign.center,
                                  style: const TextStyle(color: Colors.white),
                                ),
                                const SizedBox(height: 12),
                                FilledButton.icon(
                                  onPressed: () {
                                    Navigator.of(context).pop();
                                    _launchFile(item, inline: true);
                                  },
                                  icon: const Icon(Icons.open_in_new),
                                  label: Text(
                                    _isVideo(item.name)
                                        ? 'Open Video'
                                        : 'Open Original',
                                  ),
                                ),
                              ],
                            ),
                          )
                        : Image.memory(
                            loadedPreview.bytes,
                            fit: BoxFit.contain,
                            gaplessPlayback: true,
                          ),
                  ),
                ),
                Padding(
                  padding: const EdgeInsets.all(8),
                  child: Row(
                    mainAxisAlignment: MainAxisAlignment.end,
                    children: [
                      TextButton.icon(
                        onPressed: () {
                          Navigator.of(context).pop();
                          _launchFile(item, inline: true);
                        },
                        icon: const Icon(Icons.open_in_new),
                        label: Text(
                          _isVideo(item.name) ? 'Open Video' : 'Open Original',
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        );
      },
    );
  }

  Future<void> _launchFile(EspFileItem item, {required bool inline}) async {
    final api = _api;
    if (api == null) return;

    if (inline && item.size > largePreviewBytes) {
      final ok = await _confirm(
        'Large preview',
        '${item.name} is ${_formatBytes(item.size)}. Open it anyway?',
      );
      if (!ok) return;
    }

    final uri = api.downloadUri(
      _joinPath(_currentPath, item.name),
      inline: inline,
    );
    final launched = await launchUrl(uri, mode: LaunchMode.externalApplication);
    if (!launched && mounted) {
      setState(() => _status = 'Could not open $uri');
    }
  }

  Future<void> _upload() async {
    final api = _api;
    if (api == null) return;

    final result = await FilePicker.pickFiles(withData: kIsWeb);
    if (result == null || result.files.isEmpty) {
      return;
    }

    final file = result.files.single;
    setState(() {
      _loading = true;
      _status = 'Uploading ${file.name}...';
    });

    try {
      await api.upload(_currentPath, file);
      setState(() => _status = 'Uploaded ${file.name}');
      await _refresh(force: true);
    } catch (error) {
      _debug('Upload failed: $error');
      setState(() => _status = 'Upload failed: $error');
    } finally {
      if (mounted) {
        setState(() => _loading = false);
      }
    }
  }

  Future<void> _createFolder() async {
    final name = await _textDialog(title: 'New folder', label: 'Folder name');
    if (name == null || name.trim().isEmpty) return;

    await _runMutation(
      'Creating folder...',
      () => _api!.mkdir(_joinPath(_currentPath, name.trim())),
    );
  }

  Future<void> _rename(EspFileItem item) async {
    final name = await _textDialog(
      title: 'Rename',
      label: 'New name',
      initialValue: item.name,
    );
    if (name == null || name.trim().isEmpty || name.trim() == item.name) return;

    await _runMutation(
      'Renaming...',
      () => _api!.rename(
        _joinPath(_currentPath, item.name),
        _joinPath(_currentPath, name.trim()),
      ),
    );
  }

  Future<void> _delete(EspFileItem item) async {
    final ok = await _confirm(
      'Delete ${item.isDir ? 'folder' : 'file'}',
      _joinPath(_currentPath, item.name),
    );
    if (!ok) return;

    await _runMutation(
      'Deleting...',
      () => _api!.delete(_joinPath(_currentPath, item.name)),
    );
  }

  Future<void> _runMutation(
    String message,
    Future<void> Function() action,
  ) async {
    if (_api == null) return;

    setState(() {
      _loading = true;
      _status = message;
    });

    try {
      await action();
      await _refresh(force: true);
    } catch (error) {
      _debug('Mutation failed: $error');
      setState(() => _status = 'Operation failed: $error');
    } finally {
      if (mounted) {
        setState(() => _loading = false);
      }
    }
  }

  Future<String?> _textDialog({
    required String title,
    required String label,
    String initialValue = '',
  }) async {
    final controller = TextEditingController(text: initialValue);
    final result = await showDialog<String>(
      context: context,
      builder: (context) {
        return AlertDialog(
          title: Text(title),
          content: TextField(
            controller: controller,
            autofocus: true,
            decoration: InputDecoration(labelText: label),
            onSubmitted: (value) => Navigator.of(context).pop(value),
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(context).pop(),
              child: const Text('Cancel'),
            ),
            FilledButton(
              onPressed: () => Navigator.of(context).pop(controller.text),
              child: const Text('OK'),
            ),
          ],
        );
      },
    );
    controller.dispose();
    return result;
  }

  Future<bool> _confirm(String title, String message) async {
    return await showDialog<bool>(
          context: context,
          builder: (context) {
            return AlertDialog(
              title: Text(title),
              content: Text(message),
              actions: [
                TextButton(
                  onPressed: () => Navigator.of(context).pop(false),
                  child: const Text('Cancel'),
                ),
                FilledButton(
                  onPressed: () => Navigator.of(context).pop(true),
                  child: const Text('Continue'),
                ),
              ],
            );
          },
        ) ??
        false;
  }

  void _showActions(EspFileItem item) {
    showModalBottomSheet<void>(
      context: context,
      showDragHandle: true,
      builder: (context) {
        return SafeArea(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              ListTile(
                title: Text(
                  item.name,
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                ),
                subtitle: Text(item.isDir ? 'Folder' : _formatBytes(item.size)),
              ),
              if (item.isDir)
                ListTile(
                  leading: const Icon(Icons.folder_open),
                  title: const Text('Open'),
                  onTap: () {
                    Navigator.pop(context);
                    _openFolder(_joinPath(_currentPath, item.name));
                  },
                )
              else ...[
                ListTile(
                  leading: const Icon(Icons.download),
                  title: const Text('Download'),
                  onTap: () {
                    Navigator.pop(context);
                    _launchFile(item, inline: false);
                  },
                ),
                if (_supportsFastPreview(item.name))
                  ListTile(
                    leading: const Icon(Icons.image_search),
                    title: const Text('Fast Preview'),
                    subtitle: Text(
                      _isVideo(item.name)
                          ? 'Uses sidecar thumbnail only'
                          : 'Uses EXIF or cached thumbnail',
                    ),
                    onTap: () {
                      Navigator.pop(context);
                      _showFastPreview(item);
                    },
                  ),
                ListTile(
                  leading: const Icon(Icons.open_in_new),
                  title: Text(
                    _isVideo(item.name) ? 'Open Video' : 'Open Original',
                  ),
                  subtitle: Text(
                    _isVideo(item.name)
                        ? 'Streams the full video'
                        : 'Loads the full-size file',
                  ),
                  onTap: () {
                    Navigator.pop(context);
                    _launchFile(item, inline: true);
                  },
                ),
              ],
              ListTile(
                leading: const Icon(Icons.drive_file_rename_outline),
                title: const Text('Rename'),
                onTap: () {
                  Navigator.pop(context);
                  _rename(item);
                },
              ),
              ListTile(
                leading: const Icon(Icons.delete_outline),
                title: const Text('Delete'),
                onTap: () {
                  Navigator.pop(context);
                  _delete(item);
                },
              ),
            ],
          ),
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('ESP32 SD NAS'),
        actions: [
          IconButton(
            tooltip: 'Debug log',
            onPressed: () => setState(() => _showDebug = !_showDebug),
            icon: Icon(
              _showDebug ? Icons.bug_report : Icons.bug_report_outlined,
            ),
          ),
          IconButton(
            tooltip: 'Reconnect',
            onPressed: _loading ? null : _connect,
            icon: const Icon(Icons.wifi_find),
          ),
        ],
      ),
      body: SafeArea(child: _connected ? _buildBrowser() : _buildConnection()),
    );
  }

  Widget _buildConnection() {
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Text(
          'ESP32 API address',
          style: Theme.of(context).textTheme.titleMedium,
        ),
        const SizedBox(height: 8),
        TextField(
          controller: _baseUrlController,
          keyboardType: TextInputType.url,
          decoration: const InputDecoration(
            border: OutlineInputBorder(),
            hintText: defaultBaseUrl,
            prefixIcon: Icon(Icons.link),
          ),
          onSubmitted: (_) => _connect(),
        ),
        const SizedBox(height: 12),
        FilledButton.icon(
          onPressed: _loading ? null : _connect,
          icon: const Icon(Icons.power),
          label: const Text('Connect'),
        ),
        const SizedBox(height: 16),
        Text(_status),
        const SizedBox(height: 16),
        _buildDebugPanel(),
      ],
    );
  }

  Widget _buildBrowser() {
    return Column(
      children: [
        _buildInfoStrip(),
        _buildPathBar(),
        _buildToolbar(),
        if (_loading) const LinearProgressIndicator(minHeight: 2),
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 8, 12, 6),
          child: Align(
            alignment: Alignment.centerLeft,
            child: Text(_status, style: Theme.of(context).textTheme.bodySmall),
          ),
        ),
        _buildDebugPanel(),
        Expanded(
          child: RefreshIndicator(
            onRefresh: _refresh,
            child: ListView.builder(
              itemCount: _items.length + (_hasMore ? 1 : 0),
              itemBuilder: (context, index) {
                if (index == _items.length) {
                  return Padding(
                    padding: const EdgeInsets.all(12),
                    child: FilledButton(
                      onPressed: _loading ? null : _loadMore,
                      child: const Text('Load more'),
                    ),
                  );
                }

                final item = _items[index];
                return ListTile(
                  dense: true,
                  leading: Icon(
                    item.isDir ? Icons.folder : Icons.insert_drive_file,
                  ),
                  title: Text(
                    item.name,
                    maxLines: 2,
                    overflow: TextOverflow.ellipsis,
                  ),
                  subtitle: Text(
                    item.isDir ? 'Folder' : _formatBytes(item.size),
                  ),
                  trailing: const Icon(Icons.more_vert),
                  onTap: () {
                    if (item.isDir) {
                      _openFolder(_joinPath(_currentPath, item.name));
                    } else {
                      _showActions(item);
                    }
                  },
                  onLongPress: () => _showActions(item),
                );
              },
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildDebugPanel() {
    if (!_showDebug) {
      return const SizedBox.shrink();
    }

    final lines = _debugMessages.take(8).toList(growable: false);

    return Container(
      width: double.infinity,
      margin: const EdgeInsets.fromLTRB(12, 0, 12, 8),
      padding: const EdgeInsets.all(10),
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.surfaceContainerHighest,
        borderRadius: BorderRadius.circular(8),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.bug_report_outlined, size: 16),
              const SizedBox(width: 6),
              Text('Debug', style: Theme.of(context).textTheme.labelLarge),
              const Spacer(),
              TextButton(
                onPressed: () => setState(_debugMessages.clear),
                child: const Text('Clear'),
              ),
            ],
          ),
          if (lines.isEmpty)
            const Text('No debug messages yet.')
          else
            for (final line in lines)
              Padding(
                padding: const EdgeInsets.only(top: 3),
                child: SelectableText(
                  line,
                  style: Theme.of(
                    context,
                  ).textTheme.bodySmall?.copyWith(fontFamily: 'monospace'),
                ),
              ),
        ],
      ),
    );
  }

  Widget _buildInfoStrip() {
    final info = _info;
    if (info == null) {
      return const SizedBox.shrink();
    }

    final mode = info['mode'] ?? '?';
    final ip = info['ip'] ?? '?';
    final rssi = info['rssi'] ?? 0;
    final heap = (info['heap'] as num?)?.toInt() ?? 0;

    return Container(
      width: double.infinity,
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
      color: Theme.of(context).colorScheme.surfaceContainerHighest,
      child: Text(
        'mode $mode  |  ip $ip  |  rssi $rssi  |  heap ${_formatBytes(heap)}',
      ),
    );
  }

  Widget _buildPathBar() {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      decoration: BoxDecoration(
        border: Border(
          bottom: BorderSide(color: Theme.of(context).dividerColor),
        ),
      ),
      child: Text(
        _currentPath,
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
        style: Theme.of(context).textTheme.titleSmall,
      ),
    );
  }

  Widget _buildToolbar() {
    return Padding(
      padding: const EdgeInsets.fromLTRB(8, 8, 8, 0),
      child: Wrap(
        spacing: 8,
        runSpacing: 8,
        children: [
          OutlinedButton.icon(
            onPressed: _loading ? null : _goBack,
            icon: const Icon(Icons.arrow_upward),
            label: const Text('Back'),
          ),
          OutlinedButton.icon(
            onPressed: _loading ? null : _refresh,
            icon: const Icon(Icons.refresh),
            label: const Text('Refresh'),
          ),
          OutlinedButton.icon(
            onPressed: _loading ? null : _createFolder,
            icon: const Icon(Icons.create_new_folder),
            label: const Text('Folder'),
          ),
          FilledButton.icon(
            onPressed: _loading ? null : _upload,
            icon: const Icon(Icons.upload_file),
            label: const Text('Upload'),
          ),
        ],
      ),
    );
  }
}
