/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2007-2009  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <gdbus.h>

#include "connman.h"

struct connman_device {
	struct connman_element element;
	enum connman_device_type type;
	enum connman_device_mode mode;
	enum connman_device_policy policy;
	connman_bool_t powered;
	connman_bool_t carrier;
	connman_bool_t scanning;
	connman_bool_t disconnected;
	connman_uint8_t priority;
	char *name;
	char *node;
	char *interface;
	unsigned int connections;

	struct connman_device_driver *driver;
	void *driver_data;

	connman_bool_t registered;

	char *last_network;
	struct connman_network *network;
	GHashTable *networks;
};

static const char *type2description(enum connman_device_type type)
{
	switch (type) {
	case CONNMAN_DEVICE_TYPE_UNKNOWN:
	case CONNMAN_DEVICE_TYPE_VENDOR:
		break;
	case CONNMAN_DEVICE_TYPE_ETHERNET:
		return "Ethernet";
	case CONNMAN_DEVICE_TYPE_WIFI:
		return "Wireless";
	case CONNMAN_DEVICE_TYPE_WIMAX:
		return "WiMAX";
	case CONNMAN_DEVICE_TYPE_BLUETOOTH:
		return "Bluetooth";
	case CONNMAN_DEVICE_TYPE_HSO:
	case CONNMAN_DEVICE_TYPE_NOZOMI:
	case CONNMAN_DEVICE_TYPE_HUAWEI:
	case CONNMAN_DEVICE_TYPE_NOVATEL:
		return "Cellular";
	}

	return NULL;
}

static const char *type2string(enum connman_device_type type)
{
	switch (type) {
	case CONNMAN_DEVICE_TYPE_UNKNOWN:
	case CONNMAN_DEVICE_TYPE_VENDOR:
		break;
	case CONNMAN_DEVICE_TYPE_ETHERNET:
		return "ethernet";
	case CONNMAN_DEVICE_TYPE_WIFI:
		return "wifi";
	case CONNMAN_DEVICE_TYPE_WIMAX:
		return "wimax";
	case CONNMAN_DEVICE_TYPE_BLUETOOTH:
		return "bluetooth";
	case CONNMAN_DEVICE_TYPE_HSO:
	case CONNMAN_DEVICE_TYPE_HUAWEI:
	case CONNMAN_DEVICE_TYPE_NOZOMI:
	case CONNMAN_DEVICE_TYPE_NOVATEL:
		return "cellular";
	}

	return NULL;
}

static const char *policy2string(enum connman_device_policy policy)
{
	switch (policy) {
	case CONNMAN_DEVICE_POLICY_UNKNOWN:
		break;
	case CONNMAN_DEVICE_POLICY_IGNORE:
		return "ignore";
	case CONNMAN_DEVICE_POLICY_OFF:
		return "off";
	case CONNMAN_DEVICE_POLICY_AUTO:
		return "auto";
	case CONNMAN_DEVICE_POLICY_MANUAL:
		return "manual";
	}

	return NULL;
}

static enum connman_device_policy string2policy(const char *policy)
{
	if (g_str_equal(policy, "ignore") == TRUE)
		return CONNMAN_DEVICE_POLICY_IGNORE;
	else if (g_str_equal(policy, "off") == TRUE)
		return CONNMAN_DEVICE_POLICY_OFF;
	else if (g_str_equal(policy, "auto") == TRUE)
		return CONNMAN_DEVICE_POLICY_AUTO;
	else if (g_str_equal(policy, "manual") == TRUE)
		return CONNMAN_DEVICE_POLICY_MANUAL;
	else
		return CONNMAN_DEVICE_POLICY_UNKNOWN;
}

static int set_powered(struct connman_device *device, connman_bool_t powered)
{
	struct connman_device_driver *driver = device->driver;
	int err;

	DBG("device %p powered %d", device, powered);

	if (!driver)
		return -EINVAL;

	if (powered == TRUE) {
		if (driver->enable)
			err = driver->enable(device);
		else
			err = -EINVAL;
	} else {
		g_hash_table_remove_all(device->networks);

		if (driver->disable)
			err = driver->disable(device);
		else
			err = -EINVAL;
	}

	return err;
}

static int set_policy(DBusConnection *connection,
				struct connman_device *device,
					enum connman_device_policy policy)
{
	DBusMessage *signal;
	DBusMessageIter entry, value;
	const char *str, *key = "Policy";
	int err = 0;

	DBG("device %p policy %d", device, policy);

	if (device->policy == policy)
		return 0;

	switch (policy) {
	case CONNMAN_DEVICE_POLICY_UNKNOWN:
		return -EINVAL;
	case CONNMAN_DEVICE_POLICY_IGNORE:
		break;
	case CONNMAN_DEVICE_POLICY_OFF:
		if (device->powered == TRUE)
			err = set_powered(device, FALSE);
		break;
	case CONNMAN_DEVICE_POLICY_AUTO:
	case CONNMAN_DEVICE_POLICY_MANUAL:
		if (device->powered == FALSE)
			err = set_powered(device, TRUE);
		break;
	}

	if (err < 0)
		return err;

	device->policy = policy;

	signal = dbus_message_new_signal(device->element.path,
				CONNMAN_DEVICE_INTERFACE, "PropertyChanged");
	if (signal == NULL)
		return 0;

	dbus_message_iter_init_append(signal, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	str = policy2string(policy);

	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					DBUS_TYPE_STRING_AS_STRING, &value);
	dbus_message_iter_append_basic(&value, DBUS_TYPE_STRING, &str);
	dbus_message_iter_close_container(&entry, &value);

	g_dbus_send_message(connection, signal);

	return 0;
}

static void append_networks(struct connman_device *device,
						DBusMessageIter *entry)
{
	DBusMessageIter value, iter;
	const char *key = "Networks";

	dbus_message_iter_append_basic(entry, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(entry, DBUS_TYPE_VARIANT,
		DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_OBJECT_PATH_AS_STRING,
								&value);

	dbus_message_iter_open_container(&value, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH_AS_STRING, &iter);
	__connman_element_list((struct connman_element *) device,
					CONNMAN_ELEMENT_TYPE_NETWORK, &iter);
	dbus_message_iter_close_container(&value, &iter);

	dbus_message_iter_close_container(entry, &value);
}

static DBusMessage *get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct connman_device *device = data;
	DBusMessage *reply;
	DBusMessageIter array, dict, entry;
	const char *str;

	DBG("conn %p", conn);

	if (__connman_security_check_privilege(msg,
					CONNMAN_SECURITY_PRIVILEGE_PUBLIC) < 0)
		return __connman_error_permission_denied(msg);

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &array);

	dbus_message_iter_open_container(&array, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	str = type2description(device->type);
	if (str != NULL && device->interface != NULL) {
		char *name = g_strdup_printf("%s (%s)", str, device->interface);
		if (name != NULL)
			connman_dbus_dict_append_variant(&dict, "Name",
						DBUS_TYPE_STRING, &name);
		g_free(name);
	}

	str = type2string(device->type);
	if (str != NULL)
		connman_dbus_dict_append_variant(&dict, "Type",
						DBUS_TYPE_STRING, &str);

	if (device->interface != NULL)
		connman_dbus_dict_append_variant(&dict, "Interface",
					DBUS_TYPE_STRING, &device->interface);

	str = policy2string(device->policy);
	if (str != NULL)
		connman_dbus_dict_append_variant(&dict, "Policy",
						DBUS_TYPE_STRING, &str);

	if (device->priority > 0)
		connman_dbus_dict_append_variant(&dict, "Priority",
					DBUS_TYPE_BYTE, &device->priority);

	connman_dbus_dict_append_variant(&dict, "Powered",
					DBUS_TYPE_BOOLEAN, &device->powered);

	if (device->driver && device->driver->scan)
		connman_dbus_dict_append_variant(&dict, "Scanning",
					DBUS_TYPE_BOOLEAN, &device->scanning);

	switch (device->mode) {
	case CONNMAN_DEVICE_MODE_UNKNOWN:
		break;
	case CONNMAN_DEVICE_MODE_TRANSPORT_IP:
		__connman_element_append_ipv4(&device->element, &dict);
		break;
	case CONNMAN_DEVICE_MODE_NETWORK_SINGLE:
	case CONNMAN_DEVICE_MODE_NETWORK_MULTIPLE:
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY,
								NULL, &entry);
		append_networks(device, &entry);
		dbus_message_iter_close_container(&dict, &entry);
		break;
	}

	dbus_message_iter_close_container(&array, &dict);

	return reply;
}

static DBusMessage *set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct connman_device *device = data;
	DBusMessageIter iter, value;
	const char *name;
	int type;

	DBG("conn %p", conn);

	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return __connman_error_invalid_arguments(msg);

	dbus_message_iter_get_basic(&iter, &name);
	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &value);

	if (__connman_security_check_privilege(msg,
					CONNMAN_SECURITY_PRIVILEGE_MODIFY) < 0)
		return __connman_error_permission_denied(msg);

	type = dbus_message_iter_get_arg_type(&value);

	if (g_str_equal(name, "Powered") == TRUE) {
		connman_bool_t powered;
		int err;

		if (type != DBUS_TYPE_BOOLEAN)
			return __connman_error_invalid_arguments(msg);

		dbus_message_iter_get_basic(&value, &powered);

		if (device->powered == powered)
			return __connman_error_invalid_arguments(msg);

		err = set_powered(device, powered);
		if (err < 0 && err != -EINPROGRESS)
			return __connman_error_failed(msg);
	} else if (g_str_equal(name, "Policy") == TRUE) {
		enum connman_device_policy policy;
		const char *str;
		int err;

		if (type != DBUS_TYPE_STRING)
			return __connman_error_invalid_arguments(msg);

		dbus_message_iter_get_basic(&value, &str);
		policy = string2policy(str);
		if (policy == CONNMAN_DEVICE_POLICY_UNKNOWN)
			return __connman_error_invalid_arguments(msg);

		err = set_policy(conn, device, policy);
		if (err < 0)
			return __connman_error_failed(msg);
	} else if (g_str_equal(name, "Priority") == TRUE) {
		connman_uint8_t priority;

		if (type != DBUS_TYPE_BYTE)
			return __connman_error_invalid_arguments(msg);

		dbus_message_iter_get_basic(&value, &priority);

		device->priority = priority;
	} else if (g_str_has_prefix(name, "IPv4") == TRUE) {
		switch (device->mode) {
		case CONNMAN_DEVICE_MODE_UNKNOWN:
		case CONNMAN_DEVICE_MODE_NETWORK_SINGLE:
		case CONNMAN_DEVICE_MODE_NETWORK_MULTIPLE:
			return __connman_error_invalid_arguments(msg);
		case CONNMAN_DEVICE_MODE_TRANSPORT_IP:
			__connman_element_set_ipv4(&device->element,
								name, &value);
			break;
		}
	}

	__connman_storage_save_device(device);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *create_network(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBG("conn %p", conn);

	if (__connman_security_check_privilege(msg,
					CONNMAN_SECURITY_PRIVILEGE_MODIFY) < 0)
		return __connman_error_permission_denied(msg);

	return __connman_error_invalid_arguments(msg);
}

static DBusMessage *remove_network(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBG("conn %p", conn);

	if (__connman_security_check_privilege(msg,
					CONNMAN_SECURITY_PRIVILEGE_MODIFY) < 0)
		return __connman_error_permission_denied(msg);

	return __connman_error_invalid_arguments(msg);
}

static DBusMessage *propose_scan(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct connman_device *device = data;
	int err;

	DBG("conn %p", conn);

	switch (device->mode) {
	case CONNMAN_DEVICE_MODE_UNKNOWN:
	case CONNMAN_DEVICE_MODE_TRANSPORT_IP:
		return __connman_error_not_supported(msg);
	case CONNMAN_DEVICE_MODE_NETWORK_SINGLE:
	case CONNMAN_DEVICE_MODE_NETWORK_MULTIPLE:
		break;
	}

	if (!device->driver || !device->driver->scan)
		return __connman_error_not_supported(msg);

	if (device->powered == FALSE)
		return __connman_error_failed(msg);

	err = device->driver->scan(device);
	if (err < 0)
		return __connman_error_failed(msg);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static GDBusMethodTable device_methods[] = {
	{ "GetProperties", "",      "a{sv}", get_properties },
	{ "SetProperty",   "sv",    "",      set_property   },
	{ "CreateNetwork", "a{sv}", "o",     create_network },
	{ "RemoveNetwork", "o",     "",      remove_network },
	{ "ProposeScan",   "",      "",      propose_scan   },
	{ },
};

static GDBusSignalTable device_signals[] = {
	{ "PropertyChanged", "sv" },
	{ },
};

static DBusConnection *connection;

static void append_devices(DBusMessageIter *entry)
{
	DBusMessageIter value, iter;
	const char *key = "Devices";

	dbus_message_iter_append_basic(entry, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(entry, DBUS_TYPE_VARIANT,
		DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_OBJECT_PATH_AS_STRING,
								&value);

	dbus_message_iter_open_container(&value, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH_AS_STRING, &iter);
	__connman_element_list(NULL, CONNMAN_ELEMENT_TYPE_DEVICE, &iter);
	dbus_message_iter_close_container(&value, &iter);

	dbus_message_iter_close_container(entry, &value);
}

static void emit_devices_signal(void)
{
	DBusMessage *signal;
	DBusMessageIter entry;

	signal = dbus_message_new_signal(CONNMAN_MANAGER_PATH,
				CONNMAN_MANAGER_INTERFACE, "PropertyChanged");
	if (signal == NULL)
		return;

	dbus_message_iter_init_append(signal, &entry);

	append_devices(&entry);

	g_dbus_send_message(connection, signal);
}

static int register_interface(struct connman_element *element)
{
	struct connman_device *device = element->device;

	DBG("element %p name %s", element, element->name);

	if (g_dbus_register_interface(connection, element->path,
					CONNMAN_DEVICE_INTERFACE,
					device_methods, device_signals,
					NULL, device, NULL) == FALSE) {
		connman_error("Failed to register %s device", element->path);
		return -EIO;
	}

	device->registered = TRUE;

	emit_devices_signal();

	return 0;
}

static void unregister_interface(struct connman_element *element)
{
	struct connman_device *device = element->device;

	DBG("element %p name %s", element, element->name);

	device->registered = FALSE;

	emit_devices_signal();

	g_dbus_unregister_interface(connection, element->path,
						CONNMAN_DEVICE_INTERFACE);
}

static void device_enable(struct connman_device *device)
{
	DBG("device %p", device);

	if (device->policy == CONNMAN_DEVICE_POLICY_IGNORE ||
				device->policy == CONNMAN_DEVICE_POLICY_OFF)
		return;

	if (device->powered == TRUE)
		return;

	if (device->driver->enable)
		device->driver->enable(device);
}

static void device_disable(struct connman_device *device)
{
	DBG("device %p", device);

	if (device->policy == CONNMAN_DEVICE_POLICY_IGNORE)
		return;

	if (device->powered == FALSE)
		return;

	g_hash_table_remove_all(device->networks);

	if (device->driver->disable)
		device->driver->disable(device);
}

static int setup_device(struct connman_device *device)
{
	int err;

	DBG("device %p", device);

	err = register_interface(&device->element);
	if (err < 0) {
		if (device->driver->remove)
			device->driver->remove(device);
		device->driver = NULL;
		return err;
	}

	device_enable(device);

	return 0;
}

static void probe_driver(struct connman_element *element, gpointer user_data)
{
	struct connman_device_driver *driver = user_data;

	DBG("element %p name %s", element, element->name);

	if (element->device == NULL)
		return;

	if (element->device->driver != NULL)
		return;

	if (driver->probe(element->device) < 0)
		return;

	element->device->driver = driver;

	setup_device(element->device);
}

static void remove_device(struct connman_device *device)
{
	DBG("device %p", device);

	device_disable(device);

	unregister_interface(&device->element);

	if (device->driver->remove)
		device->driver->remove(device);

	device->driver = NULL;
}

static void remove_driver(struct connman_element *element, gpointer user_data)
{
	struct connman_device_driver *driver = user_data;

	DBG("element %p name %s", element, element->name);

	if (element->device == NULL)
		return;

	if (element->device->driver == driver)
		remove_device(element->device);
}

connman_bool_t __connman_device_has_driver(struct connman_device *device)
{
	if (device == NULL || device->driver == NULL)
		return FALSE;

	return device->registered;
}

static GSList *driver_list = NULL;

static gint compare_priority(gconstpointer a, gconstpointer b)
{
	const struct connman_device_driver *driver1 = a;
	const struct connman_device_driver *driver2 = b;

	return driver2->priority - driver1->priority;
}

/**
 * connman_device_driver_register:
 * @driver: device driver definition
 *
 * Register a new device driver
 *
 * Returns: %0 on success
 */
int connman_device_driver_register(struct connman_device_driver *driver)
{
	DBG("driver %p name %s", driver, driver->name);

	driver_list = g_slist_insert_sorted(driver_list, driver,
							compare_priority);

	__connman_element_foreach(NULL, CONNMAN_ELEMENT_TYPE_DEVICE,
						probe_driver, driver);

	return 0;
}

/**
 * connman_device_driver_unregister:
 * @driver: device driver definition
 *
 * Remove a previously registered device driver
 */
void connman_device_driver_unregister(struct connman_device_driver *driver)
{
	DBG("driver %p name %s", driver, driver->name);

	driver_list = g_slist_remove(driver_list, driver);

	__connman_element_foreach(NULL, CONNMAN_ELEMENT_TYPE_DEVICE,
						remove_driver, driver);
}

static void unregister_network(gpointer data)
{
	struct connman_network *network = data;

	DBG("network %p", network);

	connman_element_unregister((struct connman_element *) network);

	connman_network_unref(network);
}

static void device_destruct(struct connman_element *element)
{
	struct connman_device *device = element->device;

	DBG("element %p name %s", element, element->name);

	g_free(device->node);
	g_free(device->name);
	g_free(device->interface);

	g_free(device->last_network);

	g_hash_table_destroy(device->networks);
	device->networks = NULL;
}

/**
 * connman_device_create:
 * @node: device node name (for example an address)
 * @type: device type
 *
 * Allocate a new device of given #type and assign the #node name to it.
 *
 * Returns: a newly-allocated #connman_device structure
 */
struct connman_device *connman_device_create(const char *node,
						enum connman_device_type type)
{
	struct connman_device *device;
	const char *str;

	DBG("node %s type %d", node, type);

	device = g_try_new0(struct connman_device, 1);
	if (device == NULL)
		return NULL;

	DBG("device %p", device);

	__connman_element_initialize(&device->element);

	device->element.name = g_strdup(node);
	device->element.type = CONNMAN_ELEMENT_TYPE_DEVICE;

	device->element.device = device;
	device->element.destruct = device_destruct;

	str = type2string(type);
	if (str != NULL)
		connman_element_set_static_property(&device->element,
					"Type", DBUS_TYPE_STRING, &str);

	device->element.ipv4.method = CONNMAN_IPV4_METHOD_DHCP;

	device->type   = type;
	device->mode   = CONNMAN_DEVICE_MODE_UNKNOWN;
	device->policy = CONNMAN_DEVICE_POLICY_AUTO;

	switch (type) {
	case CONNMAN_DEVICE_TYPE_UNKNOWN:
	case CONNMAN_DEVICE_TYPE_VENDOR:
		device->priority = 0;
		break;
	case CONNMAN_DEVICE_TYPE_ETHERNET:
	case CONNMAN_DEVICE_TYPE_WIFI:
		device->priority = 100;
		break;
	case CONNMAN_DEVICE_TYPE_WIMAX:
		device->priority = 20;
		break;
	case CONNMAN_DEVICE_TYPE_BLUETOOTH:
		device->priority = 50;
		break;
	case CONNMAN_DEVICE_TYPE_HSO:
	case CONNMAN_DEVICE_TYPE_NOZOMI:
	case CONNMAN_DEVICE_TYPE_HUAWEI:
	case CONNMAN_DEVICE_TYPE_NOVATEL:
		device->priority = 60;
		break;
	}

	device->networks = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, unregister_network);

	return device;
}

/**
 * connman_device_ref:
 * @device: device structure
 *
 * Increase reference counter of device
 */
struct connman_device *connman_device_ref(struct connman_device *device)
{
	if (connman_element_ref(&device->element) == NULL)
		return NULL;

	return device;
}

/**
 * connman_device_unref:
 * @device: device structure
 *
 * Decrease reference counter of device
 */
void connman_device_unref(struct connman_device *device)
{
	connman_element_unref(&device->element);
}

/**
 * connman_device_get_name:
 * @device: device structure
 *
 * Get unique name of device
 */
const char *connman_device_get_name(struct connman_device *device)
{
	return device->element.name;
}

/**
 * connman_device_get_path:
 * @device: device structure
 *
 * Get path name of device
 */
const char *connman_device_get_path(struct connman_device *device)
{
	return device->element.path;
}

/**
 * connman_device_set_index:
 * @device: device structure
 * @index: index number
 *
 * Set index number of device
 */
void connman_device_set_index(struct connman_device *device, int index)
{
	device->element.index = index;
}

/**
 * connman_device_get_index:
 * @device: device structure
 *
 * Get index number of device
 */
int connman_device_get_index(struct connman_device *device)
{
	return device->element.index;
}

/**
 * connman_device_set_interface:
 * @device: device structure
 * @interface: interface name
 *
 * Set interface name of device
 */
void connman_device_set_interface(struct connman_device *device,
							const char *interface)
{
	g_free(device->element.devname);
	device->element.devname = g_strdup(interface);

	g_free(device->interface);
	device->interface = g_strdup(interface);
}

/**
 * connman_device_get_interface:
 * @device: device structure
 *
 * Get interface name of device
 */
const char *connman_device_get_interface(struct connman_device *device)
{
	return device->interface;
}

/**
 * connman_device_set_policy:
 * @device: device structure
 * @policy: power and connection policy
 *
 * Change power and connection policy of device
 */
void connman_device_set_policy(struct connman_device *device,
					enum connman_device_policy policy)
{
	device->policy = policy;
}

/**
 * connman_device_set_mode:
 * @device: device structure
 * @mode: network mode
 *
 * Change network mode of device
 */
void connman_device_set_mode(struct connman_device *device,
						enum connman_device_mode mode)
{
	device->mode = mode;
}

/**
 * connman_device_get_mode:
 * @device: device structure
 *
 * Get network mode of device
 */
enum connman_device_mode connman_device_get_mode(struct connman_device *device)
{
	return device->mode;
}

/**
 * connman_device_set_powered:
 * @device: device structure
 * @powered: powered state
 *
 * Change power state of device
 */
int connman_device_set_powered(struct connman_device *device,
						connman_bool_t powered)
{
	DBusMessage *signal;
	DBusMessageIter entry, value;
	const char *key = "Powered";

	DBG("driver %p powered %d", device, powered);

	if (device->powered == powered)
		return -EALREADY;

	device->powered = powered;

	signal = dbus_message_new_signal(device->element.path,
				CONNMAN_DEVICE_INTERFACE, "PropertyChanged");
	if (signal == NULL)
		return 0;

	dbus_message_iter_init_append(signal, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					DBUS_TYPE_BOOLEAN_AS_STRING, &value);
	dbus_message_iter_append_basic(&value, DBUS_TYPE_BOOLEAN, &powered);
	dbus_message_iter_close_container(&entry, &value);

	g_dbus_send_message(connection, signal);

	if (powered == FALSE)
		return 0;

	if (device->policy != CONNMAN_DEVICE_POLICY_AUTO)
		return 0;

	if (device->driver->scan)
		device->driver->scan(device);

	return 0;
}

/**
 * connman_device_set_carrier:
 * @device: device structure
 * @carrier: carrier state
 *
 * Change carrier state of device (only for device without scanning)
 */
int connman_device_set_carrier(struct connman_device *device,
						connman_bool_t carrier)
{
	DBG("driver %p carrier %d", device, carrier);

	switch (device->mode) {
	case CONNMAN_DEVICE_MODE_UNKNOWN:
	case CONNMAN_DEVICE_MODE_NETWORK_SINGLE:
	case CONNMAN_DEVICE_MODE_NETWORK_MULTIPLE:
		return -EINVAL;
	case CONNMAN_DEVICE_MODE_TRANSPORT_IP:
		break;
	}

	if (device->carrier == carrier)
		return -EALREADY;

	device->carrier = carrier;

	if (carrier == TRUE) {
		enum connman_element_type type = CONNMAN_ELEMENT_TYPE_UNKNOWN;
		struct connman_element *element;

		switch (device->element.ipv4.method) {
		case CONNMAN_IPV4_METHOD_UNKNOWN:
		case CONNMAN_IPV4_METHOD_OFF:
			return 0;
		case CONNMAN_IPV4_METHOD_STATIC:
			type = CONNMAN_ELEMENT_TYPE_IPV4;
			break;
		case CONNMAN_IPV4_METHOD_DHCP:
			type = CONNMAN_ELEMENT_TYPE_DHCP;
			break;
		}

		element = connman_element_create(NULL);
		if (element != NULL) {
			element->type  = type;
			element->index = device->element.index;

			if (connman_element_register(element,
							&device->element) < 0)
				connman_element_unref(element);
		}
	} else
		connman_element_unregister_children(&device->element);

	return 0;
}

void __connman_device_disconnect(struct connman_device *device)
{
	GHashTableIter iter;
	gpointer key, value;

	DBG("device %p", device);

	connman_device_set_disconnected(device, TRUE);

	g_hash_table_iter_init(&iter, device->networks);

	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		struct connman_network *network = value;

		if (connman_network_get_connected(network) == TRUE)
			__connman_network_disconnect(network);
	}
}

static void connect_known_network(struct connman_device *device)
{
	struct connman_network *network = NULL;
	GHashTableIter iter;
	gpointer key, value;
	unsigned int count = 0;

	DBG("device %p", device);

	g_hash_table_iter_init(&iter, device->networks);

	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		connman_uint8_t old_priority, new_priority;
		connman_uint8_t old_strength, new_strength;
		const char *name;

		count++;

		if (connman_network_get_available(value) == FALSE)
			continue;

		name = connman_network_get_string(value, "Name");
		if (name != NULL && device->last_network != NULL) {
			if (g_str_equal(name, device->last_network) == TRUE) {
				network = value;
				break;
			}
		}

		if (connman_network_get_remember(value) == FALSE)
			continue;

		if (network == NULL) {
			network = value;
			continue;
		}

		old_priority = connman_network_get_uint8(network, "Priority");
		new_priority = connman_network_get_uint8(value, "Priority");

		if (new_priority != old_priority) {
			if (new_priority > old_priority)
				network = value;
			continue;
		}

		old_strength = connman_network_get_uint8(network, "Strength");
		new_strength = connman_network_get_uint8(value, "Strength");

		if (new_strength > old_strength)
			network = value;
	}

	if (network != NULL) {
		int err;

		err = connman_network_connect(network);
		if (err == 0 || err == -EINPROGRESS)
			return;
	}

	if (count > 0)
		return;

	if (device->driver && device->driver->scan)
		device->driver->scan(device);
}

static void mark_network_unavailable(gpointer key, gpointer value,
							gpointer user_data)
{
	struct connman_network *network = value;

	if (connman_network_get_connected(network) == TRUE)
		return;

	connman_network_set_available(network, FALSE);
}

static gboolean remove_unavailable_network(gpointer key, gpointer value,
							gpointer user_data)
{
	struct connman_network *network = value;

	if (connman_network_get_connected(network) == TRUE)
		return FALSE;

	if (connman_network_get_remember(network) == TRUE)
		return FALSE;

	if (connman_network_get_available(network) == TRUE)
		return FALSE;

	return TRUE;
}

/**
 * connman_device_set_scanning:
 * @device: device structure
 * @scanning: scanning state
 *
 * Change scanning state of device
 */
int connman_device_set_scanning(struct connman_device *device,
						connman_bool_t scanning)
{
	DBusMessage *signal;
	DBusMessageIter entry, value;
	const char *key = "Scanning";

	DBG("driver %p scanning %d", device, scanning);

	if (!device->driver || !device->driver->scan)
		return -EINVAL;

	if (device->scanning == scanning)
		return -EALREADY;

	device->scanning = scanning;

	signal = dbus_message_new_signal(device->element.path,
				CONNMAN_DEVICE_INTERFACE, "PropertyChanged");
	if (signal == NULL)
		return 0;

	dbus_message_iter_init_append(signal, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					DBUS_TYPE_BOOLEAN_AS_STRING, &value);
	dbus_message_iter_append_basic(&value, DBUS_TYPE_BOOLEAN, &scanning);
	dbus_message_iter_close_container(&entry, &value);

	g_dbus_send_message(connection, signal);

	if (scanning == TRUE) {
		g_hash_table_foreach(device->networks,
					mark_network_unavailable, NULL);
		return 0;
	}

	g_hash_table_foreach_remove(device->networks,
					remove_unavailable_network, NULL);

	if (device->connections > 0)
		return 0;

	if (device->disconnected == TRUE)
		return 0;

	if (device->policy != CONNMAN_DEVICE_POLICY_AUTO)
		return 0;

	connect_known_network(device);

	return 0;
}

/**
 * connman_device_set_disconnected:
 * @device: device structure
 * @disconnected: disconnected state
 *
 * Change disconnected state of device (only for device with networks)
 */
int connman_device_set_disconnected(struct connman_device *device,
						connman_bool_t disconnected)
{
	DBG("driver %p disconnected %d", device, disconnected);

	switch (device->mode) {
	case CONNMAN_DEVICE_MODE_UNKNOWN:
	case CONNMAN_DEVICE_MODE_TRANSPORT_IP:
		return -EINVAL;
	case CONNMAN_DEVICE_MODE_NETWORK_SINGLE:
	case CONNMAN_DEVICE_MODE_NETWORK_MULTIPLE:
		break;
	}

	if (device->disconnected == disconnected)
		return -EALREADY;

	device->disconnected = disconnected;

	return 0;
}

/**
 * connman_device_set_string:
 * @device: device structure
 * @key: unique identifier
 * @value: string value
 *
 * Set string value for specific key
 */
int connman_device_set_string(struct connman_device *device,
					const char *key, const char *value)
{
	DBG("device %p key %s value %s", device, key, value);

	if (g_str_equal(key, "Name") == TRUE) {
		g_free(device->name);
		device->name = g_strdup(value);
	} else if (g_str_equal(key, "Node") == TRUE) {
		g_free(device->node);
		device->node = g_strdup(value);
	}

	return 0;
}

/**
 * connman_device_get_string:
 * @device: device structure
 * @key: unique identifier
 *
 * Get string value for specific key
 */
const char *connman_device_get_string(struct connman_device *device,
							const char *key)
{
	DBG("device %p key %s", device, key);

	if (g_str_equal(key, "Name") == TRUE)
		return device->name;
	else if (g_str_equal(key, "Node") == TRUE)
		return device->node;

	return NULL;
}

static void set_offlinemode(struct connman_element *element, gpointer user_data)
{
	struct connman_device *device = element->device;
	connman_bool_t offlinemode = GPOINTER_TO_UINT(user_data);
	connman_bool_t powered;

	DBG("element %p name %s", element, element->name);

	if (device == NULL)
		return;

	powered = (offlinemode == TRUE) ? FALSE : TRUE;

	if (device->powered == powered)
		return;

	set_powered(device, powered);
}

int __connman_device_set_offlinemode(connman_bool_t offlinemode)
{
	DBG("offlinmode %d", offlinemode);

	__connman_element_foreach(NULL, CONNMAN_ELEMENT_TYPE_DEVICE,
			set_offlinemode, GUINT_TO_POINTER(offlinemode));

	return 0;
}

void __connman_device_increase_connections(struct connman_device *device)
{
	device->connections++;
}

void __connman_device_decrease_connections(struct connman_device *device)
{
	device->connections--;
}

/**
 * connman_device_add_network:
 * @device: device structure
 * @network: network structure
 *
 * Add new network to the device
 */
int connman_device_add_network(struct connman_device *device,
					struct connman_network *network)
{
	const char *identifier = connman_network_get_identifier(network);
	int err;

	DBG("device %p network %p", device, network);

	switch (device->mode) {
	case CONNMAN_DEVICE_MODE_UNKNOWN:
	case CONNMAN_DEVICE_MODE_TRANSPORT_IP:
		return -EINVAL;
	case CONNMAN_DEVICE_MODE_NETWORK_SINGLE:
	case CONNMAN_DEVICE_MODE_NETWORK_MULTIPLE:
		break;
	}

	__connman_network_set_device(network, device);

	__connman_storage_load_network(network);

	err = connman_element_register((struct connman_element *) network,
							&device->element);
	if (err < 0) {
		__connman_network_set_device(network, NULL);
		return err;
	}

	g_hash_table_insert(device->networks, g_strdup(identifier),
								network);

	return 0;
}

/**
 * connman_device_get_network:
 * @device: device structure
 * @identifier: network identifier
 *
 * Get network for given identifier
 */
struct connman_network *connman_device_get_network(struct connman_device *device,
							const char *identifier)
{
	DBG("device %p identifier %s", device, identifier);

	return g_hash_table_lookup(device->networks, identifier);
}

/**
 * connman_device_remove_network:
 * @device: device structure
 * @identifier: network identifier
 *
 * Remove network for given identifier
 */
int connman_device_remove_network(struct connman_device *device,
							const char *identifier)
{
	DBG("device %p identifier %s", device, identifier);

	g_hash_table_remove(device->networks, identifier);

	return 0;
}

void __connman_device_set_network(struct connman_device *device,
					struct connman_network *network)
{
	const char *name;

	if (network != NULL) {
		name = connman_network_get_string(network, "Name");
		device->last_network = g_strdup(name);
	}

	device->network = network;
}

/**
 * connman_device_register:
 * @device: device structure
 *
 * Register device with the system
 */
int connman_device_register(struct connman_device *device)
{
	__connman_storage_load_device(device);

	switch (device->mode) {
	case CONNMAN_DEVICE_MODE_UNKNOWN:
	case CONNMAN_DEVICE_MODE_TRANSPORT_IP:
		break;
	case CONNMAN_DEVICE_MODE_NETWORK_SINGLE:
	case CONNMAN_DEVICE_MODE_NETWORK_MULTIPLE:
		__connman_storage_init_network(device);
		break;
	}

	return connman_element_register(&device->element, NULL);
}

/**
 * connman_device_unregister:
 * @device: device structure
 *
 * Unregister device with the system
 */
void connman_device_unregister(struct connman_device *device)
{
	__connman_storage_save_device(device);

	connman_element_unregister(&device->element);
}

/**
 * connman_device_get_data:
 * @device: device structure
 *
 * Get private device data pointer
 */
void *connman_device_get_data(struct connman_device *device)
{
	return device->driver_data;
}

/**
 * connman_device_set_data:
 * @device: device structure
 * @data: data pointer
 *
 * Set private device data pointer
 */
void connman_device_set_data(struct connman_device *device, void *data)
{
	device->driver_data = data;
}

static gboolean match_driver(struct connman_device *device,
					struct connman_device_driver *driver)
{
	if (device->type == driver->type ||
			driver->type == CONNMAN_DEVICE_TYPE_UNKNOWN)
		return TRUE;

	return FALSE;
}

static int device_probe(struct connman_element *element)
{
	struct connman_device *device = element->device;
	GSList *list;

	DBG("element %p name %s", element, element->name);

	if (device == NULL)
		return -ENODEV;

	if (device->driver != NULL)
		return -EALREADY;

	for (list = driver_list; list; list = list->next) {
		struct connman_device_driver *driver = list->data;

		if (match_driver(device, driver) == FALSE)
			continue;

		DBG("driver %p name %s", driver, driver->name);

		if (driver->probe(device) == 0) {
			device->driver = driver;
			break;
		}
	}

	if (device->driver == NULL)
		return -ENODEV;

	return setup_device(device);
}

static void device_remove(struct connman_element *element)
{
	struct connman_device *device = element->device;

	DBG("element %p name %s", element, element->name);

	if (device == NULL)
		return;

	if (device->driver == NULL)
		return;

	remove_device(device);
}

static struct connman_driver device_driver = {
	.name		= "device",
	.type		= CONNMAN_ELEMENT_TYPE_DEVICE,
	.priority	= CONNMAN_DRIVER_PRIORITY_LOW,
	.probe		= device_probe,
	.remove		= device_remove,
};

static int device_load(struct connman_device *device)
{
	GKeyFile *keyfile;
	gchar *pathname, *data = NULL;
	gsize length;
	char *str;
	int val;

	DBG("device %p", device);

	pathname = g_strdup_printf("%s/%s.conf", STORAGEDIR,
							device->element.name);
	if (pathname == NULL)
		return -ENOMEM;

	keyfile = g_key_file_new();

	if (g_file_get_contents(pathname, &data, &length, NULL) == FALSE) {
		g_free(pathname);
		return -ENOENT;
	}

	g_free(pathname);

	if (g_key_file_load_from_data(keyfile, data, length,
							0, NULL) == FALSE) {
		g_free(data);
		return -EILSEQ;
	}

	g_free(data);

	str = g_key_file_get_string(keyfile, "Configuration", "Policy", NULL);
	if (str != NULL) {
		device->policy = string2policy(str);
		g_free(str);
	}

	val = g_key_file_get_integer(keyfile, "Configuration",
							"Priority", NULL);
	if (val > 0)
		device->priority = val;

	str = g_key_file_get_string(keyfile, "Configuration",
							"LastNetwork", NULL);
	if (str != NULL)
		device->last_network = str;

	g_key_file_free(keyfile);

	return 0;
}

static int device_save(struct connman_device *device)
{
	GKeyFile *keyfile;
	gchar *pathname, *data = NULL;
	gsize length;
	const char *str;

	DBG("device %p", device);

	pathname = g_strdup_printf("%s/%s.conf", STORAGEDIR,
							device->element.name);
	if (pathname == NULL)
		return -ENOMEM;

	keyfile = g_key_file_new();

	if (g_file_get_contents(pathname, &data, &length, NULL) == FALSE)
		goto update;

	if (length > 0) {
		if (g_key_file_load_from_data(keyfile, data, length,
							0, NULL) == FALSE)
			goto done;
	}

	g_free(data);

update:
	str = policy2string(device->policy);
	if (str != NULL)
		g_key_file_set_string(keyfile, "Configuration", "Policy", str);

	if (device->priority > 0)
		g_key_file_set_integer(keyfile, "Configuration",
						"Priority", device->priority);

	if (device->last_network != NULL)
		g_key_file_set_string(keyfile, "Configuration",
					"LastNetwork", device->last_network);

	data = g_key_file_to_data(keyfile, &length, NULL);

	g_file_set_contents(pathname, data, length, NULL);

done:
	g_free(data);

	g_key_file_free(keyfile);

	g_free(pathname);

	if (device->network != NULL)
		__connman_storage_save_network(device->network);

	return 0;
}

static struct connman_storage device_storage = {
	.name		= "device",
	.priority	= CONNMAN_STORAGE_PRIORITY_LOW,
	.device_load	= device_load,
	.device_save	= device_save,
};

int __connman_device_init(void)
{
	DBG("");

	connection = connman_dbus_get_connection();

	if (connman_storage_register(&device_storage) < 0)
		connman_error("Failed to register device storage");

	return connman_driver_register(&device_driver);
}

void __connman_device_cleanup(void)
{
	DBG("");

	connman_driver_unregister(&device_driver);

	connman_storage_unregister(&device_storage);

	dbus_connection_unref(connection);
}
