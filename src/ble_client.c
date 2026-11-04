#include "ble_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

/*
 * BlueZ GATT client for the RoboDrive link.
 *
 * Service 0x00FF, characteristic 0xFF01. Commands are write-without-response.
 * Feedback arrives as notifications on the same characteristic. Signal
 * handlers only record state; Connect, discovery, and StartNotify run from
 * ble_client_poll so they are not re-entered from a D-Bus callback.
 */

#define ROBODRIVE_SERVICE_UUID "000000ff-0000-1000-8000-00805f9b34fb"
#define ROBODRIVE_CHAR_UUID    "0000ff01-0000-1000-8000-00805f9b34fb"

#define CONNECT_TIMEOUT_US (15 * G_USEC_PER_SEC)
#define SUBSCRIBE_TIMEOUT_US (5 * G_USEC_PER_SEC)
#define CONNECT_BACKOFF_US (2 * G_USEC_PER_SEC)

enum ble_state
{
    BLE_STATE_IDLE = 0,
    BLE_STATE_SCANNING,
    BLE_STATE_CONNECTING,
    BLE_STATE_READY
};

struct ble_client
{
    GMainContext *ctx;
    gboolean ctx_ready;
    GDBusConnection *conn;
    GDBusProxy *adapter;
    GDBusProxy *device;
    GDBusProxy *characteristic;

    guint sub_added;
    guint sub_removed;
    guint sub_props;

    char *adapter_name;
    char adapter_path[64];
    char *device_name;
    char *address;

    char *device_path;
    char *service_path;
    char *char_path;
    char *last_fail_path;
    gint64 retry_not_before;
    gint64 connect_started_us;
    gint64 services_resolved_us;

    int state;
    gboolean started;
    gboolean rescan;
    gboolean connected;
    gboolean services_resolved;
    gboolean drop;
    gboolean ready;
    gboolean char_write_command;
    gboolean char_write_request;
    gboolean char_notify;
    gboolean char_notifying;
    const char *write_type;

    ble_notify_fn on_notify;
    void *notify_user;

    int rssi;
    gboolean have_rssi;

    char error[256];
};

void ble_client_destroy(ble_client *client);

static void ble_info(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "robojoy: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static void ble_set_error(ble_client *client, const char *fmt, ...)
{
    char next[sizeof client->error];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(next, sizeof next, fmt, ap);
    va_end(ap);
    if (strcmp(next, client->error) == 0)
        return;
    snprintf(client->error, sizeof client->error, "%s", next);
    fprintf(stderr, "robojoy: %s\n", client->error);
}

static gboolean remote_error_is(GError *error, const char *name)
{
    gboolean match = FALSE;
    if (error && g_dbus_error_is_remote_error(error))
    {
        gchar *remote = g_dbus_error_get_remote_error(error);
        match = remote && strcmp(remote, name) == 0;
        g_free(remote);
    }
    return match;
}

static gboolean pump_timeout(gpointer data)
{
    int *fired = data;
    *fired = 1;
    return G_SOURCE_REMOVE;
}

static void ble_pump(GMainContext *ctx, int timeout_ms)
{
    if (timeout_ms <= 0)
    {
        while (g_main_context_pending(ctx))
            g_main_context_iteration(ctx, FALSE);
        return;
    }

    int fired = 0;
    GSource *source = g_timeout_source_new((guint)timeout_ms);
    g_source_set_callback(source, pump_timeout, &fired, NULL);
    g_source_attach(source, ctx);
    while (!fired)
        g_main_context_iteration(ctx, TRUE);
    while (g_main_context_pending(ctx))
        g_main_context_iteration(ctx, FALSE);
    g_source_unref(source);
}

static gboolean proxy_bool(GDBusProxy *proxy, const char *name)
{
    GVariant *value = g_dbus_proxy_get_cached_property(proxy, name);
    gboolean result = FALSE;
    if (value)
    {
        result = g_variant_get_boolean(value);
        g_variant_unref(value);
    }
    return result;
}

static int call_ok(GVariant *result, GError *error, const char *allowed_remote)
{
    if (result)
        g_variant_unref(result);
    if (!error)
        return 0;
    if (allowed_remote && remote_error_is(error, allowed_remote))
    {
        g_error_free(error);
        return 0;
    }
    return -1;
}

static void ble_stop_discovery(ble_client *client)
{
    if (!client->adapter)
        return;

    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_sync(client->adapter, "StopDiscovery", NULL,
                                              G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (result)
        g_variant_unref(result);
    if (error)
        g_error_free(error);
}

static void ble_start_discovery(ble_client *client)
{
    if (!client->adapter)
        return;

    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_sync(client->adapter, "StartDiscovery", NULL,
                                              G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (call_ok(result, error, "org.bluez.Error.InProgress") == 0)
        return;

    ble_set_error(client, "StartDiscovery failed: %s", error->message);
    g_error_free(error);
}

static void ble_clear_gatt(ble_client *client)
{
    g_clear_object(&client->characteristic);
    g_clear_pointer(&client->char_path, g_free);
    g_clear_pointer(&client->service_path, g_free);
    client->char_write_command = FALSE;
    client->char_write_request = FALSE;
    client->char_notify = FALSE;
    client->char_notifying = FALSE;
    client->write_type = NULL;
    client->ready = FALSE;
}

static void ble_enter_scan(ble_client *client)
{
    ble_clear_gatt(client);
    g_clear_object(&client->device);
    g_clear_pointer(&client->device_path, g_free);
    client->connected = FALSE;
    client->services_resolved = FALSE;
    client->services_resolved_us = 0;
    client->have_rssi = FALSE;
    client->rssi = 0;
    client->drop = FALSE;
    client->state = BLE_STATE_SCANNING;
    client->rescan = TRUE;
    ble_info("scanning for %s on %s",
             client->address ? client->address : client->device_name,
             client->adapter_name);
    ble_start_discovery(client);
}

static gboolean path_is_child(const char *path, const char *parent)
{
    size_t length;
    if (!path || !parent)
        return FALSE;
    length = strlen(parent);
    return strncmp(path, parent, length) == 0 && path[length] == '/';
}

static gboolean uuid_is(const char *uuid, const char *want)
{
    return uuid && g_ascii_strcasecmp(uuid, want) == 0;
}

static gboolean device_matches(ble_client *client, const char *path, GVariant *props)
{
    const char *name = NULL;
    const char *alias = NULL;
    const char *address = NULL;

    if (client->last_fail_path &&
        g_get_monotonic_time() < client->retry_not_before &&
        strcmp(path, client->last_fail_path) == 0)
    {
        return FALSE;
    }

    g_variant_lookup(props, "Address", "&s", &address);
    if (client->address)
        return address && g_ascii_strcasecmp(address, client->address) == 0;

    g_variant_lookup(props, "Name", "&s", &name);
    g_variant_lookup(props, "Alias", "&s", &alias);
    if (client->device_name && name && strcmp(name, client->device_name) == 0)
        return TRUE;
    if (client->device_name && alias && strcmp(alias, client->device_name) == 0)
        return TRUE;
    return FALSE;
}

static void note_characteristic(GVariant *props, gboolean *write_command, gboolean *write_request,
                                gboolean *notify, gboolean *notifying)
{
    GVariantIter *flags = NULL;
    const char *flag = NULL;

    *write_command = FALSE;
    *write_request = FALSE;
    *notify = FALSE;
    *notifying = FALSE;

    if (g_variant_lookup(props, "Flags", "as", &flags))
    {
        while (g_variant_iter_next(flags, "&s", &flag))
        {
            if (strcmp(flag, "write-without-response") == 0)
                *write_command = TRUE;
            else if (strcmp(flag, "write") == 0)
                *write_request = TRUE;
            else if (strcmp(flag, "notify") == 0)
                *notify = TRUE;
        }
        g_variant_iter_free(flags);
    }

    g_variant_lookup(props, "Notifying", "b", notifying);
}

static int ble_walk(ble_client *client, gboolean look_for_device, gboolean look_for_gatt)
{
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        client->conn, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
        NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!result)
    {
        ble_set_error(client, "GetManagedObjects failed: %s", error->message);
        g_error_free(error);
        return -1;
    }

    GVariant *objects = g_variant_get_child_value(result, 0);
    GVariantIter iter;
    g_variant_iter_init(&iter, objects);

    char *service_path = NULL;
    char *char_path = NULL;
    char *char_service = NULL;
    gboolean write_command = FALSE;
    gboolean write_request = FALSE;
    gboolean notify = FALSE;
    gboolean notifying = FALSE;

    const char *path = NULL;
    GVariant *ifaces = NULL;
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces))
    {
        GVariant *props;

        if (look_for_device && client->state == BLE_STATE_SCANNING && client->device_path == NULL)
        {
            props = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
            if (props)
            {
                if (device_matches(client, path, props))
                    client->device_path = g_strdup(path);
                g_variant_unref(props);
            }
        }

        if (look_for_gatt && client->device_path && path_is_child(path, client->device_path))
        {
            props = g_variant_lookup_value(ifaces, "org.bluez.GattService1", G_VARIANT_TYPE("a{sv}"));
            if (props)
            {
                const char *uuid = NULL;
                const char *device = NULL;
                g_variant_lookup(props, "UUID", "&s", &uuid);
                g_variant_lookup(props, "Device", "&o", &device);
                if (!service_path && uuid_is(uuid, ROBODRIVE_SERVICE_UUID) &&
                    device && strcmp(device, client->device_path) == 0)
                {
                    service_path = g_strdup(path);
                }
                g_variant_unref(props);
            }

            props = g_variant_lookup_value(ifaces, "org.bluez.GattCharacteristic1", G_VARIANT_TYPE("a{sv}"));
            if (props)
            {
                const char *uuid = NULL;
                const char *service = NULL;
                g_variant_lookup(props, "UUID", "&s", &uuid);
                g_variant_lookup(props, "Service", "&o", &service);
                if (!char_path && uuid_is(uuid, ROBODRIVE_CHAR_UUID) && service)
                {
                    gboolean cmd = FALSE;
                    gboolean req = FALSE;
                    gboolean ntf = FALSE;
                    gboolean ntf_on = FALSE;
                    note_characteristic(props, &cmd, &req, &ntf, &ntf_on);
                    char_path = g_strdup(path);
                    char_service = g_strdup(service);
                    write_command = cmd;
                    write_request = req;
                    notify = ntf;
                    notifying = ntf_on;
                }
                g_variant_unref(props);
            }
        }

        g_variant_unref(ifaces);
        ifaces = NULL;
    }

    if (look_for_gatt && service_path && char_path && char_service &&
        strcmp(char_service, service_path) == 0)
    {
        client->service_path = service_path;
        client->char_path = char_path;
        client->char_write_command = write_command;
        client->char_write_request = write_request;
        client->char_notify = notify;
        client->char_notifying = notifying;
        service_path = NULL;
        char_path = NULL;
    }

    g_free(service_path);
    g_free(char_path);
    g_free(char_service);
    g_variant_unref(objects);
    g_variant_unref(result);
    return 0;
}

static void ble_begin_connect(ble_client *client)
{
    GError *error = NULL;
    GVariant *result;
    char *path = g_strdup(client->device_path);

    client->state = BLE_STATE_CONNECTING;
    client->connect_started_us = g_get_monotonic_time();
    client->services_resolved_us = 0;
    ble_stop_discovery(client);

    ble_info("connecting to %s", path);
    client->device = g_dbus_proxy_new_sync(client->conn, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez",
                                           path, "org.bluez.Device1", NULL, &error);
    if (!client->device)
    {
        ble_set_error(client, "device proxy failed: %s", error->message);
        g_error_free(error);
        g_free(client->last_fail_path);
        client->last_fail_path = path;
        client->retry_not_before = g_get_monotonic_time() + CONNECT_BACKOFF_US;
        ble_enter_scan(client);
        return;
    }

    result = g_dbus_proxy_call_sync(client->device, "Connect", NULL,
                                    G_DBUS_CALL_FLAGS_NONE, 20000, NULL, &error);
    if (call_ok(result, error, "org.bluez.Error.AlreadyConnected") != 0)
    {
        ble_set_error(client, "Connect failed: %s", error->message);
        g_error_free(error);
        g_free(client->last_fail_path);
        client->last_fail_path = path;
        client->retry_not_before = g_get_monotonic_time() + CONNECT_BACKOFF_US;
        ble_enter_scan(client);
        return;
    }

    g_free(path);
    g_free(client->last_fail_path);
    client->last_fail_path = NULL;
    client->retry_not_before = 0;
    client->connected = proxy_bool(client->device, "Connected");
    client->services_resolved = proxy_bool(client->device, "ServicesResolved");
    client->drop = FALSE;
    if (client->services_resolved)
        client->services_resolved_us = g_get_monotonic_time();
}

static void ble_try_subscribe(ble_client *client)
{
    GError *error = NULL;
    GVariant *result;

    if (client->state == BLE_STATE_READY)
        return;

    if (!client->char_path)
    {
        if (ble_walk(client, FALSE, TRUE) != 0)
            return;
    }

    if (!client->char_path)
    {
        if (client->services_resolved_us != 0 &&
            g_get_monotonic_time() - client->services_resolved_us > SUBSCRIBE_TIMEOUT_US)
        {
            ble_set_error(client, "characteristic %s not found", ROBODRIVE_CHAR_UUID);
            ble_enter_scan(client);
        }
        return;
    }

    if (!client->char_notify)
    {
        ble_set_error(client, "characteristic %s does not notify", ROBODRIVE_CHAR_UUID);
        ble_enter_scan(client);
        return;
    }

    if (client->char_write_command)
        client->write_type = "command";
    else if (client->char_write_request)
    {
        client->write_type = "request";
        ble_info("characteristic has write-with-response only");
    }
    else
    {
        client->write_type = "command";
        ble_info("characteristic flags omit write; trying write-without-response");
    }

    client->characteristic = g_dbus_proxy_new_sync(
        client->conn, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez", client->char_path,
        "org.bluez.GattCharacteristic1", NULL, &error);
    if (!client->characteristic)
    {
        ble_set_error(client, "characteristic proxy failed: %s", error->message);
        g_error_free(error);
        ble_enter_scan(client);
        return;
    }

    if (!client->char_notifying)
    {
        result = g_dbus_proxy_call_sync(client->characteristic, "StartNotify", NULL,
                                        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
        if (result)
            g_variant_unref(result);
        if (error)
        {
            ble_set_error(client, "StartNotify failed: %s", error->message);
            g_error_free(error);
            ble_enter_scan(client);
            return;
        }
    }

    client->state = BLE_STATE_READY;
    client->ready = TRUE;
    client->error[0] = '\0';
    ble_info("link ready");
}

static void ble_progress(ble_client *client)
{
    if (!client->started)
        return;

    if (client->drop)
    {
        client->drop = FALSE;
        if (client->state != BLE_STATE_SCANNING)
        {
            ble_info("disconnected");
            ble_enter_scan(client);
        }
        return;
    }

    if (client->state == BLE_STATE_SCANNING)
    {
        if (client->rescan)
        {
            client->rescan = FALSE;
            if (ble_walk(client, TRUE, FALSE) != 0)
                return;
        }
        if (client->device_path)
            ble_begin_connect(client);
        if (client->state == BLE_STATE_CONNECTING && client->services_resolved)
            ble_try_subscribe(client);
        return;
    }

    if (client->state == BLE_STATE_CONNECTING)
    {
        if (client->services_resolved)
        {
            if (client->services_resolved_us == 0)
                client->services_resolved_us = g_get_monotonic_time();
            ble_try_subscribe(client);
        }
        else if (g_get_monotonic_time() - client->connect_started_us > CONNECT_TIMEOUT_US)
        {
            ble_set_error(client, "timed out resolving services");
            ble_enter_scan(client);
        }
        return;
    }

    if (client->state == BLE_STATE_READY && !client->connected)
    {
        ble_info("disconnected");
        ble_enter_scan(client);
    }
}

static void on_interfaces_added(GDBusConnection *connection, const gchar *sender,
                                const gchar *object_path, const gchar *interface_name,
                                const gchar *signal_name, GVariant *parameters, gpointer user_data)
{
    ble_client *client = user_data;
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    (void)parameters;

    if (client->state == BLE_STATE_SCANNING)
        client->rescan = TRUE;
}

static void on_interfaces_removed(GDBusConnection *connection, const gchar *sender,
                                  const gchar *object_path, const gchar *interface_name,
                                  const gchar *signal_name, GVariant *parameters, gpointer user_data)
{
    ble_client *client = user_data;
    const char *path = NULL;
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;

    g_variant_get_child(parameters, 0, "&o", &path);
    if (client->device_path && path && strcmp(path, client->device_path) == 0)
        client->drop = TRUE;
}

static void on_properties_changed(GDBusConnection *connection, const gchar *sender,
                                  const gchar *object_path, const gchar *interface_name,
                                  const gchar *signal_name, GVariant *parameters, gpointer user_data)
{
    ble_client *client = user_data;
    const char *iface = NULL;
    GVariant *changed;
    const char *name = NULL;
    (void)connection;
    (void)sender;
    (void)interface_name;
    (void)signal_name;

    g_variant_get_child(parameters, 0, "&s", &iface);
    changed = g_variant_get_child_value(parameters, 1);

    if (iface && strcmp(iface, "org.bluez.Device1") == 0)
    {
        if (client->state == BLE_STATE_SCANNING &&
            (g_variant_lookup(changed, "Name", "&s", &name) ||
             g_variant_lookup(changed, "Alias", "&s", &name)))
        {
            client->rescan = TRUE;
        }

        if (client->device_path && object_path && strcmp(object_path, client->device_path) == 0)
        {
            gboolean connected = FALSE;
            gboolean resolved = FALSE;
            gint16 rssi = 0;

            if (g_variant_lookup(changed, "Connected", "b", &connected))
            {
                gboolean was_connected = client->connected;
                client->connected = connected;
                if (was_connected && !connected)
                    client->drop = TRUE;
            }
            if (g_variant_lookup(changed, "ServicesResolved", "b", &resolved))
            {
                client->services_resolved = resolved;
                if (resolved && client->services_resolved_us == 0)
                    client->services_resolved_us = g_get_monotonic_time();
            }
            if (g_variant_lookup(changed, "RSSI", "n", &rssi))
            {
                client->rssi = rssi;
                client->have_rssi = TRUE;
            }
        }
    }
    else if (iface && strcmp(iface, "org.bluez.GattCharacteristic1") == 0 &&
             client->char_path && object_path && strcmp(object_path, client->char_path) == 0)
    {
        GVariant *value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE_BYTESTRING);
        if (value)
        {
            gsize length = 0;
            const uint8_t *data = g_variant_get_fixed_array(value, &length, sizeof(uint8_t));
            if (client->on_notify && data)
                client->on_notify(data, (unsigned)length, client->notify_user);
            g_variant_unref(value);
        }
    }

    g_variant_unref(changed);
}

static int ble_setup_adapter(ble_client *client)
{
    GError *error = NULL;
    GVariant *result;
    GVariant *powered;
    gboolean is_powered;

    client->adapter = g_dbus_proxy_new_sync(client->conn, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez",
                                            client->adapter_path, "org.bluez.Adapter1", NULL, &error);
    if (!client->adapter)
    {
        ble_set_error(client, "adapter %s proxy failed: %s", client->adapter_name, error->message);
        g_error_free(error);
        return -1;
    }

    powered = g_dbus_proxy_get_cached_property(client->adapter, "Powered");
    if (!powered)
    {
        ble_set_error(client, "adapter %s not found", client->adapter_name);
        g_clear_object(&client->adapter);
        return -1;
    }

    is_powered = g_variant_get_boolean(powered);
    g_variant_unref(powered);
    if (!is_powered)
    {
        GVariant *value = g_variant_new_boolean(TRUE);
        result = g_dbus_connection_call_sync(
            client->conn, "org.bluez", client->adapter_path, "org.freedesktop.DBus.Properties", "Set",
            g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", value), NULL,
            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
        if (!result)
        {
            ble_set_error(client, "failed to power on %s: %s", client->adapter_name, error->message);
            g_error_free(error);
            return -1;
        }
        g_variant_unref(result);
    }

    {
        GVariantBuilder filter;
        g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&filter, "{sv}", "Transport", g_variant_new_string("le"));
        result = g_dbus_proxy_call_sync(client->adapter, "SetDiscoveryFilter",
                                        g_variant_new("(@a{sv})", g_variant_builder_end(&filter)),
                                        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
        if (result)
            g_variant_unref(result);
        if (error)
        {
            ble_info("SetDiscoveryFilter failed: %s", error->message);
            g_error_free(error);
        }
    }

    return 0;
}

ble_client *ble_client_create(const ble_client_config *cfg)
{
    ble_client *client = calloc(1, sizeof(*client));
    const char *adapter = "hci0";
    if (!client)
        return NULL;

    if (cfg && cfg->adapter && cfg->adapter[0])
        adapter = cfg->adapter;

    client->ctx = g_main_context_new();
    if (!g_main_context_acquire(client->ctx))
    {
        g_main_context_unref(client->ctx);
        free(client);
        return NULL;
    }
    g_main_context_push_thread_default(client->ctx);
    client->ctx_ready = TRUE;

    client->adapter_name = g_strdup(adapter);
    if (snprintf(client->adapter_path, sizeof client->adapter_path, "/org/bluez/%s", client->adapter_name) >=
        (int)sizeof client->adapter_path)
    {
        ble_client_destroy(client);
        return NULL;
    }
    if (cfg && cfg->address && cfg->address[0])
        client->address = g_strdup(cfg->address);
    if (cfg && cfg->device_name && cfg->device_name[0])
        client->device_name = g_strdup(cfg->device_name);
    else if (!client->address)
        client->device_name = g_strdup("ROBODRIVE_2025");

    client->state = BLE_STATE_IDLE;
    return client;
}

void ble_client_destroy(ble_client *client)
{
    if (!client)
        return;

    if (client->adapter)
        ble_stop_discovery(client);

    if (client->device)
    {
        GError *error = NULL;
        GVariant *result = g_dbus_proxy_call_sync(client->device, "Disconnect", NULL,
                                                  G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
        if (result)
            g_variant_unref(result);
        if (error)
            g_error_free(error);
    }

    if (client->conn)
    {
        if (client->sub_added)
            g_dbus_connection_signal_unsubscribe(client->conn, client->sub_added);
        if (client->sub_removed)
            g_dbus_connection_signal_unsubscribe(client->conn, client->sub_removed);
        if (client->sub_props)
            g_dbus_connection_signal_unsubscribe(client->conn, client->sub_props);
    }

    g_clear_object(&client->characteristic);
    g_clear_object(&client->device);
    g_clear_object(&client->adapter);
    g_clear_object(&client->conn);

    g_free(client->adapter_name);
    g_free(client->device_name);
    g_free(client->address);
    g_free(client->device_path);
    g_free(client->service_path);
    g_free(client->char_path);
    g_free(client->last_fail_path);

    if (client->ctx_ready)
    {
        g_main_context_pop_thread_default(client->ctx);
        g_main_context_release(client->ctx);
        client->ctx_ready = FALSE;
    }
    if (client->ctx)
        g_main_context_unref(client->ctx);
    free(client);
}

int ble_client_start(ble_client *client)
{
    GError *error = NULL;

    if (!client)
        return -1;
    if (client->started)
        return 0;

    if (!client->conn)
    {
        client->conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (!client->conn)
        {
            ble_set_error(client, "system bus failed: %s", error->message);
            g_error_free(error);
            return -1;
        }

        client->sub_added = g_dbus_connection_signal_subscribe(
            client->conn, "org.bluez", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
            NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_interfaces_added, client, NULL);
        client->sub_removed = g_dbus_connection_signal_subscribe(
            client->conn, "org.bluez", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
            NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_interfaces_removed, client, NULL);
        client->sub_props = g_dbus_connection_signal_subscribe(
            client->conn, "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged",
            NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_properties_changed, client, NULL);
    }

    if (ble_setup_adapter(client) != 0)
        return -1;

    client->started = TRUE;
    ble_enter_scan(client);
    return 0;
}

int ble_client_poll(ble_client *client, int timeout_ms)
{
    if (!client || !client->ctx)
        return -1;
    ble_pump(client->ctx, timeout_ms);
    ble_progress(client);
    return 0;
}

bool ble_client_ready(const ble_client *client)
{
    return client && client->ready && client->state == BLE_STATE_READY;
}

int ble_client_write(ble_client *client, const uint8_t *data, unsigned len)
{
    GError *error = NULL;
    GVariant *bytes;
    GVariantBuilder options;
    GVariant *result;
    const char *type;

    if (!client || !data || len == 0 || !ble_client_ready(client) || !client->characteristic)
        return -1;

    type = client->write_type ? client->write_type : "command";
    bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, len, sizeof(uint8_t));
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "type", g_variant_new_string(type));

    result = g_dbus_proxy_call_sync(
        client->characteristic, "WriteValue",
        g_variant_new("(@ay@a{sv})", bytes, g_variant_builder_end(&options)),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (result)
        g_variant_unref(result);
    if (!error)
        return 0;

    if (strcmp(type, "command") == 0 && client->char_write_request)
    {
        ble_info("write-without-response failed (%s); trying write-with-response", error->message);
        g_error_free(error);
        error = NULL;
        client->write_type = "request";

        bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, len, sizeof(uint8_t));
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&options, "{sv}", "type", g_variant_new_string("request"));
        result = g_dbus_proxy_call_sync(
            client->characteristic, "WriteValue",
            g_variant_new("(@ay@a{sv})", bytes, g_variant_builder_end(&options)),
            G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
        if (result)
            g_variant_unref(result);
        if (!error)
            return 0;
    }

    ble_set_error(client, "write failed: %s", error->message);
    g_error_free(error);
    client->ready = FALSE;
    return -1;
}

void ble_client_set_notify_handler(ble_client *client, ble_notify_fn fn, void *user)
{
    if (!client)
        return;
    client->on_notify = fn;
    client->notify_user = user;
}

void ble_client_reconnect(ble_client *client)
{
    GError *error = NULL;
    GVariant *result;

    if (!client || !client->started || client->state == BLE_STATE_SCANNING)
        return;

    client->ready = FALSE;
    ble_clear_gatt(client);
    client->services_resolved = FALSE;
    client->services_resolved_us = 0;

    if (!client->device)
    {
        ble_enter_scan(client);
        return;
    }

    result = g_dbus_proxy_call_sync(client->device, "Disconnect", NULL,
                                    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (result)
        g_variant_unref(result);
    if (error)
        g_error_free(error);

    client->connected = FALSE;
    client->state = BLE_STATE_CONNECTING;
    client->connect_started_us = g_get_monotonic_time();

    error = NULL;
    result = g_dbus_proxy_call_sync(client->device, "Connect", NULL,
                                    G_DBUS_CALL_FLAGS_NONE, 20000, NULL, &error);
    if (call_ok(result, error, "org.bluez.Error.AlreadyConnected") != 0)
    {
        ble_set_error(client, "reconnect failed: %s", error->message);
        g_error_free(error);
        ble_enter_scan(client);
        return;
    }

    client->connected = proxy_bool(client->device, "Connected");
    client->services_resolved = proxy_bool(client->device, "ServicesResolved");
    client->drop = FALSE;
    if (client->services_resolved)
        ble_try_subscribe(client);
}

int ble_client_rssi(const ble_client *client)
{
    if (!client || !client->have_rssi)
        return 0;
    return client->rssi;
}

const char *ble_client_last_error(const ble_client *client)
{
    if (!client)
        return "";
    return client->error;
}
