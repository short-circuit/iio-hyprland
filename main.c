#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dbus/dbus.h>

static volatile int rotation_toggle_requested = 0;

// corresponds to hyprctl orientation integers
enum Orientation { Normal, LeftUp, BottomUp, RightUp, Undefined};

DBusError error;
char* output = "eDP-1"; // Default output device
int rotate_master_layout = 0; // Default layout
int orientation_map[4] = {0,1,2,3};
char flip_bottom_up = 0; //Default orientation is not flipped
char isRotationUnlocked = 1; //Default rotation is unlocked
enum Orientation last_handled_orientation = Undefined;

void dbus_disconnect(DBusConnection* connection) {
    if (connection) {
        dbus_connection_flush(connection);
        dbus_connection_unref(connection);
    }
    dbus_error_free(&error);
}

DBusConnection* dbus_connect(void) {
    dbus_error_init(&error);
    DBusConnection* connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (connection != NULL) {
        DBusMessage* msg = dbus_message_new_method_call(
            "net.hadess.SensorProxy", "/net/hadess/SensorProxy",
            "net.hadess.SensorProxy", "ClaimAccelerometer");
        char ok = dbus_connection_send_with_reply_and_block(
                      connection, msg, DBUS_TIMEOUT_INFINITE, &error) != NULL;
        dbus_message_unref(msg);
        if (ok) {
            return connection;
        }
    }
    return NULL;
}

enum Orientation property_to_enum(const char* orientation) {
    if (!strcmp(orientation, "normal")) {
        return flip_bottom_up?BottomUp:Normal;
    }
    if (!strcmp(orientation, "bottom-up")) {
        return flip_bottom_up?Normal:BottomUp;
    }
    if (!strcmp(orientation, "left-up")) {
        return LeftUp;
    }
    if (!strcmp(orientation, "right-up")) {
        return RightUp;
    }
    return Undefined;
}

enum Orientation parse_orientation_signal(DBusMessage* msg) {
    DBusMessageIter args, iter_array, iter_dict, iter_v;
    const char *iface, *property, *orientation;
    if (dbus_message_iter_init(msg, &args)) {
        dbus_message_iter_get_basic(&args, &iface);
        if (!strcmp("net.hadess.SensorProxy", iface)) {
            dbus_message_iter_next(&args);
            dbus_message_iter_recurse(&args, &iter_array);
            dbus_message_iter_recurse(&iter_array, &iter_dict);
            dbus_message_iter_get_basic(&iter_dict, &property);
            if (!strcmp(property, "AccelerometerOrientation")) {
                dbus_message_iter_next(&iter_dict);
                dbus_message_iter_recurse(&iter_dict, &iter_v);
                dbus_message_iter_get_basic(&iter_v, &orientation);
                return property_to_enum(orientation);
            }
        }
    }
    return Undefined;
}

void system_fmt(char* format, ...) {
    char command[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(command, sizeof(command), format, args);
    system(command);
    va_end(args);
}

void handle_lock_rotation(int sig){
    (void)sig;
    rotation_toggle_requested = 1;
}

void write_touch_lua(int transform);

// Ensure custom/touch.lua exists and hyprland.lua loads it
void setup_touch_require() {
    // Create custom/touch.lua FIRST so it exists before any config reload
    write_touch_lua(0);

    const char* home = getenv("HOME");
    if (!home) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/hypr/hyprland.lua", home);

    FILE* f = fopen(path, "r");
    if (!f) return;

    char* content = NULL;
    size_t len = 0;
    while (1) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        char* new = realloc(content, len + n + 1);
        if (!new) break;
        content = new;
        memcpy(content + len, buf, n);
        len += n;
    }
    if (content) content[len] = '\0';
    fclose(f);

    if (content && !strstr(content, "custom.touch")) {
        f = fopen(path, "a");
        if (f) {
            fprintf(f, "\n-- Touch rotation (managed by iio-hyprland)\nrequire(\"custom.touch\")\n");
            fclose(f);
        }
    }
    free(content);
}

// Write the current touch/tablet transform to custom/touch.lua
void write_touch_lua(int transform) {
    const char* home = getenv("HOME");
    if (!home) return;

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/hypr/custom", home);
    mkdir(dir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/hypr/custom/touch.lua", home);

    FILE* f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "-- Managed by iio-hyprland\n");
    fprintf(f, "hl.config({input = {touchdevice = {transform = %d}, tablet = {transform = %d, output = \"%s\"}}})\n",
            transform, transform, output);
    fclose(f);
}

void write_toggle_state() {
    const char* home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/hypr/rotation-toggle", home);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", isRotationUnlocked);
        fclose(f);
    }
}

void send_toggle_notification() {
    if (isRotationUnlocked)
        system("notify-send -a iio-hyprland \"Auto-rotation enabled\" 2>/dev/null");
    else
        system("notify-send -a iio-hyprland \"Auto-rotation disabled\" 2>/dev/null");
}

void rotate_display_and_touch(int transform) {
    write_touch_lua(transform);

    int uid = getuid();
    system_fmt(
        "sig=$(ls -1t /run/user/%d/hypr 2>/dev/null | head -1) && "
        "export HYPRLAND_INSTANCE_SIGNATURE=$sig XDG_RUNTIME_DIR=/run/user/%d && "
        "hyprctl reload config-only 2>/dev/null && "
        "hyprctl eval \"hl.monitor({ output = \\\"%s\\\", mode = \\\"preferred\\\", position = \\\"auto\\\", scale = \\\"1\\\", transform = %d })\"",
        uid, uid, output, transform);
}

void handle_orientation(enum Orientation orientation, const char* monitor_id) {
    (void)monitor_id;
    if (orientation == Undefined || orientation == last_handled_orientation || !isRotationUnlocked)
        return;
    int transform = orientation_map[orientation];

    rotate_display_and_touch(transform);

    // Handle master workspace layout if requested
    if (rotate_master_layout == 1) {
        const char* dir = (orientation == Normal || orientation == BottomUp) ? "left" : "top";
        system_fmt("hyprctl eval \"hl.config({ master = { orientation = \\\"%s\\\" } })\"", dir);
    } else if (rotate_master_layout == 2) {
        const char* dir = (orientation == Normal || orientation == BottomUp) ? "right" : "bottom";
        system_fmt("hyprctl eval \"hl.config({ master = { orientation = \\\"%s\\\" } })\"", dir);
    }

    last_handled_orientation = orientation;
}

DBusMessage* request_orientation(DBusConnection* conn) {
    DBusMessage* req = dbus_message_new_method_call(
        "net.hadess.SensorProxy",
        "/net/hadess/SensorProxy",
        "org.freedesktop.DBus.Properties",
        "Get"
    );

    const char* interface_name = "net.hadess.SensorProxy";
    const char* property_name = "AccelerometerOrientation";
    dbus_message_append_args(req,
        DBUS_TYPE_STRING, &interface_name,
        DBUS_TYPE_STRING, &property_name,
        DBUS_TYPE_INVALID
    );

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, req, DBUS_TIMEOUT_INFINITE, &error);

    if (dbus_error_is_set(&error)) {
        printf("Error receiving orientation request: %s: %s\n",
               error.name, error.message);
    }

    dbus_message_unref(req);
    return reply;
}

enum Orientation parse_orientation_reply(DBusMessage* reply) {
    DBusMessageIter iter, sub_iter;
    const char* orientation;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &sub_iter);
    dbus_message_iter_get_basic(&sub_iter, &orientation);
    return property_to_enum(orientation);
}

void init_orientation(DBusConnection* conn) {
    DBusMessage* reply = request_orientation(conn);
    if (reply != NULL) {
        handle_orientation(parse_orientation_reply(reply), NULL);
        dbus_message_unref(reply);
    }
}

void listen_orientation(DBusConnection* connection) {
    DBusMessage* msg;
    dbus_bus_add_match(connection,
        "type='signal',interface='org.freedesktop.DBus.Properties'", &error);
    dbus_bus_add_match(connection,
        "type='signal',sender='org.freedesktop.DBus',interface='org."
        "freedesktop.DBus',member='NameOwnerChanged',arg0='net.hadess."
        "SensorProxy'",
        &error);
    dbus_connection_flush(connection);
    while (dbus_connection_read_write_dispatch(connection, 500)) {
        // Handle rotation toggle request from SIGUSR1
        if (rotation_toggle_requested) {
            rotation_toggle_requested = 0;
            isRotationUnlocked = !isRotationUnlocked;
            write_toggle_state();
            send_toggle_notification();
        }

        msg = dbus_connection_pop_message(connection);
        if (msg != NULL) {
            if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties",
                    "PropertiesChanged")) {
                if (parse_orientation_signal(msg) != Undefined) {
                    usleep(1000 * 300);
                    dbus_connection_flush(connection);
                    init_orientation(connection);
                }
            } else {
                dbus_message_unref(msg);
                continue;
            }
            dbus_message_unref(msg);
        }
    }
}

void parse_transform(char* transform_str) {
    orientation_map[0] = transform_str[0] - '0';
    orientation_map[1] = transform_str[2] - '0';
    orientation_map[2] = transform_str[4] - '0';
    orientation_map[3] = transform_str[6] - '0';
}

int main(int argc, char* argv[]) {
    DBusConnection* connection = dbus_connect();
    if (connection == NULL) {
        printf("error: cannot open dbus connection\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--left-master") == 0) {
            rotate_master_layout = 1;
        }
        else if (strcmp(argv[i], "--right-master") == 0) {
            rotate_master_layout = 2;
        }
        else if (strcmp(argv[i], "--flip-bottom-up") == 0) {
            flip_bottom_up = 1;
        }
        else if (strcmp(argv[i], "--transform") == 0) {
            parse_transform(argv[++i]);
        }
        else {
            output = argv[i];
        }
    }

    signal(SIGUSR1, handle_lock_rotation);

    // Initialise toggle state file
    write_toggle_state();

    // Clean up stale Hyprland instances
    int uid = getuid();
    system_fmt(
        "cd /run/user/%d/hypr 2>/dev/null && "
        "latest=$(ls -1t | head -1) && "
        "for d in */; do test \"${d%/}\" != \"$latest\" && rm -rf \"$d\"; done 2>/dev/null",
        uid);

    // One-time setup: ensure hyprland.lua loads custom/touch.lua
    setup_touch_require();

    init_orientation(connection);
    listen_orientation(connection);

    dbus_disconnect(connection);
    return 0;
}
