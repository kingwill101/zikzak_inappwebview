import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:zikzak_inappwebview_platform_interface/zikzak_inappwebview_platform_interface.dart';

import '../find_interaction/find_interaction_controller.dart';
import 'in_app_webview_controller.dart';

const _gdkKeyMap = {
  'Enter': 0xFF0D,
  'Backspace': 0xFF08,
  'Delete': 0xFFFF,
  'Tab': 0xFF09,
  'Escape': 0xFF1B,
  'ArrowUp': 0xFF52,
  'ArrowDown': 0xFF54,
  'ArrowLeft': 0xFF51,
  'ArrowRight': 0xFF53,
  'Home': 0xFF50,
  'End': 0xFF57,
  'PageUp': 0xFF55,
  'PageDown': 0xFF56,
  'Shift': 0xFFE1,
  'Control': 0xFFE3,
  'Alt': 0xFFE9,
  'Meta': 0xFFE7,
  'CapsLock': 0xFFE5,
  'Space': 0x0020,
};

int _toGdkKeyval(LogicalKeyboardKey key) {
  if (key.keyId >= 0x0010000000000 && key.keyId <= 0x00100000FFFFF) {
    final unicode = key.keyId & 0x00FFFFFF;
    if (unicode >= 0x20 && unicode <= 0x7E) return unicode;
    if (unicode == 0x08) return 0xFF08;
    if (unicode == 0x09) return 0xFF09;
    if (unicode == 0x0D) return 0xFF0D;
    if (unicode == 0x1B) return 0xFF1B;
    if (unicode == 0x7F) return 0xFFFF;
  }
  return _gdkKeyMap[key.keyLabel] ?? 0;
}

class LinuxInAppWebViewWidget extends PlatformInAppWebViewWidget {
  LinuxInAppWebViewWidget(PlatformInAppWebViewWidgetCreationParams params)
    : super.implementation(params);

  @override
  Widget build(BuildContext context) {
    return _LinuxInAppWebView(params: params);
  }

  @override
  void dispose() {
    // nothing to dispose here, the widget disposes the controller
  }

  @override
  T controllerFromPlatform<T>(PlatformInAppWebViewController controller) {
    if (params.controllerFromPlatform != null) {
      return params.controllerFromPlatform!(controller) as T;
    }
    return controller as T;
  }
}

class _LinuxInAppWebView extends StatefulWidget {
  final PlatformInAppWebViewWidgetCreationParams params;

  const _LinuxInAppWebView({required this.params});

  @override
  State<_LinuxInAppWebView> createState() => _LinuxInAppWebViewState();
}

class _LinuxInAppWebViewState extends State<_LinuxInAppWebView> {
  LinuxInAppWebViewController? _controller;
  int? _textureId;
  final FocusNode _focusNode = FocusNode();
  static const MethodChannel _sharedChannel = MethodChannel(
    'zikzak_inappwebview_linux',
  );
  int? _lastSentWidth;
  int? _lastSentHeight;
  bool _buttonsDown = false;

  @override
  void initState() {
    super.initState();
    _focusNode.addListener(_onFocusChange);
    _createWebView();
  }

  void _onFocusChange() {
    if (_focusNode.hasFocus) {
      _controller?.channel.invokeMethod('focus');
    }
  }

  void _sendResize(int width, int height) {
    if (_lastSentWidth != width || _lastSentHeight != height) {
      _lastSentWidth = width;
      _lastSentHeight = height;
      _controller?.channel.invokeMethod('resize', {
        'width': width,
        'height': height,
      });
    }
  }

  void _sendPointerEvent(String type, PointerEvent event) {
    if (type == 'pointerDown') _buttonsDown = true;
    if (type == 'pointerUp') _buttonsDown = false;
    _controller?.channel.invokeMethod('pointerEvent', {
      'type': type,
      'x': event.position.dx,
      'y': event.position.dy,
      'buttons': event.buttons,
    });
  }

  void _sendScrollEvent(PointerScrollEvent event) {
    _controller?.channel.invokeMethod('scrollEvent', {
      'x': event.position.dx,
      'y': event.position.dy,
      'deltaX': event.scrollDelta.dx,
      'deltaY': event.scrollDelta.dy,
    });
  }

  KeyEventResult _handleKeyEvent(FocusNode node, KeyEvent event) {
    if (event is KeyDownEvent || event is KeyRepeatEvent || event is KeyUpEvent) {
      final type = event is KeyUpEvent ? 'keyup' : (event is KeyRepeatEvent ? 'keyrepeat' : 'keydown');
      final keyval = _toGdkKeyval(event.logicalKey);
      if (keyval == 0) return KeyEventResult.ignored;
      _controller?.channel.invokeMethod('keyEvent', {
        'type': type,
        'keyval': keyval,
        'characters': event.character ?? '',
      });
      return KeyEventResult.handled;
    }
    return KeyEventResult.ignored;
  }

  Future<void> _createWebView() async {
    // Generate a unique ID for the webview.
    // In MacOS implementation, platform view ID is provided by the platform.
    // Here we generate it to match our custom create logic.
    var id = DateTime.now().microsecondsSinceEpoch.toString();

    try {
      var textureId = await _sharedChannel.invokeMethod('create', {'id': id});
      if (textureId != null && mounted) {
        setState(() {
          _textureId = textureId;
        });
        // Use the ID we generated to create the controller, as the native side used it too.
        _onPlatformViewCreated(id);
      }
    } catch (e) {
      print("Error creating webview: $e");
    }
  }

  void _onPlatformViewCreated(String id) {
    _controller = LinuxInAppWebViewController(
      PlatformInAppWebViewControllerCreationParams(
        id: id,
        webviewParams: widget.params,
      ),
    );

    if (widget.params.findInteractionController != null) {
      var findInteractionController =
          widget.params.findInteractionController
              as LinuxFindInteractionController;
      findInteractionController.channel = MethodChannel(
        'wtf.zikzak/zikzak_inappwebview_find_interaction_$id',
      );
      findInteractionController.setupMethodHandler();
    }

    if (widget.params.initialUrlRequest != null) {
      _controller!.loadUrl(urlRequest: widget.params.initialUrlRequest!);
    }

    if (widget.params.onWebViewCreated != null) {
      widget.params.onWebViewCreated!(
        widget.params.controllerFromPlatform?.call(_controller!) ?? _controller!,
      );
    }
  }

  @override
  void dispose() {
    _focusNode.removeListener(_onFocusChange);
    _focusNode.dispose();
    _controller?.dispose();
    _controller = null;
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_textureId != null) {
      return LayoutBuilder(
        builder: (context, constraints) {
          final w = constraints.maxWidth.round();
          final h = constraints.maxHeight.round();
          if (w > 0 && h > 0) _sendResize(w, h);
          return Focus(
            focusNode: _focusNode,
            onKeyEvent: _handleKeyEvent,
            child: Listener(
              onPointerDown: (e) => _sendPointerEvent('pointerDown', e),
              onPointerMove: (e) => _sendPointerEvent('pointerMove', e),
              onPointerUp: (e) => _sendPointerEvent('pointerUp', e),
              onPointerHover: (e) => _sendPointerEvent('pointerHover', e),
              onPointerSignal: (e) {
                if (e is PointerScrollEvent) _sendScrollEvent(e);
              },
              child: Texture(textureId: _textureId!),
            ),
          );
        },
      );
    }
    // Return a placeholder or empty container
    return Container(
      color: const Color(0xFFFFFFFF),
      child: const Center(
        child: Text("Linux InAppWebView (Implemented via Texture/Native)"),
      ),
    );
  }
}
