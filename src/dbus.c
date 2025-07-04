/* copyright 2013 Sascha Kruse and contributors (see LICENSE for licensing
 * information) */
#include "dbus.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

#include "draw.h"
#include "dunst.h"
#include "log.h"
#include "menu.h"
#include "notification.h"
#include "option_parser.h"
#include "queues.h"
#include "rules.h"
#include "settings.h"
#include "settings_data.h"
#include "utils.h"

#define FDN_PATH "/org/freedesktop/Notifications"
#define FDN_IFAC "org.freedesktop.Notifications"
#define FDN_NAME "org.freedesktop.Notifications"

#define DUNST_PATH "/org/freedesktop/Notifications"
#define DUNST_IFAC "org.dunstproject.cmd0"
#define DUNST_NAME "org.freedesktop.Notifications"

#define PROPERTIES_IFAC "org.freedesktop.DBus.Properties"

GDBusConnection *dbus_conn = NULL;

static GDBusNodeInfo *introspection_data = NULL;

static const char *introspection_xml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<node name=\"" FDN_PATH "\">"
    "    <interface name=\"" FDN_IFAC "\">"

    "        <method name=\"GetCapabilities\">"
    "            <arg direction=\"out\" name=\"capabilities\"    type=\"as\"/>"
    "        </method>"

    "        <method name=\"Notify\">"
    "            <arg direction=\"in\"  name=\"app_name\"        type=\"s\"/>"
    "            <arg direction=\"in\"  name=\"replaces_id\"     type=\"u\"/>"
    "            <arg direction=\"in\"  name=\"app_icon\"        type=\"s\"/>"
    "            <arg direction=\"in\"  name=\"summary\"         type=\"s\"/>"
    "            <arg direction=\"in\"  name=\"body\"            type=\"s\"/>"
    "            <arg direction=\"in\"  name=\"actions\"         type=\"as\"/>"
    "            <arg direction=\"in\"  name=\"hints\"           "
    "type=\"a{sv}\"/>"
    "            <arg direction=\"in\"  name=\"expire_timeout\"  type=\"i\"/>"
    "            <arg direction=\"out\" name=\"id\"              type=\"u\"/>"
    "        </method>"

    "        <method name=\"CloseNotification\">"
    "            <arg direction=\"in\"  name=\"id\"              type=\"u\"/>"
    "        </method>"

    "        <method name=\"GetServerInformation\">"
    "            <arg direction=\"out\" name=\"name\"            type=\"s\"/>"
    "            <arg direction=\"out\" name=\"vendor\"          type=\"s\"/>"
    "            <arg direction=\"out\" name=\"version\"         type=\"s\"/>"
    "            <arg direction=\"out\" name=\"spec_version\"    type=\"s\"/>"
    "        </method>"

    "        <signal name=\"NotificationClosed\">"
    "            <arg name=\"id\"         type=\"u\"/>"
    "            <arg name=\"reason\"     type=\"u\"/>"
    "        </signal>"

    "        <signal name=\"ActionInvoked\">"
    "            <arg name=\"id\"         type=\"u\"/>"
    "            <arg name=\"action_key\" type=\"s\"/>"
    "        </signal>"
    "    </interface>"
    "    <interface name=\"" DUNST_IFAC "\">"

    "        <method name=\"ContextMenuCall\"/>"
    "        <method name=\"NotificationAction\">"
    "            <arg name=\"number\"     type=\"u\"/>"
    "        </method>"
    "        <method name=\"NotificationClearHistory\"/>"
    "        <method name=\"NotificationCloseLast\" />"
    "        <method name=\"NotificationCloseAll\"  />"
    "        <method name=\"NotificationListShowing\">"
    "            <arg direction=\"out\" name=\"notifications\"   "
    "type=\"aa{sv}\"/>"
    "        </method>"

    "        <method name=\"NotificationListHistory\">"
    "            <arg direction=\"out\" name=\"notifications\"   "
    "type=\"aa{sv}\"/>"
    "        </method>"
    "        <method name=\"NotificationPopHistory\">"
    "            <arg direction=\"in\"  name=\"id\"              type=\"u\"/>"
    "        </method>"
    "        <method name=\"NotificationRemoveFromHistory\">"
    "            <arg direction=\"in\"  name=\"id\"              type=\"u\"/>"
    "        </method>"
    "        <method name=\"NotificationShow\"/>"
    "        <method name=\"RuleEnable\">"
    "            <arg name=\"name\"     type=\"s\"/>"
    "            <arg name=\"state\"    type=\"i\"/>"
    "        </method>"
    "        <method name=\"RuleList\">"
    "            <arg direction=\"out\" name=\"rules\"           "
    "type=\"aa{sv}\"/>"
    "        </method>"
    "        <method name=\"ConfigReload\">"
    "            <arg direction=\"in\" name=\"configs\"  type=\"as\"/>"
    "        </method>"
    "        <method name=\"Ping\"/>"

    "        <property name=\"paused\" type=\"b\" access=\"readwrite\">"
    "            <annotation "
    "name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"true\"/>"
    "        </property>"

    "        <property name=\"pauseLevel\" type=\"u\" access=\"readwrite\">"
    "            <annotation "
    "name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"true\"/>"
    "        </property>"

    "        <property name=\"displayedLength\" type=\"u\" access=\"read\" />"
    "        <property name=\"historyLength\" type=\"u\" access=\"read\" />"
    "        <property name=\"waitingLength\" type=\"u\" access=\"read\" />"

    "        <signal name=\"NotificationHistoryRemoved\">"
    "            <arg name=\"id\"         type=\"u\"/>"
    "        </signal>"

    "        <signal name=\"NotificationHistoryCleared\">"
    "            <arg name=\"count\"      type=\"u\"/>"
    "        </signal>"

    "        <signal name=\"ConfigReloaded\">"
    "            <arg name=\"configs\"    type=\"as\"/>"
    "        </signal>"

    "    </interface>"
    "</node>";

static const char *stack_tag_hints[] = {"synchronous", "private-synchronous",
                                        "x-canonical-private-synchronous",
                                        "x-dunst-stack-tag"};

struct dbus_method {
  const char *method_name;
  void (*method)(GDBusConnection *connection, const gchar *sender,
                 GVariant *parameters, GDBusMethodInvocation *invocation);
};

#define DBUS_METHOD(name)                                                      \
  static void dbus_cb_##name(GDBusConnection *connection, const gchar *sender, \
                             GVariant *parameters,                             \
                             GDBusMethodInvocation *invocation)

int cmp_methods(const void *vkey, const void *velem) {
  const char *key = (const char *)vkey;
  const struct dbus_method *m = (const struct dbus_method *)velem;

  return strcmp(key, m->method_name);
}

DBUS_METHOD(Notify);
DBUS_METHOD(CloseNotification);
DBUS_METHOD(GetCapabilities);
DBUS_METHOD(GetServerInformation);
static struct dbus_method methods_fdn[] = {
    {"CloseNotification", dbus_cb_CloseNotification},
    {"GetCapabilities", dbus_cb_GetCapabilities},
    {"GetServerInformation", dbus_cb_GetServerInformation},
    {"Notify", dbus_cb_Notify},
};

void dbus_cb_fdn_methods(GDBusConnection *connection, const gchar *sender,
                         const gchar *object_path, const gchar *interface_name,
                         const gchar *method_name, GVariant *parameters,
                         GDBusMethodInvocation *invocation,
                         gpointer user_data) {
  struct dbus_method *m =
      bsearch(method_name, methods_fdn, G_N_ELEMENTS(methods_fdn),
              sizeof(struct dbus_method), cmp_methods);

  if (m) {
    m->method(connection, sender, parameters, invocation);
  } else {
    LOG_M("Unknown method name: '%s' (sender: '%s').", method_name, sender);
  }
}

DBUS_METHOD(dunst_ContextMenuCall);
DBUS_METHOD(dunst_NotificationAction);
DBUS_METHOD(dunst_NotificationClearHistory);
DBUS_METHOD(dunst_NotificationCloseAll);
DBUS_METHOD(dunst_NotificationCloseLast);
DBUS_METHOD(dunst_NotificationListShowing);
DBUS_METHOD(dunst_NotificationListHistory);
DBUS_METHOD(dunst_NotificationPopHistory);
DBUS_METHOD(dunst_NotificationRemoveFromHistory);
DBUS_METHOD(dunst_NotificationShow);
DBUS_METHOD(dunst_RuleEnable);
DBUS_METHOD(dunst_RuleList);
DBUS_METHOD(dunst_ConfigReload);
DBUS_METHOD(dunst_Ping);

// NOTE: Keep the names sorted alphabetically
static struct dbus_method methods_dunst[] = {
    {"ConfigReload", dbus_cb_dunst_ConfigReload},
    {"ContextMenuCall", dbus_cb_dunst_ContextMenuCall},
    {"NotificationAction", dbus_cb_dunst_NotificationAction},
    {"NotificationClearHistory", dbus_cb_dunst_NotificationClearHistory},
    {"NotificationCloseAll", dbus_cb_dunst_NotificationCloseAll},
    {"NotificationCloseLast", dbus_cb_dunst_NotificationCloseLast},
    {"NotificationListHistory", dbus_cb_dunst_NotificationListHistory},
    {"NotificationListShowing", dbus_cb_dunst_NotificationListShowing},
    {"NotificationPopHistory", dbus_cb_dunst_NotificationPopHistory},
    {"NotificationRemoveFromHistory",
     dbus_cb_dunst_NotificationRemoveFromHistory},
    {"NotificationShow", dbus_cb_dunst_NotificationShow},
    {"Ping", dbus_cb_dunst_Ping},
    {"RuleEnable", dbus_cb_dunst_RuleEnable},
    {"RuleList", dbus_cb_dunst_RuleList},
};

void dbus_cb_dunst_methods(GDBusConnection *connection, const gchar *sender,
                           const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *method_name, GVariant *parameters,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data) {
  struct dbus_method *m =
      bsearch(method_name, methods_dunst, G_N_ELEMENTS(methods_dunst),
              sizeof(struct dbus_method), cmp_methods);

  if (m) {
    m->method(connection, sender, parameters, invocation);
  } else {
    LOG_M("Unknown method name: '%s' (sender: '%s').", method_name, sender);
  }
}

static void dbus_cb_dunst_ContextMenuCall(GDBusConnection *connection,
                                          const gchar *sender,
                                          GVariant *parameters,
                                          GDBusMethodInvocation *invocation) {
  LOG_D("CMD: Calling context menu");
  context_menu();

  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void
dbus_cb_dunst_NotificationAction(GDBusConnection *connection,
                                 const gchar *sender, GVariant *parameters,
                                 GDBusMethodInvocation *invocation) {
  guint32 notification_nr = 0;
  g_variant_get(parameters, "(u)", &notification_nr);

  LOG_D("CMD: Calling action for notification %d", notification_nr);

  if (queues_length_waiting() < notification_nr) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Couldn't activate action for notification in position %d, %d "
        "notifications currently open",
        notification_nr, queues_length_waiting());
    return;
  }

  struct notification *n =
      g_list_nth_data(queues_get_displayed(), notification_nr);

  if (n) {
    LOG_D("CMD: Calling action for notification %s", n->summary);
    notification_do_action(n);
  }

  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void dbus_cb_dunst_NotificationClearHistory(
    GDBusConnection *connection, const gchar *sender, GVariant *parameters,
    GDBusMethodInvocation *invocation) {
  LOG_D("CMD: Clearing the history");
  queues_history_clear();
  wake_up();

  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void
dbus_cb_dunst_NotificationCloseAll(GDBusConnection *connection,
                                   const gchar *sender, GVariant *parameters,
                                   GDBusMethodInvocation *invocation) {
  LOG_D("CMD: Pushing all to history");
  queues_history_push_all();
  wake_up();

  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void
dbus_cb_dunst_NotificationCloseLast(GDBusConnection *connection,
                                    const gchar *sender, GVariant *parameters,
                                    GDBusMethodInvocation *invocation) {
  LOG_D("CMD: Closing last notification");
  const GList *list = queues_get_displayed();
  if (list && list->data) {
    struct notification *n = list->data;
    queues_notification_close_id(n->id, REASON_USER);
    wake_up();
  }

  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void dbus_cb_dunst_NotificationShow(GDBusConnection *connection,
                                           const gchar *sender,
                                           GVariant *parameters,
                                           GDBusMethodInvocation *invocation) {
  LOG_D("CMD: Showing last notification from history");
  queues_history_pop();
  wake_up();

  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void
dbus_cb_dunst_NotificationListShowing(GDBusConnection *connection,
                                      const gchar *sender, GVariant *parameters,
                                      GDBusMethodInvocation *invocation) {
  LOG_D("CMD: Listing all notifications from history");

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("aa{sv}"));

  GList *notification_list = queues_get_displayed();

  // reverse chronological list
  for (int i = queues_length_displayed(); i > 0; i--) {
    struct notification *n;
    n = g_list_nth_data(notification_list, i - 1);

    GVariantBuilder n_builder;
    g_variant_builder_init(&n_builder, G_VARIANT_TYPE("a{sv}"));

    char *body, *msg, *summary, *appname, *category;
    char *default_action_name, *icon_path;
    char *urls, *stack_tag;
    const char *urgency;

    body = (n->body == NULL) ? "" : n->body;
    msg = (n->msg == NULL) ? "" : n->msg;
    summary = (n->summary == NULL) ? "" : n->summary;
    appname = (n->appname == NULL) ? "" : n->appname;
    category = (n->category == NULL) ? "" : n->category;
    default_action_name =
        (n->default_action_name == NULL) ? "" : n->default_action_name;
    icon_path = (n->icon_path == NULL) ? "" : n->icon_path;
    urgency = notification_urgency_to_string(n->urgency);
    urls = (n->urls == NULL) ? "" : n->urls;
    stack_tag = (n->stack_tag == NULL) ? "" : n->stack_tag;

    g_variant_builder_add(&n_builder, "{sv}", "body",
                          g_variant_new_string(body));
    g_variant_builder_add(&n_builder, "{sv}", "message",
                          g_variant_new_string(msg));
    g_variant_builder_add(&n_builder, "{sv}", "summary",
                          g_variant_new_string(summary));
    g_variant_builder_add(&n_builder, "{sv}", "appname",
                          g_variant_new_string(appname));
    g_variant_builder_add(&n_builder, "{sv}", "category",
                          g_variant_new_string(category));
    g_variant_builder_add(&n_builder, "{sv}", "default_action_name",
                          g_variant_new_string(default_action_name));
    g_variant_builder_add(&n_builder, "{sv}", "icon_path",
                          g_variant_new_string(icon_path));
    g_variant_builder_add(&n_builder, "{sv}", "id", g_variant_new_int32(n->id));
    g_variant_builder_add(&n_builder, "{sv}", "timestamp",
                          g_variant_new_int64(n->timestamp));
    g_variant_builder_add(&n_builder, "{sv}", "timeout",
                          g_variant_new_int64(n->timeout));
    g_variant_builder_add(&n_builder, "{sv}", "progress",
                          g_variant_new_int32(n->progress));
    g_variant_builder_add(&n_builder, "{sv}", "urgency",
                          g_variant_new_string(urgency));
    g_variant_builder_add(&n_builder, "{sv}", "stack_tag",
                          g_variant_new_string(stack_tag));
    g_variant_builder_add(&n_builder, "{sv}", "urls",
                          g_variant_new_string(urls));

    g_variant_builder_add(&builder, "a{sv}", &n_builder);
  }

  g_dbus_method_invocation_return_value(invocation,
                                        g_variant_new("(aa{sv})", &builder));
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void
dbus_cb_dunst_NotificationListHistory(GDBusConnection *connection,
                                      const gchar *sender, GVariant *parameters,
                                      GDBusMethodInvocation *invocation) {
  LOG_D("CMD: Listing all notifications from history");

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("aa{sv}"));

  GList *notification_list = queues_get_history();

  // reverse chronological list
  for (int i = queues_length_history(); i > 0; i--) {
    struct notification *n;
    n = g_list_nth_data(notification_list, i - 1);

    GVariantBuilder n_builder;
    g_variant_builder_init(&n_builder, G_VARIANT_TYPE("a{sv}"));

    char *body, *msg, *summary, *appname, *category;
    char *default_action_name, *icon_path;
    char *urls, *stack_tag;
    const char *urgency;

    body = (n->body == NULL) ? "" : n->body;
    msg = (n->msg == NULL) ? "" : n->msg;
    summary = (n->summary == NULL) ? "" : n->summary;
    appname = (n->appname == NULL) ? "" : n->appname;
    category = (n->category == NULL) ? "" : n->category;
    default_action_name =
        (n->default_action_name == NULL) ? "" : n->default_action_name;
    icon_path = (n->icon_path == NULL) ? "" : n->icon_path;
    urgency = notification_urgency_to_string(n->urgency);
    urls = (n->urls == NULL) ? "" : n->urls;
    stack_tag = (n->stack_tag == NULL) ? "" : n->stack_tag;

    g_variant_builder_add(&n_builder, "{sv}", "body",
                          g_variant_new_string(body));
    g_variant_builder_add(&n_builder, "{sv}", "message",
                          g_variant_new_string(msg));
    g_variant_builder_add(&n_builder, "{sv}", "summary",
                          g_variant_new_string(summary));
    g_variant_builder_add(&n_builder, "{sv}", "appname",
                          g_variant_new_string(appname));
    g_variant_builder_add(&n_builder, "{sv}", "category",
                          g_variant_new_string(category));
    g_variant_builder_add(&n_builder, "{sv}", "default_action_name",
                          g_variant_new_string(default_action_name));
    g_variant_builder_add(&n_builder, "{sv}", "icon_path",
                          g_variant_new_string(icon_path));
    g_variant_builder_add(&n_builder, "{sv}", "id", g_variant_new_int32(n->id));
    g_variant_builder_add(&n_builder, "{sv}", "timestamp",
                          g_variant_new_int64(n->timestamp));
    g_variant_builder_add(&n_builder, "{sv}", "timeout",
                          g_variant_new_int64(n->timeout));
    g_variant_builder_add(&n_builder, "{sv}", "progress",
                          g_variant_new_int32(n->progress));
    g_variant_builder_add(&n_builder, "{sv}", "urgency",
                          g_variant_new_string(urgency));
    g_variant_builder_add(&n_builder, "{sv}", "stack_tag",
                          g_variant_new_string(stack_tag));
    g_variant_builder_add(&n_builder, "{sv}", "urls",
                          g_variant_new_string(urls));

    g_variant_builder_add(&builder, "a{sv}", &n_builder);
  }

  g_dbus_method_invocation_return_value(invocation,
                                        g_variant_new("(aa{sv})", &builder));
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void
dbus_cb_dunst_NotificationPopHistory(GDBusConnection *connection,
                                     const gchar *sender, GVariant *parameters,
                                     GDBusMethodInvocation *invocation) {
  LOG_D("CMD: Popping notification from history");

  guint32 id;
  g_variant_get(parameters, "(u)", &id);

  queues_history_pop_by_id(id);
  wake_up();

  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void dbus_cb_dunst_NotificationRemoveFromHistory(
    GDBusConnection *connection, const gchar *sender, GVariant *parameters,
    GDBusMethodInvocation *invocation) {
  LOG_D("CMD: Removing notification from history");

  guint32 id;
  g_variant_get(parameters, "(u)", &id);

  queues_history_remove_by_id(id);
  wake_up();

  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static const char *enum_to_string(const struct string_to_enum_def values[],
                                  int enum_value) {
  for (size_t i = 0; values[i].string != NULL; i++) {
    if (values[i].enum_value == enum_value) {
      return values[i].string;
    }
  }
  return NULL;
}

static void color_entry(const struct color c, GVariantDict *dict,
                        const char *field_name) {
  char buf[10];
  if (color_to_string(c, buf)) {
    g_variant_dict_insert(dict, field_name, "s", buf);
  }
}

static void gradient_entry(const struct gradient *grad, GVariantDict *dict,
                           const char *field_name) {
  if (GRADIENT_VALID(grad)) {
    if (grad->length == 1) {
      color_entry(grad->colors[0], dict, field_name);
      return;
    }

    char **strv = g_malloc((grad->length + 1) * sizeof(char *));
    for (size_t i = 0; i < grad->length; i++) {
      char buf[10];
      if (color_to_string(grad->colors[i], buf))
        strv[i] = g_strdup(buf);
    }
    strv[grad->length] = NULL;

    g_variant_dict_insert(dict, field_name, "^as", strv);
  }
}

static void dbus_cb_dunst_RuleList(GDBusConnection *connection,
                                   const gchar *sender, GVariant *parameters,
                                   GDBusMethodInvocation *invocation) {
  LOG_D("CMD: Listing all configured rules");

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("aa{sv}"));

  for (GSList *iter = rules; iter; iter = iter->next) {
    struct rule *r = iter->data;

    if (is_special_section(r->name)) {
      continue;
    }

    GVariantDict dict;
    g_variant_dict_init(&dict, NULL);
    g_variant_dict_insert(&dict, "name", "s", r->name);

    // filters - order according to rule_matches_notification
    g_variant_dict_insert(&dict, "enabled", "b", BOOL2G(r->enabled));
    // undocumented filter?
    if (r->match_dbus_timeout > -1)
      g_variant_dict_insert(&dict, "match_dbus_timeout", "i",
                            r->match_dbus_timeout);
    if (r->msg_urgency != URG_NONE)
      g_variant_dict_insert(&dict, "msg_urgency", "s",
                            enum_to_string(urgency_enum_data, r->msg_urgency));
    if (r->match_transient > -1)
      g_variant_dict_insert(&dict, "match_transient", "b",
                            BOOL2G(r->match_transient));
    if (r->appname)
      g_variant_dict_insert(&dict, "appname", "s", r->appname);
    if (r->desktop_entry)
      g_variant_dict_insert(&dict, "desktop_entry", "s", r->desktop_entry);
    if (r->summary)
      g_variant_dict_insert(&dict, "summary", "s", r->summary);
    if (r->body)
      g_variant_dict_insert(&dict, "body", "s", r->body);
    if (r->category)
      g_variant_dict_insert(&dict, "category", "s", r->category);
    if (r->stack_tag)
      g_variant_dict_insert(&dict, "stack_tag", "s", r->stack_tag);

    // settings to apply - order according to rule_apply
    if (r->timeout != -1)
      g_variant_dict_insert(&dict, "timeout", "x", r->timeout);
    if (r->override_dbus_timeout != -1)
      g_variant_dict_insert(&dict, "override_dbus_timeout", "x",
                            r->override_dbus_timeout);
    if (r->urgency != URG_NONE)
      g_variant_dict_insert(&dict, "urgency", "s",
                            enum_to_string(urgency_enum_data, r->urgency));
    if (r->fullscreen != FS_NULL)
      g_variant_dict_insert(
          &dict, "fullscreen", "s",
          enum_to_string(fullscreen_enum_data, r->fullscreen));
    if (r->history_ignore != -1)
      g_variant_dict_insert(&dict, "history_ignore", "b",
                            BOOL2G(r->history_ignore));
    if (r->set_transient != -1)
      g_variant_dict_insert(&dict, "set_transient", "b",
                            BOOL2G(r->set_transient));
    if (r->skip_display != -1)
      g_variant_dict_insert(&dict, "skip_display", "b",
                            BOOL2G(r->skip_display));
    if (r->word_wrap != -1)
      g_variant_dict_insert(&dict, "word_wrap", "b", BOOL2G(r->word_wrap));
    if (r->ellipsize != -1)
      g_variant_dict_insert(&dict, "ellipsize", "s",
                            enum_to_string(ellipsize_enum_data, r->ellipsize));
    if (r->alignment != -1)
      g_variant_dict_insert(
          &dict, "alignment", "s",
          enum_to_string(horizontal_alignment_enum_data, r->alignment));
    if (r->hide_text != -1)
      g_variant_dict_insert(&dict, "hide_text", "b", BOOL2G(r->hide_text));
    if (r->progress_bar_alignment != -1)
      g_variant_dict_insert(&dict, "progress_bar_alignment", "s",
                            enum_to_string(horizontal_alignment_enum_data,
                                           r->progress_bar_alignment));
    if (r->min_icon_size != -1)
      g_variant_dict_insert(&dict, "min_icon_size", "i", r->min_icon_size);
    if (r->max_icon_size != -1)
      g_variant_dict_insert(&dict, "max_icon_size", "i", r->max_icon_size);
    if (r->action_name)
      g_variant_dict_insert(&dict, "action_name", "s", r->action_name);
    if (r->set_category)
      g_variant_dict_insert(&dict, "set_category", "s", r->set_category);
    if (r->markup != MARKUP_NULL)
      g_variant_dict_insert(&dict, "markup", "s",
                            enum_to_string(markup_mode_enum_data, r->markup));
    if (r->icon_position != -1)
      g_variant_dict_insert(
          &dict, "icon_position", "s",
          enum_to_string(icon_position_enum_data, r->icon_position));
    color_entry(r->fg, &dict, "fg");
    color_entry(r->bg, &dict, "bg");
    gradient_entry(r->highlight, &dict, "highlight");
    color_entry(r->fc, &dict, "fc");
    if (r->format)
      g_variant_dict_insert(&dict, "format", "s", r->format);
    if (r->default_icon)
      g_variant_dict_insert(&dict, "default_icon", "s", r->default_icon);
    if (r->new_icon)
      g_variant_dict_insert(&dict, "new_icon", "s", r->new_icon);
    if (r->script)
      g_variant_dict_insert(&dict, "script", "s", r->script);
    if (r->set_stack_tag)
      g_variant_dict_insert(&dict, "set_stack_tag", "s", r->set_stack_tag);
    if (r->override_pause_level != -1)
      g_variant_dict_insert(&dict, "override_pause_level", "i",
                            r->override_pause_level);

    g_variant_builder_add_value(&builder, g_variant_dict_end(&dict));
  }

  g_dbus_method_invocation_return_value(invocation,
                                        g_variant_new("(aa{sv})", &builder));
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void dbus_cb_dunst_RuleEnable(GDBusConnection *connection,
                                     const gchar *sender, GVariant *parameters,
                                     GDBusMethodInvocation *invocation) {
  // dbus param state: 0 → disable, 1 → enable, 2 → toggle.

  int state = 0;
  char *name = NULL;
  g_variant_get(parameters, "(si)", &name, &state);

  LOG_D("CMD: Changing rule \"%s\" enable state to %d", name, state);

  if (state < 0 || state > 2) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
        "Couldn't understand state %d. It must be 0, 1 or 2", state);
    return;
  }

        struct rule *target_rule = get_rule(name);
        if (target_rule == NULL) {
                g_dbus_method_invocation_return_error(invocation,
                        G_DBUS_ERROR,
                        G_DBUS_ERROR_INVALID_ARGS,
                        "There is no rule named \"%s\"",
                        name);
                g_free(name);
                return;
        }
        g_free(name);

  if (state == 0)
    target_rule->enabled = false;
  else if (state == 1)
    target_rule->enabled = true;
  else if (state == 2)
    target_rule->enabled = !target_rule->enabled;

  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void dbus_cb_dunst_ConfigReload(GDBusConnection *connection,
                                       const gchar *sender,
                                       GVariant *parameters,
                                       GDBusMethodInvocation *invocation) {
  gchar **configs = NULL;
  g_variant_get(parameters, "(^as)", &configs);
  reload(configs);

  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

/* Just a simple Ping command to give the ability to dunstctl to test for the
 * existence of this interface Any other way requires parsing the XML of the
 * Introspection or other foo. Just calling the Ping on an old dunst version
 * will fail. */
static void dbus_cb_dunst_Ping(GDBusConnection *connection, const gchar *sender,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation) {
  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void dbus_cb_GetCapabilities(GDBusConnection *connection,
                                    const gchar *sender, GVariant *parameters,
                                    GDBusMethodInvocation *invocation) {
  GVariantBuilder *builder;
  GVariant *value;

  builder = g_variant_builder_new(G_VARIANT_TYPE("as"));
  g_variant_builder_add(builder, "s", "actions");
  g_variant_builder_add(builder, "s", "body");
  g_variant_builder_add(builder, "s", "body-hyperlinks");
  g_variant_builder_add(builder, "s", "icon-static");

  for (size_t i = 0; i < sizeof(stack_tag_hints) / sizeof(*stack_tag_hints);
       ++i)
    g_variant_builder_add(builder, "s", stack_tag_hints[i]);

  // Since markup isn't a global variable anymore, look it up in the
  // global rule
  struct rule *global_rule = get_rule("global");
  if (global_rule && global_rule->markup != MARKUP_NO)
    g_variant_builder_add(builder, "s", "body-markup");

  value = g_variant_new("(as)", builder);
  g_clear_pointer(&builder, g_variant_builder_unref);
  g_dbus_method_invocation_return_value(invocation, value);

  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static struct notification *dbus_message_to_notification(const gchar *sender,
                                                         GVariant *parameters) {
  /* Assert that the parameters' type is actually correct. Albeit usually DBus
   * already rejects ill typed parameters, it may not be always the case. */
  GVariantType *required_type = g_variant_type_new("(susssasa{sv}i)");
  if (!g_variant_is_of_type(parameters, required_type)) {
    g_variant_type_free(required_type);
    return NULL;
  }

  struct notification *n = notification_create();
  n->dbus_client = g_strdup(sender);
  n->dbus_valid = true;

  GVariant *hints;
  gchar **actions;
  int timeout;

  GVariantIter i;
  g_variant_iter_init(&i, parameters);

  g_variant_iter_next(&i, "s", &n->appname);
  g_variant_iter_next(&i, "u", &n->id);
  g_variant_iter_next(&i, "s", &n->iconname);
  g_variant_iter_next(&i, "s", &n->summary);
  g_variant_iter_next(&i, "s", &n->body);
  g_variant_iter_next(&i, "^a&s", &actions);
  g_variant_iter_next(&i, "@a{?*}", &hints);
  g_variant_iter_next(&i, "i", &timeout);

  gsize num = 0;
  while (actions[num]) {
    if (actions[num + 1]) {
      g_hash_table_insert(n->actions, g_strdup(actions[num]),
                          g_strdup(actions[num + 1]));
      num += 2;
    } else {
      LOG_W("Odd length in actions array. Ignoring element: %s", actions[num]);
      break;
    }
  }

  GVariant *dict_value;
  GVariant *icon_value = NULL;

  // First process the items that can be filtered on
  if ((dict_value =
           g_variant_lookup_value(hints, "urgency", G_VARIANT_TYPE_BYTE))) {
    n->urgency = g_variant_get_byte(dict_value);
    g_variant_unref(dict_value);
  }

  if ((dict_value =
           g_variant_lookup_value(hints, "category", G_VARIANT_TYPE_STRING))) {
    n->category = g_variant_dup_string(dict_value, NULL);
    g_variant_unref(dict_value);
  }

  if ((dict_value = g_variant_lookup_value(hints, "desktop-entry",
                                           G_VARIANT_TYPE_STRING))) {
    n->desktop_entry = g_variant_dup_string(dict_value, NULL);
    g_variant_unref(dict_value);
  }

  if ((dict_value =
           g_variant_lookup_value(hints, "value", G_VARIANT_TYPE_INT32))) {
    n->progress = g_variant_get_int32(dict_value);
    g_variant_unref(dict_value);
  } else if ((dict_value = g_variant_lookup_value(hints, "value",
                                                  G_VARIANT_TYPE_UINT32))) {
    n->progress = g_variant_get_uint32(dict_value);
    g_variant_unref(dict_value);
  }
  if (n->progress < 0)
    n->progress = -1;

  /* Check for hints that define the stack_tag
   *
   * Only accept to first one we find.
   */
  for (size_t i = 0; i < sizeof(stack_tag_hints) / sizeof(*stack_tag_hints);
       ++i) {
    if ((dict_value = g_variant_lookup_value(hints, stack_tag_hints[i],
                                             G_VARIANT_TYPE_STRING))) {
      n->stack_tag = g_variant_dup_string(dict_value, NULL);
      g_variant_unref(dict_value);
      break;
    }
  }

  /* Check for transient hints
   *
   * According to the spec, the transient hint should be boolean.
   * But notify-send does not support hints of type 'boolean'.
   * So let's check for int and boolean until notify-send is fixed.
   */
  if ((dict_value = g_variant_lookup_value(hints, "transient",
                                           G_VARIANT_TYPE_BOOLEAN))) {
    n->transient = g_variant_get_boolean(dict_value);
    g_variant_unref(dict_value);
  } else if ((dict_value = g_variant_lookup_value(hints, "transient",
                                                  G_VARIANT_TYPE_UINT32))) {
    n->transient = g_variant_get_uint32(dict_value) > 0;
    g_variant_unref(dict_value);
  } else if ((dict_value = g_variant_lookup_value(hints, "transient",
                                                  G_VARIANT_TYPE_INT32))) {
    n->transient = g_variant_get_int32(dict_value) > 0;
    g_variant_unref(dict_value);
  }

        dict_value = g_variant_lookup_value(hints, "image-path", G_VARIANT_TYPE_STRING);
        if (!dict_value)
                dict_value = g_variant_lookup_value(hints, "image_path", G_VARIANT_TYPE_STRING);

        if (dict_value) {
                g_free(n->iconname);
                n->iconname = g_variant_dup_string(dict_value, NULL);
                g_variant_unref(dict_value);
        }

  // Set raw icon data only after initializing the notification, so the
  // desired icon size is known. This way the buffer can be immediately
  // rescaled. If at some point you might want to match by if a
  // notificaton has an image, this has to be reworked.
  dict_value =
      g_variant_lookup_value(hints, "image-data", G_VARIANT_TYPE("(iiibiiay)"));
  if (!dict_value)
    dict_value = g_variant_lookup_value(hints, "image_data",
                                        G_VARIANT_TYPE("(iiibiiay)"));
  if (!dict_value)
    dict_value = g_variant_lookup_value(hints, "icon_data",
                                        G_VARIANT_TYPE("(iiibiiay)"));
  if (dict_value) {
    // Signal that the notification is still waiting for a raw
    // icon. It cannot be set now, because min_icon_size and
    // max_icon_size aren't known yet. It cannot be set later,
    // because it has to be overwritten by the new_icon rule.
    n->receiving_raw_icon = true;
    icon_value = dict_value;
    dict_value = NULL;
  }

  // Set the dbus timeout
  if (timeout >= 0)
    n->dbus_timeout = ((gint64)timeout) * 1000;

  // All attributes that have to be set before initializations are set,
  // so we can initialize the notification. This applies all rules that
  // are defined and applies the formatting to the message.
  notification_init(n);

  if (icon_value) {
    if (n->receiving_raw_icon)
      notification_icon_replace_data(n, icon_value);
    g_variant_unref(icon_value);
  }

  // Modify these values after the notification is initialized and all rules are
  // applied.
  if ((dict_value =
           g_variant_lookup_value(hints, "fgcolor", G_VARIANT_TYPE_STRING))) {
    struct color c;
    if (string_parse_color(g_variant_get_string(dict_value, NULL), &c)) {
      notification_keep_original(n);
      if (!COLOR_VALID(n->original->fg))
        n->original->fg = n->colors.fg;
      n->colors.fg = c;
    }
    g_variant_unref(dict_value);
  }

  if ((dict_value =
           g_variant_lookup_value(hints, "bgcolor", G_VARIANT_TYPE_STRING))) {
    struct color c;
    if (string_parse_color(g_variant_get_string(dict_value, NULL), &c)) {
      notification_keep_original(n);
      if (!COLOR_VALID(n->original->bg))
        n->original->bg = n->colors.bg;
      n->colors.bg = c;
    }
    g_variant_unref(dict_value);
  }

  if ((dict_value =
           g_variant_lookup_value(hints, "frcolor", G_VARIANT_TYPE_STRING))) {
    struct color c;
    if (string_parse_color(g_variant_get_string(dict_value, NULL), &c)) {
      notification_keep_original(n);
      if (!COLOR_VALID(n->original->fc))
        n->original->fc = n->colors.frame;
      n->colors.frame = c;
    }
    g_variant_unref(dict_value);
  }

  if ((dict_value = g_variant_lookup_value(hints, "hlcolor",
                                           G_VARIANT_TYPE_STRING_ARRAY))) {
    char **cols = (char **)g_variant_get_strv(dict_value, NULL);
    size_t length = g_strv_length(cols);
    struct gradient *grad = gradient_alloc(length);

                for (size_t i = 0; i < length; i++) {
                        if (!string_parse_color(cols[i], &grad->colors[i])) {
                                g_free(grad);
                                goto end;
                        }
                }

    gradient_pattern(grad);

    notification_keep_original(n);
    if (!GRADIENT_VALID(n->original->highlight))
      n->original->highlight = gradient_acquire(n->colors.highlight);
    gradient_release(n->colors.highlight);
    n->colors.highlight = gradient_acquire(grad);

  end:
    g_variant_unref(dict_value);
  } else if ((dict_value = g_variant_lookup_value(hints, "hlcolor",
                                                  G_VARIANT_TYPE_STRING))) {
    struct color c;
    if (string_parse_color(g_variant_get_string(dict_value, NULL), &c)) {
      struct gradient *grad = gradient_alloc(1);
      grad->colors[0] = c;
      gradient_pattern(grad);

      notification_keep_original(n);
      if (!GRADIENT_VALID(n->original->highlight))
        n->original->highlight = gradient_acquire(n->colors.highlight);
      gradient_release(n->colors.highlight);
      n->colors.highlight = gradient_acquire(grad);
    }
    g_variant_unref(dict_value);
  }

  g_variant_unref(hints);
  g_variant_type_free(required_type);
  g_free(actions); // the strv is only a shallow copy

  return n;
}

void signal_length_propertieschanged(void) {
  static unsigned int last_displayed = 0;
  static unsigned int last_history = 0;
  static unsigned int last_waiting = 0;

  if (!dbus_conn)
    return;

  GVariantBuilder *builder = g_variant_builder_new(G_VARIANT_TYPE_VARDICT);
  GVariantBuilder *invalidated_builder =
      g_variant_builder_new(G_VARIANT_TYPE_STRING_ARRAY);

  unsigned int displayed = queues_length_displayed();
  unsigned int history = queues_length_history();
  unsigned int waiting = queues_length_waiting();
  bool properties_changed = false;

  if (last_displayed != displayed) {
    g_variant_builder_add(builder, "{sv}", "displayedLength",
                          g_variant_new_uint32(displayed));
    last_displayed = displayed;
    properties_changed = true;
  }

  if (last_history != history) {
    g_variant_builder_add(builder, "{sv}", "historyLength",
                          g_variant_new_uint32(history));
    last_history = history;
    properties_changed = true;
  }
  if (last_waiting != waiting) {
    g_variant_builder_add(builder, "{sv}", "waitingLength",
                          g_variant_new_uint32(waiting));
    last_waiting = waiting;
    properties_changed = true;
  }

  if (properties_changed) {
    GVariant *body =
        g_variant_new("(sa{sv}as)", DUNST_IFAC, builder, invalidated_builder);

    GError *err = NULL;

    g_dbus_connection_emit_signal(dbus_conn, NULL, FDN_PATH, PROPERTIES_IFAC,
                                  "PropertiesChanged", body, &err);

    if (err) {
      LOG_W("Unable to emit signal: %s", err->message);
      g_error_free(err);
    }
  }

  g_clear_pointer(&builder, g_variant_builder_unref);
  g_clear_pointer(&invalidated_builder, g_variant_builder_unref);
}

static void dbus_cb_Notify(GDBusConnection *connection, const gchar *sender,
                           GVariant *parameters,
                           GDBusMethodInvocation *invocation) {
  struct notification *n = dbus_message_to_notification(sender, parameters);
  if (!n) {
    LOG_W("A notification failed to decode.");
    g_dbus_method_invocation_return_dbus_error(invocation, FDN_IFAC ".Error",
                                               "Cannot decode notification!");
    return;
  }

  int id = queues_notification_insert(n);

  GVariant *reply = g_variant_new("(u)", id);
  g_dbus_method_invocation_return_value(invocation, reply);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);

  // The message got discarded
  if (id == 0) {
    signal_notification_closed(n, REASON_USER);
    notification_unref(n);
  }

  wake_up();
}

static void dbus_cb_CloseNotification(GDBusConnection *connection,
                                      const gchar *sender, GVariant *parameters,
                                      GDBusMethodInvocation *invocation) {
  guint32 id;
  g_variant_get(parameters, "(u)", &id);
  if (settings.ignore_dbusclose) {
    LOG_D("Ignoring CloseNotification message");
    // Stay commpliant by lying to the sender,  telling him we closed the
    // notification
    if (id > 0) {
      struct notification *n = queues_get_by_id(id);
      if (n)
        signal_notification_closed(n, REASON_SIG);
    }
  } else {
    queues_notification_close_id(id, REASON_SIG);
  }
  wake_up();
  g_dbus_method_invocation_return_value(invocation, NULL);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

static void dbus_cb_GetServerInformation(GDBusConnection *connection,
                                         const gchar *sender,
                                         GVariant *parameters,
                                         GDBusMethodInvocation *invocation) {
  GVariant *answer =
      g_variant_new("(ssss)", "dunst", "knopwob", VERSION, "1.2");

  g_dbus_method_invocation_return_value(invocation, answer);
  g_dbus_connection_flush(connection, NULL, NULL, NULL);
}

void signal_notification_closed(struct notification *n, enum reason reason) {
  if (!n->dbus_valid) {
    return;
  }

  if (reason < REASON_MIN || REASON_MAX < reason) {
    LOG_W("Closing notification with reason '%d' not supported. "
          "Closing it with reason '%d'.",
          reason, REASON_UNDEF);
    reason = REASON_UNDEF;
  }

  if (!dbus_conn) {
    LOG_E("Unable to close notification: No DBus connection.");
  }

  GVariant *body = g_variant_new("(uu)", n->id, reason);
  GError *err = NULL;

        g_dbus_connection_emit_signal(dbus_conn,
                                      n->dbus_client,
                                      FDN_PATH,
                                      FDN_IFAC,
                                      "NotificationClosed",
                                      body,
                                      &err);

  notification_invalidate_actions(n);

  n->dbus_valid = false;

  if (err) {
    LOG_W("Unable to close notification: %s", err->message);
    g_error_free(err);
  } else {
    char *reason_string;
    switch (reason) {
    case REASON_TIME:
      reason_string = "time";
      break;
    case REASON_USER:
      reason_string = "user";
      break;
    case REASON_SIG:
      reason_string = "signal";
      break;
    case REASON_UNDEF:
      reason_string = "undfined";
      break;
    default:
      reason_string = "unknown";
    }

    LOG_D("Queues: Closing notification for reason: %s", reason_string);
  }
}

void signal_action_invoked(const struct notification *n,
                           const char *identifier) {
  if (!n->dbus_valid) {
    LOG_W("Invoking action '%s' not supported. "
          "Notification already closed.",
          identifier);
    return;
  }

  GVariant *body = g_variant_new("(us)", n->id, identifier);
  GError *err = NULL;

  g_dbus_connection_emit_signal(dbus_conn, n->dbus_client, FDN_PATH, FDN_IFAC,
                                "ActionInvoked", body, &err);

  if (err) {
    LOG_W("Unable to invoke action: %s", err->message);
    g_error_free(err);
  }
}

GVariant *dbus_cb_dunst_Properties_Get(GDBusConnection *connection,
                                       const gchar *sender,
                                       const gchar *object_path,
                                       const gchar *interface_name,
                                       const gchar *property_name,
                                       GError **error, gpointer user_data) {
  struct dunst_status status = dunst_status_get();

  if (STR_EQ(property_name, "paused")) {
    return g_variant_new_boolean(status.pause_level != 0);
  } else if (STR_EQ(property_name, "pauseLevel")) {
    return g_variant_new_uint32(status.pause_level);
  } else if (STR_EQ(property_name, "displayedLength")) {
    unsigned int displayed = queues_length_displayed();
    return g_variant_new_uint32(displayed);
  } else if (STR_EQ(property_name, "historyLength")) {
    unsigned int history = queues_length_history();
    return g_variant_new_uint32(history);
  } else if (STR_EQ(property_name, "waitingLength")) {
    unsigned int waiting = queues_length_waiting();
    return g_variant_new_uint32(waiting);
  } else {
    LOG_W("Unknown property!\n");
    *error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                         "Unknown property");
    return NULL;
  }
}

gboolean dbus_cb_dunst_Properties_Set(
    GDBusConnection *connection, const gchar *sender, const gchar *object_path,
    const gchar *interface_name, const gchar *property_name, GVariant *value,
    GError **error, gpointer user_data) {
  int targetPauseLevel = -1;
  if (STR_EQ(property_name, "paused")) {
    if (g_variant_get_boolean(value)) {
      targetPauseLevel = MAX_PAUSE_LEVEL;
    } else {
      targetPauseLevel = 0;
    }
  } else if STR_EQ (property_name, "pauseLevel") {
    targetPauseLevel = g_variant_get_uint32(value);
    if (targetPauseLevel > MAX_PAUSE_LEVEL) {
      targetPauseLevel = MAX_PAUSE_LEVEL;
    }
  }

  if (targetPauseLevel >= 0) {
    dunst_status_int(S_PAUSE_LEVEL, targetPauseLevel);
    wake_up();

    GVariantBuilder *builder = g_variant_builder_new(G_VARIANT_TYPE_VARDICT);
    GVariantBuilder *invalidated_builder =
        g_variant_builder_new(G_VARIANT_TYPE_STRING_ARRAY);
    g_variant_builder_add(builder, "{sv}", "paused",
                          g_variant_new_boolean(targetPauseLevel != 0));
    g_variant_builder_add(builder, "{sv}", "pauseLevel",
                          g_variant_new_uint32(targetPauseLevel));
    g_dbus_connection_emit_signal(connection, NULL, object_path,
                                  PROPERTIES_IFAC, "PropertiesChanged",
                                  g_variant_new("(sa{sv}as)", interface_name,
                                                builder, invalidated_builder),
                                  NULL);

    g_clear_pointer(&builder, g_variant_builder_unref);
    g_clear_pointer(&invalidated_builder, g_variant_builder_unref);
    return true;
  }

  *error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                       "Unknown property");

  return false;
}

static const GDBusInterfaceVTable interface_vtable_fdn = {
    dbus_cb_fdn_methods,
};

static const GDBusInterfaceVTable interface_vtable_dunst = {
    dbus_cb_dunst_methods,
    dbus_cb_dunst_Properties_Get,
    dbus_cb_dunst_Properties_Set,
};

static void dbus_cb_bus_acquired(GDBusConnection *connection, const gchar *name,
                                 gpointer user_data) {
  GError *err = NULL;
  if (!g_dbus_connection_register_object(
          connection, FDN_PATH, introspection_data->interfaces[0],
          &interface_vtable_fdn, NULL, NULL, &err)) {
    DIE("Unable to register dbus connection interface '%s': %s",
        introspection_data->interfaces[0]->name, err->message);
  }

  if (!g_dbus_connection_register_object(
          connection, FDN_PATH, introspection_data->interfaces[1],
          &interface_vtable_dunst, NULL, NULL, &err)) {
    DIE("Unable to register dbus connection interface '%s': %s",
        introspection_data->interfaces[1]->name, err->message);
  }
}

static void dbus_cb_name_acquired(GDBusConnection *connection,
                                  const gchar *name, gpointer user_data) {
  // If we're not able to get org.fd.N bus, we've still got a problem
  if (STR_EQ(name, FDN_NAME))
    dbus_conn = connection;
}

/**
 * Get the PID of the current process, which acquired FDN DBus Name.
 *
 * If name or vendor specified, the name and vendor
 * will get additionally get via the FDN GetServerInformation method
 *
 * @param connection The DBus connection
 * @param pid The place to report the PID to
 * @param name The place to report the name to, if not required set to NULL
 * @param vendor The place to report the vendor to, if not required set to NULL
 *
 * @retval true: on success
 * @retval false: Any error happened
 */
static bool dbus_get_fdn_daemon_info(GDBusConnection *connection, guint *pid,
                                     char **name, char **vendor) {
  ASSERT_OR_RET(pid, false);
  ASSERT_OR_RET(connection, false);

  char *owner = NULL;
  GError *error = NULL;

  GDBusProxy *proxy_fdn;
  GDBusProxy *proxy_dbus;

  proxy_fdn = g_dbus_proxy_new_sync(
      connection,
      /* do not trigger a start of the notification daemon */
      G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, /* info */
      FDN_NAME, FDN_PATH, FDN_IFAC, NULL,         /* cancelable */
      &error);

  if (error) {
    g_error_free(error);
    return false;
  }

  GVariant *daemoninfo = NULL;
  if (name || vendor) {
    daemoninfo =
        g_dbus_proxy_call_sync(proxy_fdn, FDN_IFAC ".GetServerInformation",
                               NULL, G_DBUS_CALL_FLAGS_NONE,
                               /* It's not worth to wait for the info
                                * longer than half a second when dying */
                               500, NULL, /* cancelable */
                               &error);
  }

  if (error) {
    /* Ignore the error, we may still be able to retrieve the PID */
    g_clear_pointer(&error, g_error_free);
  } else {
    g_variant_get(daemoninfo, "(ssss)", name, vendor, NULL, NULL);
  }

  owner = g_dbus_proxy_get_name_owner(proxy_fdn);

  proxy_dbus = g_dbus_proxy_new_sync(
      connection, G_DBUS_PROXY_FLAGS_NONE, NULL, /* info */
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      NULL, /* cancelable */
      &error);

  if (error) {
    g_error_free(error);
    return false;
  }

  GVariant *pidinfo = g_dbus_proxy_call_sync(
      proxy_dbus, "org.freedesktop.DBus.GetConnectionUnixProcessID",
      g_variant_new("(s)", owner), G_DBUS_CALL_FLAGS_NONE,
      /* It's not worth to wait for the PID
       * longer than half a second when dying */
      500, NULL, &error);

  if (error) {
    g_error_free(error);
    return false;
  }

  g_object_unref(proxy_fdn);
  g_object_unref(proxy_dbus);
  g_free(owner);
  if (daemoninfo)
    g_variant_unref(daemoninfo);

  if (pidinfo) {
    g_variant_get(pidinfo, "(u)", pid);
    g_variant_unref(pidinfo);
    return true;
  } else {
    return false;
  }
}

static void dbus_cb_name_lost(GDBusConnection *connection, const gchar *name,
                              gpointer user_data) {
  if (connection) {
    char *name;
    unsigned int pid;
    if (dbus_get_fdn_daemon_info(connection, &pid, &name, NULL)) {
      DIE("Cannot acquire '" FDN_NAME "': "
          "Name is acquired by '%s' with PID '%d'.",
          name, pid);
    } else {
      DIE("Cannot acquire '" FDN_NAME "'.");
    }
  } else {
    const char *env = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (STR_EMPTY(env)) {
      LOG_W("DBUS_SESSION_BUS_ADDRESS is blank. Is the dbus session configured "
            "correctly?");
    }

    DIE("Cannot connect to DBus.");
  }
  exit(1);
}

int dbus_init(void) {
  guint owner_id;

  introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, NULL);

  owner_id =
      g_bus_own_name(G_BUS_TYPE_SESSION, FDN_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                     dbus_cb_bus_acquired, dbus_cb_name_acquired,
                     dbus_cb_name_lost, NULL, NULL);

  return owner_id;
}

void dbus_teardown(int owner_id) {
  g_clear_pointer(&introspection_data, g_dbus_node_info_unref);

  g_bus_unown_name(owner_id);
  dbus_conn = NULL;
}

/* vim: set ft=c tabstop=8 shiftwidth=8 expandtab textwidth=0: */
