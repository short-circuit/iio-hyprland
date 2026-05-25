#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dbus/dbus.h>

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
    dbus_connection_flush(connection);
    dbus_connection_close(connection);
    dbus_connection_unref(connection);
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
    isRotationUnlocked ^= 1;
}

// Ensure hyprland.lua has require("custom.touch") — one-time setup
void setup_touch_require() {
    const char* home = getenv("HOME");
    if (!home) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/hypr/hyprland.lua", home);

    FILE* f = fopen(path, "r");
    if (!f) return;

    char* content = NULL;
    size_t len = 0;
    long pos;
    while ((pos = ftell(f)) >= 0) {
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

    if (!content) return;

    if (strstr(content, "custom.touch")) {
        free(content);
        return;
    }

    f = fopen(path, "a");
    if (f) {
        fprintf(f, "\n-- Touch rotation (managed by iio-hyprland)\nrequire(\"custom.touch\")\n");
        fclose(f);
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

void rotate_display_and_touch(int transform) {
    write_touch_lua(transform);

    // Reload config to trigger input device callbacks
    system("hyprctl reload config-only 2>/dev/null");

    // Re-apply display rotation (lost during reload)
    system_fmt("hyprctl eval \"hl.monitor({ output = \\\"%s\\\", mode = \\\"preferred\\\", position = \\\"auto\\\", scale = \\\"1\\\", transform = %d })\"", output, transform);
}

void handle_orientation(enum Orientation orientation, const char* monitor_id) {
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

void init_orientation(DBusConnection* conn, const char* monitor_id) {
    DBusMessage* reply = request_orientation(conn);
    if (reply != NULL) {
        handle_orientation(parse_orientation_reply(reply), monitor_id);
        dbus_message_unref(reply);
    }
}

void listen_orientation(DBusConnection* connection, const char* monitor_id) {
    DBusMessage* msg;
    dbus_bus_add_match(connection,
        "type='signal',interface='org.freedesktop.DBus.Properties'", &error);
    dbus_bus_add_match(connection,
        "type='signal',sender='org.freedesktop.DBus',interface='org."
        "freedesktop.DBus',member='NameOwnerChanged',arg0='net.hadess."
        "SensorProxy'",
        &error);
    dbus_connection_flush(connection);
    while (dbus_connection_read_write_dispatch(connection, -1)) {
        msg = dbus_connection_pop_message(connection);
        if (msg != NULL) {
            if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties",
                    "PropertiesChanged")) {
                if (parse_orientation_signal(msg) != Undefined) {
                    usleep(1000 * 300);
                    dbus_connection_flush(connection);
                    init_orientation(connection, monitor_id);
                }
            } else {
                dbus_message_unref(msg);
                break;
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

char* get_monitor_id(const char* monitor_name) {
    char command[256];
    snprintf(command, sizeof(command),
        "hyprctl monitors -j all | jq -r '.[] | select(.name==\"%s\") | .id'",
        monitor_name);
    FILE* fp = popen(command, "r");
    if (fp == NULL) {
        perror("popen");
        return NULL;
    }

    static char monitor_id[16];
    if (fgets(monitor_id, sizeof(monitor_id), fp) == NULL) {
        perror("fgets");
        pclose(fp);
        return NULL;
    }

    pclose(fp);
    monitor_id[strcspn(monitor_id, "\n")] = '\0';
    return monitor_id;
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

    // One-time setup: ensure hyprland.lua loads custom/touch.lua
    setup_touch_require();

    char* monitor_id = get_monitor_id(output);
    if (monitor_id == NULL) {
        printf("error: cannot get monitor ID for %s\n", output);
        dbus_disconnect(connection);
        return 1;
    }

    init_orientation(connection, monitor_id);
    listen_orientation(connection, monitor_id);

    dbus_disconnect(connection);
    return 0;
}
