/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with main.c; if not, write to:
 *            The Free Software Foundation, Inc.,
 *            59 Temple Place - Suite 330,
 *            Boston,  MA  02111-1307, USA.
 */
#include "config.h"
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "notify.h"
#include "notifynotification.h"

static void notify_notification_class_init (NotifyNotificationClass * klass);
static void notify_notification_init (NotifyNotification * sp);
static void notify_notification_finalize (GObject * object);
static void _close_signal_handler (DBusGProxy *proxy,
                                   guint32 id, 
                                   NotifyNotification *notification); 

struct NotifyNotificationPrivate
{
  guint32 id;
  gchar *summary;
  gchar *message;

  /*NULL to use icon data
     anything else to have server lookup icon */
  gchar *icon_name;

    /*-1 = use server default
       0 = never timeout
      >0 = Number of milliseconds before we timeout
    */
  gint timeout;

  GSList *actions;
  GHashTable *hints;

  GtkWidget *attached_widget;
  gint widget_old_x;
  gint widget_old_y;

  gpointer user_data;
  GDestroyNotify user_data_free_func;

  gboolean updates_pending;

  DBusGProxy *proxy;
};

typedef enum
{
  SIGNAL_TYPE_CLOSED, 
  LAST_SIGNAL
} NotifyNotificationSignalType;

typedef struct
{
  NotifyNotification *object;
} NotifyNotificationSignal;

static guint notify_notification_signals[LAST_SIGNAL] = { 0 }; 
static GObjectClass *parent_class = NULL;

GType
notify_notification_get_type ()
{
  static GType type = 0;

  if (type == 0)
    {
      static const GTypeInfo our_info = {
	sizeof (NotifyNotificationClass),
	NULL,
	NULL,
	(GClassInitFunc) notify_notification_class_init,
	NULL,
	NULL,
	sizeof (NotifyNotification),
	0,
	(GInstanceInitFunc) notify_notification_init,
      };

      type = g_type_register_static (G_TYPE_OBJECT,
				     "NotifyNotification", &our_info, 0);
    }

  return type;
}

static void
notify_notification_class_init (NotifyNotificationClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);
  object_class->finalize = notify_notification_finalize;

  /* Create signals here: */
     notify_notification_signals[SIGNAL_TYPE_CLOSED] = 
       g_signal_new ("closed",
                     G_TYPE_FROM_CLASS (object_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET (NotifyNotificationClass, closed),
                     NULL, NULL, 
                     g_cclosure_marshal_VOID__VOID,
                     G_TYPE_NONE,
                     0);
}

static void
_g_value_free (GValue * value)
{
  g_value_unset (value);
  g_free (value);
}

static void
notify_notification_init (NotifyNotification * obj)
{
  obj->priv = g_new0 (NotifyNotificationPrivate, 1);

  obj->priv->id = 0;

  obj->priv->summary = NULL;
  obj->priv->message = NULL;
  obj->priv->icon_name = NULL;
  obj->priv->timeout = NOTIFY_TIMEOUT_DEFAULT;

  obj->priv->actions = NULL;
  obj->priv->hints = g_hash_table_new_full (g_str_hash,
					    g_str_equal,
					    g_free,
					    (GDestroyNotify) _g_value_free);

  obj->priv->attached_widget = NULL;
  obj->priv->user_data = NULL;
  obj->priv->user_data_free_func = NULL;

  obj->priv->updates_pending = FALSE;

  obj->priv->widget_old_x = 0;
  obj->priv->widget_old_y = 0;

  obj->priv->proxy = NULL;
}

static void
notify_notification_finalize (GObject * object)
{
  NotifyNotification *obj;
  NotifyNotificationPrivate *priv;

  obj = NOTIFY_NOTIFICATION (object);
  priv = obj->priv;

  g_free (priv->summary);
  g_free (priv->message);
  g_free (priv->icon_name);

  if (priv->actions != NULL)
    {
      g_slist_foreach (priv->actions, (GFunc) g_free, NULL);
      g_slist_free (priv->actions);
    }

  if (priv->hints != NULL)
    g_hash_table_destroy (priv->hints);

  if (priv->attached_widget != NULL)
    gtk_widget_unref (priv->attached_widget);

  if (priv->user_data_free_func != NULL)
    priv->user_data_free_func (priv->user_data);

  dbus_g_proxy_disconnect_signal (priv->proxy, "NotificationClosed", 
                                  _close_signal_handler, 
                                  object);

  g_free (obj->priv);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
_notify_notification_update_applet_hints (NotifyNotification * n)
{
  NotifyNotificationPrivate *priv;
  gboolean update_pending;

  update_pending = FALSE;
  priv = n->priv;

  if (priv->attached_widget != NULL)
    {
      gint x, y, h, w;
      GtkWidget *widget;
      GtkRequisition requisition;

      widget = priv->attached_widget;

      gtk_widget_size_request (widget, &requisition);
      w = requisition.width;
      h = requisition.height;

      gdk_window_get_origin (widget->window, &x, &y);
      if (GTK_WIDGET_NO_WINDOW (widget))
	{
	  x += widget->allocation.x;
	  y += widget->allocation.y;
	}

      x += widget->allocation.width / 2;
      y += widget->allocation.height / 2;

      if (x != priv->widget_old_x)
	{
	  notify_notification_set_hint_int32 (n, "x", x);
	  priv->widget_old_x = x;

	  update_pending = TRUE;
	}

      if (y != priv->widget_old_y)
	{
	  notify_notification_set_hint_int32 (n, "y", y);
	  priv->widget_old_y = y;

	  update_pending = TRUE;
	}
    }

  return update_pending;
}

#if 0

/* This is left here just incase we revisit autoupdating
   One thought would be to check for updates every time the icon
   is redrawn but we still have to deal with the race conditions
   that could occure between the server and the client so we will
   leave this alone for now.
*/
static gboolean
_idle_check_updates (void *user_data)
{
  NotifyNotification *n;
  NotifyNotificationPrivate *priv;

  n = NOTIFY_NOTIFICATION (user_data);
  priv = n->priv;

  if (priv->is_visible)
    {
      priv->updates_pending = _notify_notification_update_applet_hints (n);
      if (priv->updates_pending)
	{
	  /* Try again if we fail on next idle */
	  priv->updates_pending = !notify_notification_show (n, NULL);
	}
    }
  else
    {
      priv->updates_pending = FALSE;
    }

  n = NOTIFY_NOTIFICATION (user_data);
  priv = n->priv;

  return TRUE;
}
#endif

GdkFilterReturn
_catch (GdkXEvent * xevent, GdkEvent * event, gpointer data)
{
  static int i = 1;
  printf ("here, %i\n", i);
  i++;
  return GDK_FILTER_CONTINUE;
}

NotifyNotification *
notify_notification_new (const gchar * summary,
			 const gchar * message,
			 const gchar * icon, GtkWidget * attach)
{
  NotifyNotification *obj;

  g_assert (summary != NULL);
  g_assert (message != NULL);

  obj = NOTIFY_NOTIFICATION (g_object_new (NOTIFY_TYPE_NOTIFICATION, NULL));

  obj->priv->summary = g_strdup (summary);
  obj->priv->message = g_strdup (message);
  obj->priv->icon_name = g_strdup (icon);

  if (attach != NULL)
    {
      gtk_widget_ref (attach);
      obj->priv->attached_widget = attach;
    }

  return obj;
}

gboolean
notify_notification_update (NotifyNotification * notification,
			    const gchar * summary,
			    const gchar * message, const gchar * icon)
{
  NotifyNotificationPrivate *priv;

  priv = notification->priv;
  g_free (priv->summary);
  g_free (priv->message);
  g_free (priv->icon_name);

  priv->summary = g_strdup (summary);
  priv->message = g_strdup (message);
  priv->icon_name = g_strdup (icon);

  priv->updates_pending = TRUE;

  /*TODO: return false on OOM */
  return TRUE;
}

void
notify_notification_attach_to_widget (NotifyNotification * notification,
				      GtkWidget * attach)
{
  NotifyNotificationPrivate *priv;

  priv = notification->priv;

  if (priv->attached_widget != NULL)
    gtk_widget_unref (priv->attached_widget);

  if (attach != NULL)
    priv->attached_widget = gtk_widget_ref (attach);
  else
    priv->attached_widget = NULL;

}

gboolean
notify_notification_set_user_data (NotifyNotification * notification,
				   void *user_data, GFreeFunc free_func)
{
  NotifyNotificationPrivate *priv;

  priv = notification->priv;

  if (priv->user_data)
    if (priv->user_data_free_func)
      priv->user_data_free_func (priv->user_data);

  priv->user_data = user_data;
  priv->user_data_free_func = priv->user_data;

  /* TODO: return FALSE on OOM */
  return TRUE;
}

static DBusHandlerResult
filter_func (DBusConnection     *connection,
             DBusMessage        *message,
             void               *user_data)
{
  g_message ("member %s", dbus_message_get_member (message));

  return DBUS_HANDLER_RESULT_HANDLED;
}

static void 
_close_signal_handler (DBusGProxy *proxy, 
                       guint32 id, 
                       NotifyNotification *notification) 
{
  printf ("Got the NotificationClosed signal (id = %i, notification->id = %i)\n"
, id, notification->priv->id);

  if (id == notification->priv->id)
      g_signal_emit (notification, 
                     notify_notification_signals[SIGNAL_TYPE_CLOSED], 
                     0);
}

gboolean
notify_notification_show (NotifyNotification *notification, GError **error)
{
  NotifyNotificationPrivate *priv;
  GError *tmp_error;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);


  priv = notification->priv;

  tmp_error = NULL;

  if (priv->proxy == NULL)
    {
      DBusGConnection *bus;
      bus = dbus_g_bus_get (DBUS_BUS_SESSION, &tmp_error);
      if (tmp_error != NULL)
	{
	  g_propagate_error (error, tmp_error);
	  return FALSE;
	}

      priv->proxy = dbus_g_proxy_new_for_name (bus,
					       NOTIFY_DBUS_NAME,
					       NOTIFY_DBUS_CORE_OBJECT,
					       NOTIFY_DBUS_CORE_INTERFACE);

      dbus_g_proxy_add_signal (priv->proxy, "NotificationClosed",
                               G_TYPE_UINT, NULL);
      dbus_g_proxy_connect_signal (priv->proxy, "NotificationClosed", 
                               _close_signal_handler, 
                               notification, NULL);

      dbus_g_connection_unref (bus);
    }

  /*if attached to a widget modify x and y in hints */
  _notify_notification_update_applet_hints (notification);

  /*TODO: make this nonblocking */
  dbus_g_proxy_call (priv->proxy, "Notify", &tmp_error,
		     G_TYPE_STRING, _notify_get_app_name (),
		     G_TYPE_STRING,
		     (priv->icon_name != NULL) ? priv->icon_name : "",
		     G_TYPE_UINT, priv->id, G_TYPE_STRING, priv->summary,
		     G_TYPE_STRING, priv->message,
		     dbus_g_type_get_collection ("GSList", G_TYPE_STRING),
		     priv->actions, dbus_g_type_get_map ("GHashTable",
							 G_TYPE_STRING,
							 G_TYPE_VALUE),
		     priv->hints, G_TYPE_INT, priv->timeout, G_TYPE_INVALID,
		     G_TYPE_UINT, &priv->id, G_TYPE_INVALID);

  if (tmp_error != NULL)
    {
      g_propagate_error (error, tmp_error);
      return FALSE;
    }

  return TRUE;
}

void
notify_notification_set_timeout (NotifyNotification * notification,
				 gint timeout)
{
  notification->priv->timeout = timeout;
}

gboolean
notify_notification_set_category (NotifyNotification * notification,
				  const char *category)
{
  return notify_notification_set_hint_string (notification,
					      "category", category);
}

gboolean
notify_notification_set_urgency (NotifyNotification * notification,
				 NotifyUrgency l)
{
  return notify_notification_set_hint_byte (notification,
					    "urgency", (guchar) l);
}

static gboolean
_gvalue_array_append_int (GValueArray * array, gint i)
{
  GValue *value;

  value = g_new0 (GValue, 1);
  if (!value)
    return FALSE;

  g_value_init (value, G_TYPE_INT);
  g_value_set_int (value, i);

  g_value_array_append (array, value);

  return TRUE;
}

static gboolean
_gvalue_array_append_bool (GValueArray * array, gboolean b)
{
  GValue *value;

  value = g_new0 (GValue, 1);
  if (!value)
    return FALSE;

  g_value_init (value, G_TYPE_BOOLEAN);
  g_value_set_boolean (value, b);

  g_value_array_append (array, value);

  return TRUE;
}

static gboolean
_gvalue_array_append_byte_array (GValueArray * array,
				 guchar * bytes, gsize len)
{
  GArray *byte_array;
  GValue *value;

  byte_array = g_array_sized_new (FALSE, FALSE, sizeof (guchar), len);
  if (!byte_array)
    return FALSE;

  byte_array = g_array_append_vals (byte_array, bytes, len);

  value = g_new0 (GValue, 1);
  if (!value)
    {
      g_array_free (byte_array, TRUE);
      return FALSE;
    }

  g_value_init (value, dbus_g_type_get_collection ("GArray", G_TYPE_UCHAR));
  g_value_set_boxed_take_ownership (value, byte_array);

  g_value_array_append (array, value);

  return TRUE;
}



gboolean
notify_notification_set_icon_data_from_pixbuf (NotifyNotification *
					       notification, GdkPixbuf * icon)
{
  gint width;
  gint height;
  gint rowstride;
  gboolean alpha;
  gint bits_per_sample;
  gint n_channels;
  guchar *image;
  gsize image_len;
  gchar *key_dup;

  GValueArray *image_struct;
  GValue *value;
  NotifyNotificationPrivate *priv;

  priv = notification->priv;

  width = gdk_pixbuf_get_width (icon);
  height = gdk_pixbuf_get_height (icon);
  rowstride = gdk_pixbuf_get_rowstride (icon);
  n_channels = gdk_pixbuf_get_n_channels (icon);
  bits_per_sample = gdk_pixbuf_get_bits_per_sample (icon);
  alpha = gdk_pixbuf_get_has_alpha (icon);
  image_len =
    (height - 1) * rowstride +
    width * ((n_channels * bits_per_sample + 7) / 8);

  image = gdk_pixbuf_get_pixels (icon);

  image_struct = g_value_array_new (8);

  if (!image_struct)
    goto fail;

  _gvalue_array_append_int (image_struct, width);
  _gvalue_array_append_int (image_struct, height);
  _gvalue_array_append_int (image_struct, rowstride);
  _gvalue_array_append_bool (image_struct, alpha);
  _gvalue_array_append_int (image_struct, bits_per_sample);
  _gvalue_array_append_int (image_struct, n_channels);
  _gvalue_array_append_byte_array (image_struct, image, image_len);

  value = g_new0 (GValue, 1);
  if (!value)
    goto fail;

  g_value_init (value, G_TYPE_VALUE_ARRAY);
  g_value_set_boxed (value, image_struct);

  key_dup = g_strdup ("icon_data");
  if (!key_dup)
    goto fail;

  g_hash_table_insert (priv->hints, key_dup, value);

  return TRUE;

fail:
  if (image_struct != NULL)
    g_value_array_free (image_struct);
  return FALSE;
}

gboolean
notify_notification_set_hint_int32 (NotifyNotification * notification,
				    const gchar * key, gint value)
{
  NotifyNotificationPrivate *priv;
  GValue *hint_value;
  gchar *key_dup;

  priv = notification->priv;

  hint_value = g_new0 (GValue, 1);
  g_value_init (hint_value, G_TYPE_INT);
  g_value_set_int (hint_value, value);

  key_dup = g_strdup (key);

  g_hash_table_insert (priv->hints, key_dup, hint_value);

  /* TODO: return FALSE on OOM */
  return TRUE;
}

gboolean
notify_notification_set_hint_double (NotifyNotification * notification,
				     const gchar * key, gdouble value)
{
  NotifyNotificationPrivate *priv;
  GValue *hint_value;
  gchar *key_dup;

  priv = notification->priv;

  hint_value = g_new0 (GValue, 1);
  g_value_init (hint_value, G_TYPE_FLOAT);
  g_value_set_float (hint_value, value);

  key_dup = g_strdup (key);

  g_hash_table_insert (priv->hints, key_dup, hint_value);

  /* TODO: return FALSE on OOM */
  return TRUE;
}

gboolean
notify_notification_set_hint_byte (NotifyNotification * notification,
				   const gchar * key, guchar value)
{
  NotifyNotificationPrivate *priv;
  GValue *hint_value;
  gchar *key_dup;

  priv = notification->priv;

  hint_value = g_new0 (GValue, 1);
  g_value_init (hint_value, G_TYPE_UCHAR);
  g_value_set_uchar (hint_value, value);

  key_dup = g_strdup (key);

  g_hash_table_insert (priv->hints, key_dup, hint_value);

  /* TODO: return FALSE on OOM */
  return TRUE;
}

gboolean
notify_notification_set_hint_byte_array (NotifyNotification * notification,
					 const gchar * key,
					 const guchar * value, gsize len)
{
  NotifyNotificationPrivate *priv;
  GValue *hint_value;
  gchar *key_dup;
  GArray *byte_array;

  priv = notification->priv;

  byte_array = g_array_sized_new (FALSE, FALSE, sizeof (guchar), len);
  byte_array = g_array_append_vals (byte_array, value, len);

  hint_value = g_new0 (GValue, 1);
  g_value_init (hint_value,
		dbus_g_type_get_collection ("GArray", G_TYPE_UCHAR));
  g_value_set_boxed_take_ownership (hint_value, byte_array);
  key_dup = g_strdup (key);

  g_hash_table_insert (priv->hints, key_dup, hint_value);

  /* TODO: return FALSE on OOM */
  return TRUE;
}


gboolean
notify_notification_set_hint_string (NotifyNotification * notification,
				     const gchar * key, const gchar * value)
{
  NotifyNotificationPrivate *priv;
  GValue *hint_value;
  gchar *key_dup;

  priv = notification->priv;

  hint_value = g_new0 (GValue, 1);
  g_value_init (hint_value, G_TYPE_STRING);
  g_value_set_string (hint_value, value);

  key_dup = g_strdup (key);

  g_hash_table_insert (priv->hints, key_dup, hint_value);

  /* TODO: return FALSE on OOM */
  return TRUE;
}

static gboolean
_remove_all (void)
{
  return TRUE;
}

void 
notify_notification_clear_hints (NotifyNotification *notification)
{
  g_hash_table_foreach_remove (notification->priv->hints,
                               (GHRFunc) _remove_all, NULL);
}

gboolean
notify_notification_add_action (NotifyNotification *notification,
				const char *action,
				const char *label,
				NotifyActionCallback callback)
{
  /* TODO: implement actions which will also set up a dbus listener 
     for those actions
   */
  return FALSE;
}

gboolean
notify_notification_close (NotifyNotification * notification, GError ** error)
{
  NotifyNotificationPrivate *priv;
  GError *tmp_error;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  priv = notification->priv;

  tmp_error = NULL;

  if (priv->proxy == NULL)
    {
      DBusGConnection *bus;

      bus = dbus_g_bus_get (DBUS_BUS_SESSION, &tmp_error);
      if (tmp_error != NULL)
	{
	  g_propagate_error (error, tmp_error);
	  return FALSE;
	}

      priv->proxy = dbus_g_proxy_new_for_name (bus,
					       NOTIFY_DBUS_NAME,
					       NOTIFY_DBUS_CORE_OBJECT,
					       NOTIFY_DBUS_CORE_INTERFACE);
      dbus_g_connection_unref (bus);
    }

  dbus_g_proxy_call (priv->proxy, "CloseNotification", &tmp_error,
		     G_TYPE_UINT, priv->id, G_TYPE_INVALID, G_TYPE_INVALID);

  if (tmp_error != NULL)
    {
      g_propagate_error (error, tmp_error);
      return FALSE;
    }

  return TRUE;
}
