/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2004 Christian Hammond.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA  02111-1307, USA.
 */

#include "config.h"

#include <libnotify/notify.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gprintf.h>

#define N_(x) (x)
#define GETTEXT_PACKAGE NULL

static NotifyUrgency urgency = NOTIFY_URGENCY_NORMAL;
static GMainLoop *loop;

static gboolean
g_option_arg_urgency_cb (const char *option_name,
                         const char *value,
                         gpointer    data,
                         GError    **error)
{
        if (value != NULL) {
                if (!strcasecmp (value, "low"))
                        urgency = NOTIFY_URGENCY_LOW;
                else if (!strcasecmp (value, "normal"))
                        urgency = NOTIFY_URGENCY_NORMAL;
                else if (!strcasecmp (value, "critical"))
                        urgency = NOTIFY_URGENCY_CRITICAL;
                else {
                        *error = g_error_new (G_OPTION_ERROR,
                                              G_OPTION_ERROR_BAD_VALUE,
                                              N_("Unknown urgency %s specified. "
                                                 "Known urgency levels: low, "
                                                 "normal, critical."), value);

                        return FALSE;
                }
        }

        return TRUE;
}

static gboolean
notify_notification_set_hint_variant (NotifyNotification *notification,
                                      const char         *type,
                                      const char         *key,
                                      const char         *value,
                                      GError            **error)
{
        static gboolean conv_error = FALSE;
        if (!strcasecmp (type, "string")) {
                notify_notification_set_hint_string (notification,
                                                     key,
                                                     value);
        } else if (!strcasecmp (type, "int")) {
                if (!g_ascii_isdigit (*value))
                        conv_error = TRUE;
                else {
                        gint h_int = (gint) g_ascii_strtoull (value, NULL, 10);
                        notify_notification_set_hint_int32 (notification,
                                                            key,
                                                            h_int);
                }
        } else if (!strcasecmp (type, "double")) {
                if (!g_ascii_isdigit (*value))
                        conv_error = TRUE;
                else {
                        gdouble h_double = g_strtod (value, NULL);
                        notify_notification_set_hint_double (notification,
                                                             key, h_double);
                }
        } else if (!strcasecmp (type, "byte")) {
                gint h_byte = (gint) g_ascii_strtoull (value, NULL, 10);

                if (h_byte < 0 || h_byte > 0xFF)
                        conv_error = TRUE;
                else {
                        notify_notification_set_hint_byte (notification,
                                                           key,
                                                           (guchar) h_byte);
                }
        } else {
                *error = g_error_new (G_OPTION_ERROR,
                                      G_OPTION_ERROR_BAD_VALUE,
                                      N_("Invalid hint type \"%s\". Valid types "
                                         "are int, double, string and byte."),
                                      type);
                return FALSE;
        }

        if (conv_error) {
                *error = g_error_new (G_OPTION_ERROR,
                                      G_OPTION_ERROR_BAD_VALUE,
                                      N_("Value \"%s\" of hint \"%s\" could not be "
                                         "parsed as type \"%s\"."), value, key,
                                      type);
                return FALSE;
        }

        return TRUE;
}

static void
_handle_action (NotifyNotification * notify, 
                char *action,
                gpointer user_data)
{
        if (user_data != NULL) {
                // Use the exact action name from user input
                char *s = g_strdup(user_data);
                printf ("%s\n", s);
                g_free(s);
        }
        notify_notification_close (notify, NULL);
        g_main_loop_quit (loop);
}

static void
_handle_closed (NotifyNotification * notify, 
                gpointer user_data)
{
        g_main_loop_quit (loop);
}

static void
_handle_timeout (gpointer data)
{
        // g_message ("Quitting due to local timeout");
        g_main_loop_quit (loop);
}

int
main (int argc, char *argv[])
{
        static const char  *summary = NULL;
        char               *body;
        static const char  *type = NULL;
        static char        *app_name = NULL;
        static char        *icon_str = NULL;
        static char        *icons = NULL;
        static char       **n_text = NULL;
        static char       **hints = NULL;
        static char       **actions = NULL;
        static gboolean     do_version = FALSE;
        static gboolean     hint_error = FALSE;
        static gboolean     wait = FALSE;
        static glong        expire_timeout = NOTIFY_EXPIRES_DEFAULT;
        GOptionContext     *opt_ctx;
        NotifyNotification *notify;
        GError             *error = NULL;
        gboolean            retval;

        static const GOptionEntry entries[] = {
                {"urgency", 'u', 0, G_OPTION_ARG_CALLBACK,
                 g_option_arg_urgency_cb,
                 N_("Specifies the urgency level (low, normal, critical)."),
                 N_("LEVEL")},
                {"expire-time", 't', 0, G_OPTION_ARG_INT, &expire_timeout,
                 N_
                 ("Specifies the timeout in milliseconds at which to expire the "
                  "notification."), N_("TIME")},
                {"app-name", 'a', 0, G_OPTION_ARG_STRING, &app_name,
                 N_("Specifies the app name for the icon"), N_("APP_NAME")},
                {"icon", 'i', 0, G_OPTION_ARG_FILENAME, &icons,
                 N_("Specifies an icon filename or stock icon to display."),
                 N_("ICON[,ICON...]")},
                {"category", 'c', 0, G_OPTION_ARG_FILENAME, &type,
                 N_("Specifies the notification category."),
                 N_("TYPE[,TYPE...]")},
                {"hint", 'h', 0, G_OPTION_ARG_FILENAME_ARRAY, &hints,
                 N_
                 ("Specifies basic extra data to pass. Valid types are int, double, string and byte."),
                 N_("TYPE:NAME:VALUE")},
                {"wait", 'w', 0, G_OPTION_ARG_NONE, &wait,
                 N_
                 ("Wait for the notification to be closed before exiting. Timeout must not be infinite."),
                 NULL},
                {"action", 'A', 0, G_OPTION_ARG_FILENAME_ARRAY, &actions,
                 N_
                 ("Specifies the actions to display to the user. Implies --wait to wait for user input."
                 " May be set multiple times. The name of the action (the words up to the first colon) "
                 "is output to stdout. If NAME is not specified, the numerical index of the option is "
                 "used (starting with 1)."),
                 N_("[NAME=]Text...")},
                {"version", 'v', 0, G_OPTION_ARG_NONE, &do_version,
                 N_("Version of the package."),
                 NULL},
                {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY,
                 &n_text, NULL,
                 NULL},
                {NULL}
        };

        body = NULL;

        setlocale (LC_ALL, "");

#if !GLIB_CHECK_VERSION (2, 36, 0)
        g_type_init ();
#endif

        g_set_prgname (argv[0]);
        g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

        opt_ctx = g_option_context_new (N_("<SUMMARY> [BODY] - "
                                           "create a notification"));
        g_option_context_add_main_entries (opt_ctx, entries, GETTEXT_PACKAGE);
        retval = g_option_context_parse (opt_ctx, &argc, &argv, &error);
        g_option_context_free (opt_ctx);

        if (!retval) {
                fprintf (stderr, "%s\n", error->message);
                g_error_free (error);
                exit (1);
        }

        if (do_version) {
                g_printf ("%s %s\n", g_get_prgname (), VERSION);
                exit (0);
        }

        if (n_text != NULL && n_text[0] != NULL && *n_text[0] != '\0')
                summary = n_text[0];

        if (summary == NULL) {
                fprintf (stderr, "%s\n", N_("No summary specified."));
                exit (1);
        }

        if (n_text[1] != NULL) {
                body = g_strcompress (n_text[1]);

                if (n_text[2] != NULL) {
                        fprintf (stderr, "%s\n",
                                 N_("Invalid number of options."));
                        exit (1);
                }
        }

        if (icons != NULL) {
                char           *c;

                /* XXX */
                if ((c = strchr (icons, ',')) != NULL)
                        *c = '\0';

                icon_str = icons;
        }

        if (!notify_init ("notify-send"))
                exit (1);

        notify = notify_notification_new (summary,
                                          body,
                                          icon_str);
        notify_notification_set_category (notify, type);
        notify_notification_set_urgency (notify, urgency);
        notify_notification_set_timeout (notify, expire_timeout);
        notify_notification_set_app_name (notify, app_name);

        g_free (body);

        /* Set hints */
        if (hints != NULL) {
                gint            i = 0;
                gint            l;
                char          *hint = NULL;
                char         **tokens = NULL;

                while ((hint = hints[i++])) {
                        tokens = g_strsplit (hint, ":", 3);
                        l = g_strv_length (tokens);

                        if (l != 3) {
                                fprintf (stderr, "%s\n",
                                         N_("Invalid hint syntax specified. "
                                            "Use TYPE:NAME:VALUE."));
                                hint_error = TRUE;
                        } else {
                                retval = notify_notification_set_hint_variant (notify,
                                                                               tokens[0],
                                                                               tokens[1],
                                                                               tokens[2],
                                                                               &error);

                                if (!retval) {
                                        fprintf (stderr, "%s\n", error->message);
                                        g_error_free (error);
                                        hint_error = TRUE;
                                }
                        }

                        g_strfreev (tokens);
                        if (hint_error)
                                break;
                }
        }

        if (wait && expire_timeout == 0)
                wait = FALSE;

        /* Parse actions */
        if (actions != NULL && !hint_error) {
                GList *iter = notify_get_server_caps();
                if (iter && g_list_length(iter)) {
                        hint_error = TRUE;

                        for (;iter != NULL; iter = iter->next) {
                                if (strcasecmp(iter->data, "actions"))
                                        continue;
                                hint_error = FALSE;
                                break;
                        }

                        g_list_free(iter);
                        if (hint_error) {
                                g_printerr(N_("Actions are not supported by this notifications server. Displaying non-interactively.\n"));
                                wait = FALSE;
                                goto err_cont;
                        }
                }

                gint    i = 0, l;
                char    *action = NULL, *name = NULL;
                gchar   **spl = NULL;

                while ((action = actions[i++])) {
                        spl = g_strsplit(action, "=", 2);
                        l = g_strv_length (spl);
                        if (l == 1)
                                name = g_strdup_printf("%i", i);
                        else
                                name = g_strdup(spl[0]);

                        notify_notification_add_action (notify,
                                                        name,
                                                        spl[l-1],
                                                        NOTIFY_ACTION_CALLBACK(&_handle_action),
                                                        name,
                                                        NULL);

                        g_strfreev (spl);
                }
                g_strfreev(actions);
                wait = TRUE;
        }

        if (wait)
                g_signal_connect (G_OBJECT (notify),
                        "closed",
                        G_CALLBACK (&_handle_closed),
                        NULL);

        if (!hint_error)
 err_cont:
                hint_error |= !notify_notification_show (notify, NULL);

        if (wait && !hint_error) {
                if (expire_timeout > 0 && actions == NULL)
                        // Don't leave us hanging. (Maybe add a switch to disable this?)
                        g_timeout_add (expire_timeout, G_SOURCE_FUNC(&_handle_timeout), NULL);
                loop = g_main_loop_new (NULL, FALSE);
                g_main_loop_run (loop);
                g_main_loop_unref(loop);
        }

        g_object_unref (G_OBJECT (notify));

        notify_uninit ();

        exit (hint_error);
}
