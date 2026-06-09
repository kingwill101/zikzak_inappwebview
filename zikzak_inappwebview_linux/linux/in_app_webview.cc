#include "include/zikzak_inappwebview_linux/in_app_webview.h"
#include <cstring>
#include <glib/gstdio.h>
#include <gdk/gdkkeysyms.h>
#include <iostream>

struct _InAppWebView {
  FlPixelBufferTexture parent_instance;
  FlPluginRegistrar* registrar;
  char* id;
  FlMethodChannel* channel;
  GtkWidget* web_view;
  GtkWidget* window;
  int64_t texture_id;

  uint8_t* buffer;
  int32_t width;
  int32_t height;
};

G_DEFINE_TYPE(InAppWebView, in_app_webview, fl_pixel_buffer_texture_get_type())

static gboolean in_app_webview_copy_pixels(FlPixelBufferTexture* texture,
                                           const uint8_t** buffer,
                                           uint32_t* width,
                                           uint32_t* height,
                                           GError** error) {
  InAppWebView* self = IN_APP_WEBVIEW(texture);

  if (self->buffer == nullptr) {
      *width = 1;
      *height = 1;
      static uint8_t dummy[4] = {255, 0, 0, 255};
      *buffer = dummy;
      return TRUE;
  }

  *buffer = self->buffer;
  *width = self->width;
  *height = self->height;
  return TRUE;
}

static void in_app_webview_dispose(GObject* object) {
  InAppWebView* self = IN_APP_WEBVIEW(object);
  if (self->window) {
    gtk_widget_destroy(self->window);
    self->window = nullptr;
    self->web_view = nullptr;
  } else if (self->web_view) {
    g_object_unref(self->web_view);
    self->web_view = nullptr;
  }
  if (self->channel) {
    g_object_unref(self->channel);
    self->channel = nullptr;
  }
  if (self->buffer) {
    g_free(self->buffer);
    self->buffer = nullptr;
  }
  if (self->id) {
    g_free(self->id);
    self->id = nullptr;
  }
  if (self->registrar) {
    g_object_unref(self->registrar);
  }
  G_OBJECT_CLASS(in_app_webview_parent_class)->dispose(object);
}

static void in_app_webview_class_init(InAppWebViewClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = in_app_webview_dispose;
  FL_PIXEL_BUFFER_TEXTURE_CLASS(klass)->copy_pixels = in_app_webview_copy_pixels;
}

static void in_app_webview_init(InAppWebView* self) {
    self->width = 1280;
    self->height = 720;
    self->buffer = (uint8_t*)g_malloc0(self->width * self->height * 4);

    // Fill with blue for initial state
    for (int i = 0; i < self->width * self->height; i++) {
        self->buffer[i * 4] = 0;     // R
        self->buffer[i * 4 + 1] = 0; // G
        self->buffer[i * 4 + 2] = 255; // B
        self->buffer[i * 4 + 3] = 255; // A
    }
}

static void in_app_webview_method_call_handler(FlMethodChannel* channel, FlMethodCall* method_call, gpointer user_data) {
    in_app_webview_handle_method_call(IN_APP_WEBVIEW(user_data), method_call);
}

// Helper to update texture from snapshot
static void on_snapshot_ready(GObject* source_object, GAsyncResult* res, gpointer user_data) {
    InAppWebView* self = IN_APP_WEBVIEW(user_data);
    GError* error = nullptr;
    WebKitWebView* web_view = WEBKIT_WEB_VIEW(source_object);
    cairo_surface_t* surface = webkit_web_view_get_snapshot_finish(web_view, res, &error);

    if (surface) {
        int width = cairo_image_surface_get_width(surface);
        int height = cairo_image_surface_get_height(surface);
        // int stride = cairo_image_surface_get_stride(surface); // Unused
        unsigned char* data = cairo_image_surface_get_data(surface);

        if (width != self->width || height != self->height) {
            g_free(self->buffer);
            self->width = width;
            self->height = height;
            self->buffer = (uint8_t*)g_malloc0(width * height * 4);
        }

        // Cairo uses ARGB or RGB24, usually premultiplied. Flutter expects RGBA.
        // WebKit snapshot is usually CAIRO_FORMAT_ARGB32 (premultiplied ARGB, host endian).

        // Convert ARGB to RGBA
        for (int i = 0; i < width * height; i++) {
            // uint32_t* pixel = (uint32_t*)(data + i * 4);

            uint8_t b = data[i * 4];
            uint8_t g = data[i * 4 + 1];
            uint8_t r = data[i * 4 + 2];
            uint8_t a = data[i * 4 + 3];

            self->buffer[i * 4] = r;
            self->buffer[i * 4 + 1] = g;
            self->buffer[i * 4 + 2] = b;
            self->buffer[i * 4 + 3] = a;
        }

        cairo_surface_destroy(surface);

        // Notify texture updated
        fl_texture_registrar_mark_texture_frame_available(
            fl_plugin_registrar_get_texture_registrar(self->registrar),
            FL_TEXTURE(self));
    } else {
        if (error) {
            g_warning("Snapshot failed: %s", error->message);
            g_error_free(error);
        }
    }

    g_object_unref(self);
}

static void on_screenshot_ready(GObject* source_object, GAsyncResult* res, gpointer user_data) {
    FlMethodCall* method_call = FL_METHOD_CALL(user_data);
    GError* error = nullptr;
    WebKitWebView* web_view = WEBKIT_WEB_VIEW(source_object);
    cairo_surface_t* surface = webkit_web_view_get_snapshot_finish(web_view, res, &error);

    if (!surface) {
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_null())), nullptr);
        if (error) {
            g_error_free(error);
        }
        g_object_unref(method_call);
        return;
    }

    cairo_surface_flush(surface);

    guchar* png_data = nullptr;
    gsize png_size = 0;

    GdkPixbuf* pixbuf = gdk_pixbuf_get_from_surface(
        surface, 0, 0,
        cairo_image_surface_get_width(surface),
        cairo_image_surface_get_height(surface)
    );

    if (pixbuf) {
        gchar* buffer = nullptr;
        gsize buffer_size = 0;
        gboolean saved = gdk_pixbuf_save_to_buffer(
            pixbuf, &buffer, &buffer_size, "png", nullptr, nullptr
        );
        if (saved && buffer && buffer_size > 0) {
            png_data = (guchar*)g_malloc(buffer_size);
            memcpy(png_data, buffer, buffer_size);
            png_size = buffer_size;
            g_free(buffer);
        }
        g_object_unref(pixbuf);
    }

    cairo_surface_destroy(surface);

    if (png_data && png_size > 0) {
        g_autoptr(FlValue) fl_data = fl_value_new_uint8_list(png_data, png_size);
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_data)), nullptr);
        g_free(png_data);
    } else {
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_null())), nullptr);
    }

    g_object_unref(method_call);
}

static void update_texture(InAppWebView* self) {
    webkit_web_view_get_snapshot(WEBKIT_WEB_VIEW(self->web_view),
                                 WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE,
                                 nullptr,
                                 on_snapshot_ready,
                                 g_object_ref(self));
}

static void on_load_changed(WebKitWebView* web_view, WebKitLoadEvent load_event, gpointer user_data) {
    InAppWebView* self = IN_APP_WEBVIEW(user_data);
    // Take snapshot on load events
    if (load_event == WEBKIT_LOAD_FINISHED) {
        update_texture(self);
    }
}

InAppWebView* in_app_webview_new(FlPluginRegistrar* registrar, const char* id) {
  InAppWebView* self = IN_APP_WEBVIEW(g_object_new(IN_APP_WEBVIEW_TYPE, nullptr));
  self->registrar = FL_PLUGIN_REGISTRAR(g_object_ref(registrar));
  self->id = g_strdup(id);

  g_autofree gchar* channel_name = g_strdup_printf("dev.zuzu/zikzak_inappwebview_%s", id);
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  self->channel = fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                                        channel_name,
                                        FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(self->channel, in_app_webview_method_call_handler, g_object_ref(self), g_object_unref);

  self->web_view = webkit_web_view_new();
  g_object_ref_sink(self->web_view);

  // Connect load-changed signal
  g_signal_connect(self->web_view, "load-changed", G_CALLBACK(on_load_changed), self);

  // Register texture
  FlTextureRegistrar* texture_registrar = fl_plugin_registrar_get_texture_registrar(registrar);
  if (fl_texture_registrar_register_texture(texture_registrar, FL_TEXTURE(self))) {
      self->texture_id = (int64_t)self;
  } else {
      self->texture_id = 0;
  }

  // Set initial size
  gtk_widget_set_size_request(self->web_view, 1280, 720);

  WebKitSettings* webkit_settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(self->web_view));
  webkit_settings_set_hardware_acceleration_policy(
      webkit_settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);

  self->window = gtk_offscreen_window_new();
  gtk_container_add(GTK_CONTAINER(self->window), self->web_view);
  gtk_window_set_default_size(GTK_WINDOW(self->window), 1280, 720);
  gtk_widget_show_all(self->window);

  return self;
}

int64_t in_app_webview_get_texture_id(InAppWebView* self) {
    return self->texture_id;
}

typedef struct {
    FlMethodCall* method_call;
    char* filename;
} PrintContext;

static void print_finished_callback(WebKitPrintOperation* operation, gpointer user_data) {
    PrintContext* context = (PrintContext*)user_data;
    FlMethodCall* method_call = context->method_call;
    char* filename = context->filename;

    GError* error = nullptr;
    gchar* contents = nullptr;
    gsize length = 0;

    if (g_file_get_contents(filename, &contents, &length, &error)) {
        FlValue* result = fl_value_new_uint8_list((const uint8_t*)contents, length);
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(result)), nullptr);
        g_free(contents);
    } else {
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_error_response_new("error", error->message, nullptr)), nullptr);
        g_error_free(error);
    }

    g_unlink(filename);
    g_free(filename);
    g_free(context);
    g_object_unref(method_call);
}

static void print_failed_callback(WebKitPrintOperation* operation, GError* error, gpointer user_data) {
    PrintContext* context = (PrintContext*)user_data;
    FlMethodCall* method_call = context->method_call;
    char* filename = context->filename;

    fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_error_response_new("error", error->message, nullptr)), nullptr);

    g_unlink(filename);
    g_free(filename);
    g_free(context);
    g_object_unref(method_call);
}

void in_app_webview_handle_method_call(InAppWebView* self, FlMethodCall* method_call) {
    const gchar* method = fl_method_call_get_name(method_call);
    FlValue* args = fl_method_call_get_args(method_call);

    if (strcmp(method, "getUrl") == 0) {
        const gchar* uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(self->web_view));
        g_autoptr(FlValue) result = uri ? fl_value_new_string(uri) : fl_value_new_null();
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(result)), nullptr);
    } else if (strcmp(method, "getHtml") == 0) {
        webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(self->web_view),
            "document.documentElement.outerHTML",
            -1,
            nullptr,
            nullptr,
            nullptr,
            [](GObject* object, GAsyncResult* result, gpointer user_data) {
                FlMethodCall* method_call = FL_METHOD_CALL(user_data);
                GError* error = nullptr;
                JSCValue* js_value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);

                if (!js_value) {
                    fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_error_response_new("error", error->message, nullptr)), nullptr);
                    g_error_free(error);
                } else {
                    if (jsc_value_is_string(js_value)) {
                        g_autofree gchar* str_value = jsc_value_to_string(js_value);
                        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_string(str_value))), nullptr);
                    } else {
                        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_null())), nullptr);
                    }
                    g_object_unref(js_value);
                }
                g_object_unref(method_call);
            },
            g_object_ref(method_call));
        return;
    } else if (strcmp(method, "loadUrl") == 0) {
        if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
             FlValue* urlRequest = fl_value_lookup_string(args, "urlRequest");
             if (urlRequest && fl_value_get_type(urlRequest) == FL_VALUE_TYPE_MAP) {
                 FlValue* urlVal = fl_value_lookup_string(urlRequest, "url");
                 if (urlVal && fl_value_get_type(urlVal) == FL_VALUE_TYPE_STRING) {
                     const char* url = fl_value_get_string(urlVal);
                     webkit_web_view_load_uri(WEBKIT_WEB_VIEW(self->web_view), url);
                     fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(true))), nullptr);
                     return;
                 }
             }
        }
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_error_response_new("error", "Invalid arguments", nullptr)), nullptr);
    } else if (strcmp(method, "createPdf") == 0) {
        WebKitPrintOperation* operation = webkit_print_operation_new(WEBKIT_WEB_VIEW(self->web_view));

        GtkPrintSettings* settings = gtk_print_settings_new();

        gchar* filename = g_strdup_printf("/tmp/flutter_inappwebview_print_%p_%ld.pdf", self, g_get_real_time());
        gchar* uri = g_strdup_printf("file://%s", filename);

        gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
        gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT, "pdf");

        webkit_print_operation_set_print_settings(operation, settings);

        PrintContext* context = g_new(PrintContext, 1);
        context->method_call = method_call;
        g_object_ref(context->method_call);
        context->filename = filename;

        g_signal_connect(operation, "finished", G_CALLBACK(print_finished_callback), context);
        g_signal_connect(operation, "failed", G_CALLBACK(print_failed_callback), context);

        webkit_print_operation_print(operation);

        g_object_unref(settings);
        g_free(uri);
        return;
    } else if (strcmp(method, "takeScreenshot") == 0) {
        webkit_web_view_get_snapshot(WEBKIT_WEB_VIEW(self->web_view),
                                     WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                     WEBKIT_SNAPSHOT_OPTIONS_NONE,
                                     nullptr,
                                     on_screenshot_ready,
                                     g_object_ref(method_call));
        return;
    } else if (strcmp(method, "evaluateJavascript") == 0) {
        if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
            FlValue* sourceVal = fl_value_lookup_string(args, "source");
            if (sourceVal && fl_value_get_type(sourceVal) == FL_VALUE_TYPE_STRING) {
                const char* source = fl_value_get_string(sourceVal);
                webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(self->web_view),
                    source, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
            }
        }
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(true))), nullptr);
    } else if (strcmp(method, "resize") == 0) {
        if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
            FlValue* widthVal = fl_value_lookup_string(args, "width");
            FlValue* heightVal = fl_value_lookup_string(args, "height");
            if (widthVal && heightVal &&
                fl_value_get_type(widthVal) == FL_VALUE_TYPE_INT &&
                fl_value_get_type(heightVal) == FL_VALUE_TYPE_INT) {
                int width = fl_value_get_int(widthVal);
                int height = fl_value_get_int(heightVal);
                if (width > 0 && height > 0) {
                    gtk_window_resize(GTK_WINDOW(self->window), width, height);
                    gtk_widget_set_size_request(self->web_view, width, height);
                    if (width != self->width || height != self->height) {
                        g_free(self->buffer);
                        self->width = width;
                        self->height = height;
                        self->buffer = (uint8_t*)g_malloc0(width * height * 4);
                    }
                    update_texture(self);
                }
            }
        }
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(true))), nullptr);
    } else if (strcmp(method, "focus") == 0) {
        gtk_widget_grab_focus(self->web_view);
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(true))), nullptr);
    } else if (strcmp(method, "pointerEvent") == 0) {
        if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
            FlValue* typeVal = fl_value_lookup_string(args, "type");
            FlValue* xVal = fl_value_lookup_string(args, "x");
            FlValue* yVal = fl_value_lookup_string(args, "y");
            if (typeVal && xVal && yVal &&
                fl_value_get_type(typeVal) == FL_VALUE_TYPE_STRING &&
                fl_value_get_type(xVal) == FL_VALUE_TYPE_FLOAT &&
                fl_value_get_type(yVal) == FL_VALUE_TYPE_FLOAT) {
                const char* type = fl_value_get_string(typeVal);
                double x = fl_value_get_float(xVal);
                double y = fl_value_get_float(yVal);
                GdkWindow* gdk_window = gtk_widget_get_window(self->web_view);
                GdkDisplay* display = gdk_display_get_default();
                GdkSeat* seat = gdk_display_get_default_seat(display);
                GdkDevice* pointer = gdk_seat_get_pointer(seat);
                g_message("[inappwebview] pointerEvent=%s xy=%.0f,%.0f window=%p", type, x, y, (void*)gdk_window);
                if (!gdk_window) {
                    g_warning("[inappwebview] no gdk_window for webview!");
                } else if (strcmp(type, "pointerDown") == 0) {
                    GdkEvent* ev = gdk_event_new(GDK_BUTTON_PRESS);
                    ev->button.window = GDK_WINDOW(g_object_ref(gdk_window));
                    ev->button.x = x; ev->button.y = y;
                    ev->button.button = 1; ev->button.time = GDK_CURRENT_TIME;
                    gdk_event_set_device(ev, pointer);
                    gdk_display_put_event(display, ev);
                    gdk_event_free(ev);
                } else if (strcmp(type, "pointerUp") == 0) {
                    GdkEvent* ev = gdk_event_new(GDK_BUTTON_RELEASE);
                    ev->button.window = GDK_WINDOW(g_object_ref(gdk_window));
                    ev->button.x = x; ev->button.y = y;
                    ev->button.button = 1; ev->button.time = GDK_CURRENT_TIME;
                    gdk_event_set_device(ev, pointer);
                    gdk_display_put_event(display, ev);
                    gdk_event_free(ev);
                } else if (strcmp(type, "pointerMove") == 0 || strcmp(type, "pointerHover") == 0) {
                    GdkEvent* ev = gdk_event_new(GDK_MOTION_NOTIFY);
                    ev->motion.window = GDK_WINDOW(g_object_ref(gdk_window));
                    ev->motion.x = x; ev->motion.y = y;
                    ev->motion.time = GDK_CURRENT_TIME;
                    gdk_event_set_device(ev, pointer);
                    gdk_display_put_event(display, ev);
                    gdk_event_free(ev);
                }
            }
        }
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(true))), nullptr);
    } else if (strcmp(method, "scrollEvent") == 0) {
        if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
            FlValue* xVal = fl_value_lookup_string(args, "x");
            FlValue* yVal = fl_value_lookup_string(args, "y");
            FlValue* dxVal = fl_value_lookup_string(args, "deltaX");
            FlValue* dyVal = fl_value_lookup_string(args, "deltaY");
            if (xVal && yVal && dxVal && dyVal &&
                fl_value_get_type(xVal) == FL_VALUE_TYPE_FLOAT &&
                fl_value_get_type(yVal) == FL_VALUE_TYPE_FLOAT &&
                fl_value_get_type(dxVal) == FL_VALUE_TYPE_FLOAT &&
                fl_value_get_type(dyVal) == FL_VALUE_TYPE_FLOAT) {
                double x = fl_value_get_float(xVal);
                double y = fl_value_get_float(yVal);
                double dx = fl_value_get_float(dxVal);
                double dy = fl_value_get_float(dyVal);
                GdkWindow* gdk_window = gtk_widget_get_window(self->web_view);
                if (gdk_window) {
                    GdkDisplay* display = gdk_display_get_default();
                    GdkSeat* seat = gdk_display_get_default_seat(display);
                    GdkDevice* pointer = gdk_seat_get_pointer(seat);
                    GdkEvent* ev = gdk_event_new(GDK_SCROLL);
                    ev->scroll.window = GDK_WINDOW(g_object_ref(gdk_window));
                    ev->scroll.x = x; ev->scroll.y = y;
                    ev->scroll.direction = GDK_SCROLL_SMOOTH;
                    ev->scroll.delta_x = dx; ev->scroll.delta_y = dy;
                    ev->scroll.time = GDK_CURRENT_TIME;
                    gdk_event_set_device(ev, pointer);
                    gdk_display_put_event(display, ev);
                    gdk_event_free(ev);
                    g_message("[inappwebview] scrollEvent sent: dx=%.0f dy=%.0f", dx, dy);
                }
            }
        }
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(true))), nullptr);
    } else if (strcmp(method, "keyEvent") == 0) {
        if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
            FlValue* typeVal = fl_value_lookup_string(args, "type");
            FlValue* keyvalVal = fl_value_lookup_string(args, "keyval");
            FlValue* charsVal = fl_value_lookup_string(args, "characters");
            if (typeVal && keyvalVal &&
                fl_value_get_type(typeVal) == FL_VALUE_TYPE_STRING &&
                fl_value_get_type(keyvalVal) == FL_VALUE_TYPE_INT) {
                const char* type = fl_value_get_string(typeVal);
                guint keyval = (guint)fl_value_get_int(keyvalVal);
                const char* characters = (charsVal && fl_value_get_type(charsVal) == FL_VALUE_TYPE_STRING)
                    ? fl_value_get_string(charsVal) : "";
                GdkWindow* gdk_window = gtk_widget_get_window(self->web_view);
                g_message("[inappwebview] keyEvent=%s keyval=0x%x chars='%s' window=%p",
                          type, keyval, characters, (void*)gdk_window);
                if (gdk_window) {
                    GdkDisplay* display = gdk_display_get_default();
                    GdkSeat* seat = gdk_display_get_default_seat(display);
                    GdkDevice* keyboard = gdk_seat_get_keyboard(seat);
                    GdkEventType etype = (strcmp(type, "keyup") == 0) ? GDK_KEY_RELEASE : GDK_KEY_PRESS;
                    GdkEvent* ev = gdk_event_new(etype);
                    ev->key.window = GDK_WINDOW(g_object_ref(gdk_window));
                    ev->key.keyval = keyval;
                    ev->key.time = GDK_CURRENT_TIME;
                    if (characters[0] && keyval < 0xFF00) {
                        ev->key.length = strlen(characters);
                        ev->key.string = g_strdup(characters);
                    }
                    gdk_event_set_device(ev, keyboard);
                    gdk_display_put_event(display, ev);
                    if (ev->key.string) g_free(ev->key.string);
                    gdk_event_free(ev);
                }
            }
        }
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(true))), nullptr);
    } else {
        fl_method_call_respond(method_call, FL_METHOD_RESPONSE(fl_method_not_implemented_response_new()), nullptr);
    }
}
