/* object representing one terminal window/tab with settings */

/*
 * Copyright (C) 2001 Havoc Pennington
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/* grab gtk_style_get_font and gdk_x11_get_font_name */
#undef GTK_DISABLE_DEPRECATED
#undef GDK_DISABLE_DEPRECATED
#include <gtk/gtkstyle.h>
#include <gdk/gdkx.h>
#define GTK_DISABLE_DEPRECATED
#define GDK_DISABLE_DEPRECATED

#include "terminal-intl.h"
#include "terminal-accels.h"
#include "terminal-window.h"
#include "terminal-widget.h"
#include "terminal-profile.h"
#include "terminal.h"
#include <libgnome/gnome-util.h> /* gnome_util_user_shell */
#include <libgnome/gnome-url.h> /* gnome_url_show */
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

struct _TerminalScreenPrivate
{
  GtkWidget *term;
  TerminalWindow *window;
  TerminalProfile *profile; /* may be NULL at times */
  guint profile_changed_id;
  guint profile_forgotten_id;
  int id;
  GtkWidget *popup_menu;
  char *raw_title;
  char *cooked_title;
  char *matched_string;
  char **override_command;
  GtkWidget *title_editor;
  GtkWidget *title_entry;
  char *working_dir;
  int child_pid;
  guint recheck_working_dir_idle;
};

static GList* used_ids = NULL;

enum {
  PROFILE_SET,
  TITLE_CHANGED,
  SELECTION_CHANGED,
  LAST_SIGNAL
};

static void terminal_screen_init        (TerminalScreen      *screen);
static void terminal_screen_class_init  (TerminalScreenClass *klass);
static void terminal_screen_finalize    (GObject             *object);
static void terminal_screen_update_on_realize (GtkWidget      *widget,
                                               TerminalScreen *screen);
static void     terminal_screen_popup_menu         (GtkWidget      *term,
                                                    TerminalScreen *screen);
static gboolean terminal_screen_button_press_event (GtkWidget      *term,
                                                    GdkEventButton *event,
                                                    TerminalScreen *screen);


static void terminal_screen_widget_title_changed     (GtkWidget      *term,
                                                      TerminalScreen *screen);

static void terminal_screen_widget_child_died        (GtkWidget      *term,
                                                      TerminalScreen *screen);

static void terminal_screen_widget_selection_changed (GtkWidget      *term,
                                                      TerminalScreen *screen);

static void terminal_screen_setup_dnd                (TerminalScreen *screen);

static void reread_profile (TerminalScreen *screen);

static void rebuild_title  (TerminalScreen *screen);

static void queue_recheck_working_dir (TerminalScreen *screen);

static gboolean xfont_is_monospace                   (const char *fontname);
static char*    make_xfont_monospace                 (const char *fontname);
static char*    make_xfont_char_cell                 (const char *fontname);
static char*    make_xfont_have_size_from_other_font (const char *fontname,
                                                     const char *other_font);
static PangoFontDescription* make_font_monospace (const PangoFontDescription *font);

static GdkFont* load_fonset_without_error (const gchar *fontset_name);

static gpointer parent_class;
static guint signals[LAST_SIGNAL] = { 0 };

GType
terminal_screen_get_type (void)
{
  static GType object_type = 0;

  g_type_init ();
  
  if (!object_type)
    {
      static const GTypeInfo object_info =
      {
        sizeof (TerminalScreenClass),
        (GBaseInitFunc) NULL,
        (GBaseFinalizeFunc) NULL,
        (GClassInitFunc) terminal_screen_class_init,
        NULL,           /* class_finalize */
        NULL,           /* class_data */
        sizeof (TerminalScreen),
        0,              /* n_preallocs */
        (GInstanceInitFunc) terminal_screen_init,
      };
      
      object_type = g_type_register_static (G_TYPE_OBJECT,
                                            "TerminalScreen",
                                            &object_info, 0);
    }
  
  return object_type;
}


static void
style_set_callback (GtkWidget *widget,
                    GtkStyle  *previous_style,
                    void      *data)
{
  if (GTK_WIDGET_REALIZED (widget))
    terminal_screen_update_on_realize (widget, TERMINAL_SCREEN (data));  
}

static void
terminal_screen_init (TerminalScreen *screen)
{
  char buf[PATH_MAX+1];
  
  screen->priv = g_new0 (TerminalScreenPrivate, 1);

  screen->priv->working_dir = g_strdup (getcwd (buf, sizeof (buf)));
  if (screen->priv->working_dir == NULL) /* shouldn't ever happen */
    screen->priv->working_dir = g_strdup (g_get_home_dir ());
  screen->priv->child_pid = -1;

  screen->priv->recheck_working_dir_idle = 0;
  
  screen->priv->term = terminal_widget_new ();
  
  g_object_ref (G_OBJECT (screen->priv->term));
  gtk_object_sink (GTK_OBJECT (screen->priv->term));

  terminal_widget_match_add (screen->priv->term,
                             "(((news|telnet|nttp|file|http|ftp|https)://)|(www|ftp)[-A-Za-z0-9]*\\.)[-A-Za-z0-9\\.]+(:[0-9]*)?");
  
  terminal_widget_match_add (screen->priv->term,
                             "(((news|telnet|nttp|file|http|ftp|https)://)|(www|ftp)[-A-Za-z0-9]*\\.)[-A-Za-z0-9\\.]+(:[0-9]*)?/[-A-Za-z0-9_\\$\\.\\+\\!\\*\\(\\),;:@&=\\?/~\\#\\%]*[^]'\\.}>\\) ,\\\"]");

  terminal_screen_setup_dnd (screen);
  
  g_object_set_data (G_OBJECT (screen->priv->term),
                     "terminal-screen",
                     screen);
  
  g_signal_connect (G_OBJECT (screen->priv->term),
                    "realize",
                    G_CALLBACK (terminal_screen_update_on_realize),
                    screen);

  g_signal_connect (G_OBJECT (screen->priv->term),
                    "style_set",
                    G_CALLBACK (style_set_callback),
                    screen);
  
  g_signal_connect (G_OBJECT (screen->priv->term),
                    "popup_menu",
                    G_CALLBACK (terminal_screen_popup_menu),
                    screen);

  g_signal_connect (G_OBJECT (screen->priv->term),
                    "button_press_event",
                    G_CALLBACK (terminal_screen_button_press_event),
                    screen);

  terminal_widget_connect_title_changed (screen->priv->term,
                                         G_CALLBACK (terminal_screen_widget_title_changed),
                                         screen);

  terminal_widget_connect_child_died (screen->priv->term,
                                      G_CALLBACK (terminal_screen_widget_child_died),
                                      screen);

  terminal_widget_connect_selection_changed (screen->priv->term,
                                             G_CALLBACK (terminal_screen_widget_selection_changed),
                                             screen);

  gtk_widget_show (screen->priv->term);
}

static void
terminal_screen_class_init (TerminalScreenClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  
  parent_class = g_type_class_peek_parent (klass);
  
  object_class->finalize = terminal_screen_finalize;
  
  signals[PROFILE_SET] =
    g_signal_new ("profile_set",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, profile_set),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[TITLE_CHANGED] =
    g_signal_new ("title_changed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, title_changed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[SELECTION_CHANGED] =
    g_signal_new ("selection_changed",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (TerminalScreenClass, selection_changed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);  
}

static void
terminal_screen_finalize (GObject *object)
{
  TerminalScreen *screen;

  screen = TERMINAL_SCREEN (object);
  
  used_ids = g_list_remove (used_ids, GINT_TO_POINTER (screen->priv->id));  

  if (screen->priv->title_editor)
    gtk_widget_destroy (screen->priv->title_editor);
  
  terminal_screen_set_profile (screen, NULL);
  
  g_object_unref (G_OBJECT (screen->priv->term));

  if (screen->priv->recheck_working_dir_idle)
    g_source_remove (screen->priv->recheck_working_dir_idle);
  
  g_free (screen->priv->raw_title);
  g_free (screen->priv->cooked_title);
  g_free (screen->priv->matched_string);
  g_strfreev (screen->priv->override_command);
  g_free (screen->priv->working_dir);
  
  g_free (screen->priv);
  
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static int
next_unused_id (void)
{
  int i = 0;

  while (g_list_find (used_ids, GINT_TO_POINTER (i)))
    ++i;

  return i;
}

TerminalScreen*
terminal_screen_new (void)
{
  TerminalScreen *screen;

  screen = g_object_new (TERMINAL_TYPE_SCREEN, NULL);
  
  screen->priv->id = next_unused_id ();

  used_ids = g_list_prepend (used_ids, GINT_TO_POINTER (screen->priv->id));
  
  return screen;
}

int
terminal_screen_get_id (TerminalScreen *screen)
{
  return screen->priv->id;
}

TerminalWindow*
terminal_screen_get_window (TerminalScreen *screen)
{
  return screen->priv->window;
}

void
terminal_screen_set_window (TerminalScreen *screen,
                            TerminalWindow *window)
{
  screen->priv->window = window;
}

const char*
terminal_screen_get_title (TerminalScreen *screen)
{
  if (screen->priv->cooked_title == NULL)
    rebuild_title (screen);

  /* cooked_title may still be NULL */
  if (screen->priv->cooked_title)
    return screen->priv->cooked_title;
  else
    return "";
}

static void
reread_profile (TerminalScreen *screen)
{
  TerminalProfile *profile;
  GtkWidget *term;
  TerminalBackgroundType bg_type;
  
  profile = screen->priv->profile;  
  
  if (profile == NULL)
    return;

  term = screen->priv->term;

  rebuild_title (screen);
  
  if (screen->priv->window)
    {
      /* We need these in line for the set_size in
       * update_on_realize
       */
      terminal_window_update_scrollbar (screen->priv->window, screen);
      terminal_window_update_icon (screen->priv->window);
      terminal_window_update_geometry (screen->priv->window);
    }
  
  if (GTK_WIDGET_REALIZED (screen->priv->term))
    terminal_screen_update_on_realize (term, screen);

  terminal_widget_set_cursor_blinks (term,
                                     terminal_profile_get_cursor_blink (profile));

  terminal_widget_set_audible_bell (term,
                                    !terminal_profile_get_silent_bell (profile));

  terminal_widget_set_word_characters (term,
                                       terminal_profile_get_word_chars (profile));

  terminal_widget_set_scroll_on_keystroke (term,
                                           terminal_profile_get_scroll_on_keystroke (profile));
  
  terminal_widget_set_scroll_on_output (term,
                                        terminal_profile_get_scroll_on_output (profile));

  terminal_widget_set_scrollback_lines (term,
                                        terminal_profile_get_scrollback_lines (profile));

  bg_type = terminal_profile_get_background_type (profile);
  
  if (bg_type == TERMINAL_BACKGROUND_IMAGE)
    {
      terminal_widget_set_background_image_file (term,
                                                 terminal_profile_get_background_image_file (profile));
      terminal_widget_set_background_scrolls (term,
                                              terminal_profile_get_scroll_background (profile));
    }
  else
    {
      terminal_widget_set_background_image_file (term, NULL);
      terminal_widget_set_background_scrolls (term, FALSE);
    }

  if (bg_type == TERMINAL_BACKGROUND_IMAGE ||
      bg_type == TERMINAL_BACKGROUND_TRANSPARENT)
    terminal_widget_set_background_darkness (term,
                                             terminal_profile_get_background_darkness (profile));
  else
    terminal_widget_set_background_darkness (term, 0.0); /* normal color */
  
  terminal_widget_set_background_transparent (term,
                                              bg_type == TERMINAL_BACKGROUND_TRANSPARENT);

  terminal_widget_set_backspace_binding (term,
                                         terminal_profile_get_backspace_binding (profile));
  
  terminal_widget_set_delete_binding (term,
                                      terminal_profile_get_delete_binding (profile));
}

static void
rebuild_title  (TerminalScreen *screen)
{
  TerminalTitleMode mode;

  if (screen->priv->profile)
    mode = terminal_profile_get_title_mode (screen->priv->profile);
  else
    mode = TERMINAL_TITLE_REPLACE;

  g_free (screen->priv->cooked_title);
  
  switch (mode)
    {
    case TERMINAL_TITLE_AFTER:
      screen->priv->cooked_title =
        g_strconcat (terminal_profile_get_title (screen->priv->profile),
                     (screen->priv->raw_title && *(screen->priv->raw_title)) ?
                     " - " : "",
                     screen->priv->raw_title,
                     NULL);
      break;
    case TERMINAL_TITLE_BEFORE:
      screen->priv->cooked_title =
        g_strconcat (screen->priv->raw_title ?
                     screen->priv->raw_title : "",
                     (screen->priv->raw_title && *(screen->priv->raw_title)) ?
                     " - " : "",
                     terminal_profile_get_title (screen->priv->profile),
                     NULL);
      break;
    case TERMINAL_TITLE_REPLACE:
      if (screen->priv->raw_title)
        screen->priv->cooked_title = g_strdup (screen->priv->raw_title);
      else
        screen->priv->cooked_title = g_strdup (terminal_profile_get_title (screen->priv->profile));
      break;
    case TERMINAL_TITLE_IGNORE:
      screen->priv->cooked_title = g_strdup (terminal_profile_get_title (screen->priv->profile));
      break;
    default:
      g_assert_not_reached ();
      break;
    }
      
  g_signal_emit (G_OBJECT (screen), signals[TITLE_CHANGED], 0);
}

static void
profile_changed_callback (TerminalProfile          *profile,
                          TerminalSettingMask       mask,
                          TerminalScreen           *screen)
{
  reread_profile (screen);
}

static void
update_color_scheme (TerminalScreen *screen)
{
  GdkColor fg, bg;
  GdkColor palette[TERMINAL_PALETTE_SIZE];
  
  if (screen->priv->term == NULL ||
      !GTK_WIDGET_REALIZED (screen->priv->term))
    return;

  terminal_profile_get_palette (screen->priv->profile,
                                palette);

  if (terminal_profile_get_use_theme_colors (screen->priv->profile))
    {
      fg = screen->priv->term->style->text[GTK_STATE_NORMAL];
      bg = screen->priv->term->style->base[GTK_STATE_NORMAL];
    }
  else
    {
      terminal_profile_get_color_scheme (screen->priv->profile,
                                         &fg, &bg);
    }

  terminal_widget_set_colors (screen->priv->term,
                              &fg, &bg, palette);
}

static void
terminal_screen_update_on_realize (GtkWidget      *term,
                                   TerminalScreen *screen)
{
  TerminalProfile *profile;
  
  profile = screen->priv->profile;
  
  update_color_scheme (screen);

  if (terminal_widget_supports_pango_fonts ())
    {
      if (terminal_profile_get_use_system_font (profile))
        {
          PangoFontDescription *desc;

          /* Try creating monospace variant of system font */
          desc = make_font_monospace (term->style->font_desc);

          if (desc)
            {
              terminal_widget_set_pango_font (term, desc);
              
              pango_font_description_free (desc);
            }
          else
            {
              /* At least use the same size */
              desc = pango_font_description_copy (terminal_profile_get_font (profile));

              if (pango_font_description_get_set_fields (term->style->font_desc) &
                  PANGO_FONT_MASK_SIZE)
                {
                  pango_font_description_set_size (desc,
                                                   pango_font_description_get_size (term->style->font_desc));
                }

              terminal_widget_set_pango_font (term,
                                              terminal_profile_get_font (profile));

              pango_font_description_free (desc);
            }
        }
      else
        {
          terminal_widget_set_pango_font (term,
                                          terminal_profile_get_font (profile));
        }
    }
  else
    {
      GdkFont *font;

      font = NULL;
      
       if (terminal_profile_get_use_system_font (screen->priv->profile))
         {
            GtkStyle *style;
            GdkFont *from_desc;

            style = gtk_widget_get_style (GTK_WIDGET(term));
            from_desc = gdk_font_from_description (style->font_desc);
            
            if (from_desc != NULL)
              {
                char *font_name;
                
                font_name = g_strdup (gdk_x11_font_get_name (from_desc));

                g_assert (font_name);
                
                if (!xfont_is_monospace (font_name));
                {
                  /* Can't use the system font as-is */
                  char *fallback;

                  fallback = make_xfont_monospace (font_name);

                  g_assert (fallback);
                  
                  font = load_fonset_without_error (fallback);

                  if (font == NULL)
                    {
                      g_free (fallback);
                      fallback = make_xfont_char_cell (font_name);
                      font = load_fonset_without_error (fallback);
                    }
                
                  if (font == NULL)
                    {
                      g_free (fallback);
                      fallback =
                        make_xfont_have_size_from_other_font (terminal_profile_get_x_font (profile),
                                                              font_name);
                      font = load_fonset_without_error (fallback);
                    }
                
                  g_free (fallback);
                }
                
                g_free (font_name);
                gdk_font_unref (from_desc);
              }
         }
       
       if (font == NULL)
         {
           font = gdk_fontset_load (terminal_profile_get_x_font (profile));
           if (font == NULL)
             {
               g_printerr (_("Could not load font \"%s\"\n"),
                           terminal_profile_get_x_font (profile));
             }
         }

       if (font)
         {
           terminal_widget_set_normal_gdk_font (term, font);
           terminal_widget_set_bold_gdk_font (term, font);
           gdk_font_unref (font);
         }
    }
  
  terminal_widget_set_allow_bold (term,
                                  terminal_profile_get_allow_bold (profile));

  terminal_window_set_size (screen->priv->window, screen, TRUE);
}

static void
profile_forgotten_callback (TerminalProfile *profile,
                            TerminalScreen  *screen)
{
  TerminalProfile *new_profile;

  /* Revert to the new term profile if any */
  new_profile = terminal_profile_get_for_new_term (NULL);

  if (new_profile)
    terminal_screen_set_profile (screen, new_profile);
  else
    g_assert_not_reached (); /* FIXME ? */
}

void
terminal_screen_set_profile (TerminalScreen *screen,
                             TerminalProfile *profile)
{
  if (profile == screen->priv->profile)
    return;
  
  if (screen->priv->profile_changed_id)
    {
      g_signal_handler_disconnect (G_OBJECT (screen->priv->profile),
                                   screen->priv->profile_changed_id);
      screen->priv->profile_changed_id = 0;
    }

  if (screen->priv->profile_forgotten_id)
    {
      g_signal_handler_disconnect (G_OBJECT (screen->priv->profile),
                                   screen->priv->profile_forgotten_id);
      screen->priv->profile_forgotten_id = 0;
    }
  
  if (profile)
    {
      g_object_ref (G_OBJECT (profile));
      screen->priv->profile_changed_id =
        g_signal_connect (G_OBJECT (profile),
                          "changed",
                          G_CALLBACK (profile_changed_callback),
                          screen);
      screen->priv->profile_forgotten_id =
        g_signal_connect (G_OBJECT (profile),
                          "forgotten",
                          G_CALLBACK (profile_forgotten_callback),
                          screen);
    }

#if 0
  g_print ("Switching profile from '%s' to '%s'\n",
           screen->priv->profile ?
           terminal_profile_get_visible_name (screen->priv->profile) : "none",
           profile ? terminal_profile_get_visible_name (profile) : "none");
#endif
  
  if (screen->priv->profile)
    {
      g_object_unref (G_OBJECT (screen->priv->profile));
    }

  screen->priv->profile = profile;

  reread_profile (screen);

  if (screen->priv->profile)
    g_signal_emit (G_OBJECT (screen), signals[PROFILE_SET], 0);
}

TerminalProfile*
terminal_screen_get_profile (TerminalScreen *screen)
{
  return screen->priv->profile;
}

void
terminal_screen_set_override_command (TerminalScreen *screen,
                                      char          **argv)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));

  g_strfreev (screen->priv->override_command);
  screen->priv->override_command = g_strdupv (argv);
}

const char**
terminal_screen_get_override_command (TerminalScreen *screen)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), NULL);

  return (const char**) screen->priv->override_command;
}


GtkWidget*
terminal_screen_get_widget (TerminalScreen *screen)
{
  return screen->priv->term;
}

static void
show_fork_error_dialog (TerminalScreen *screen,
                        GError         *err)
{
  GtkWidget *dialog;
  
  dialog = gtk_message_dialog_new ((GtkWindow*)
                                   gtk_widget_get_ancestor (screen->priv->term,
                                                            GTK_TYPE_WINDOW),
                                   GTK_DIALOG_DESTROY_WITH_PARENT,
                                   GTK_MESSAGE_ERROR,
                                   GTK_BUTTONS_CLOSE,
                                   "%s", err->message);

  g_signal_connect (G_OBJECT (dialog),
                    "response",
                    G_CALLBACK (gtk_widget_destroy),
                    NULL);

  gtk_widget_show (dialog);
}

static void
show_command_error_dialog (TerminalScreen *screen,
                        GError         *error)
{
  GtkWidget *dialog;

  g_return_if_fail (error != NULL);
  
  dialog = gtk_message_dialog_new ((GtkWindow*)
                                   gtk_widget_get_ancestor (screen->priv->term,
                                                            GTK_TYPE_WINDOW),
                                   GTK_DIALOG_DESTROY_WITH_PARENT,
                                   GTK_MESSAGE_ERROR,
                                   GTK_BUTTONS_CLOSE,
                                   _("There was a problem with the command for this terminal: %s"),
                                   error->message);

  g_signal_connect (G_OBJECT (dialog),
                    "response",
                    G_CALLBACK (gtk_widget_destroy),
                    NULL);

  gtk_widget_show (dialog);
}

static gboolean
get_child_command (TerminalScreen *screen,
                   char          **file_p,
                   char         ***argv_p,
                   GError        **err)
{
  /* code from gnome-terminal */
  TerminalProfile *profile;
  char  *file;
  char **argv;

  profile = screen->priv->profile;

  file = NULL;
  argv = NULL;
  
  if (file_p)
    *file_p = NULL;
  if (argv_p)
    *argv_p = NULL;

  if (screen->priv->override_command)
    {
      file = g_strdup (screen->priv->override_command[0]);
      argv = g_strdupv (screen->priv->override_command);
    }
  else if (terminal_profile_get_use_custom_command (profile))
    {
      if (!g_shell_parse_argv (terminal_profile_get_custom_command (profile),
                               NULL, &argv,
                               err))
        return FALSE;

      file = g_strdup (argv[0]);
    }
  else
    {
      const char *only_name;
      char *shell;

      shell = gnome_util_user_shell ();

      file = g_strdup (shell);
      
      only_name = strrchr (shell, '/');
      if (only_name != NULL)
        only_name++;
      else
        only_name = shell;

      argv = g_new (char*, 2);

      if (terminal_profile_get_login_shell (profile))
        argv[0] = g_strconcat ("-", only_name, NULL);
      else
        argv[0] = g_strdup (only_name);

      argv[1] = NULL;

      g_free (shell);
    }

  if (file_p)
    *file_p = file;
  else
    g_free (file);

  if (argv_p)
    *argv_p = argv;
  else
    g_strfreev (argv);

  return TRUE;
}

extern char **environ;

static char**
get_child_environment (GtkWidget      *term,
                       TerminalScreen *screen)
{
  /* code from gnome-terminal, sort of. */
  TerminalProfile *profile;
  char **p;
  int i;
  char **retval;
#define EXTRA_ENV_VARS 6
  
  profile = screen->priv->profile;

  /* count env vars that are set */
  for (p = environ; *p; p++)
    ;
  
  i = p - environ;
  retval = g_new (char *, i + 1 + EXTRA_ENV_VARS);

  for (i = 0, p = environ; *p; p++)
    {
      /* Strip all these out, we'll replace some of them */
      if ((strncmp (*p, "COLUMNS=", 8) == 0) ||
          (strncmp (*p, "LINES=", 6) == 0)   ||
          (strncmp (*p, "WINDOWID=", 9) == 0) ||
          (strncmp (*p, "TERM=", 5) == 0)    ||
          (strncmp (*p, "GNOME_DESKTOP_ICON=", 19) == 0) ||
          (strncmp (*p, "COLORTERM=", 10) == 0))
        {
          /* nothing: do not copy */
        }
      else
        {
          retval[i] = g_strdup (*p);
          ++i;
        }
    }

  retval[i] = g_strdup ("COLORTERM="EXECUTABLE_NAME);
  ++i;
  retval[i] = g_strdup ("TERM=xterm"); /* FIXME configurable later? */
  ++i;
  retval[i] = g_strdup_printf ("WINDOWID=%ld",
                               GDK_WINDOW_XWINDOW (term->window));
  ++i;
  
  retval[i] = NULL;
  
  return retval;
}

void
terminal_screen_launch_child (TerminalScreen *screen)
{
  TerminalProfile *profile;
  char **env;
  char  *path;
  char **argv;
  GError *err;
  
  profile = screen->priv->profile;

  err = NULL;
  if (!get_child_command (screen, &path, &argv, &err))
    {
      show_command_error_dialog (screen, err);
      g_error_free (err);
      return;
    }
  
  env = get_child_environment (screen->priv->term, screen);  

  err = NULL;
  if (!terminal_widget_fork_command (screen->priv->term,
                                     terminal_profile_get_update_records (profile),
                                     path,
                                     argv,
                                     env,
                                     terminal_screen_get_working_dir (screen),
                                     &screen->priv->child_pid,
                                     &err))
    {
      show_fork_error_dialog (screen, err);
      g_error_free (err);
    }
  
  g_free (path);
  g_strfreev (argv);
  g_strfreev (env);
}

void
terminal_screen_close (TerminalScreen *screen)
{
  g_return_if_fail (screen->priv->window);

  terminal_window_remove_screen (screen->priv->window, screen);
  /* screen should be finalized here, do not touch it past this point */
}

gboolean
terminal_screen_get_text_selected (TerminalScreen *screen)
{
  if (GTK_WIDGET_REALIZED (screen->priv->term))
    return terminal_widget_get_has_selection (screen->priv->term);
  else
    return FALSE;
}

static void
new_window_callback (GtkWidget      *menu_item,
                     TerminalScreen *screen)
{
  terminal_app_new_terminal (terminal_app_get (),
                             terminal_profile_get_for_new_term (screen->priv->profile),
                             NULL,
                             FALSE, FALSE, NULL, NULL, NULL, NULL);
}

static void
new_tab_callback (GtkWidget      *menu_item,
                  TerminalScreen *screen)
{
  terminal_app_new_terminal (terminal_app_get (),
                             terminal_profile_get_for_new_term (screen->priv->profile),
                             screen->priv->window,
                             FALSE, FALSE, NULL, NULL, NULL, NULL);
}

static void
copy_callback (GtkWidget      *menu_item,
               TerminalScreen *screen)
{
  terminal_widget_copy_clipboard (screen->priv->term);
}

static void
paste_callback (GtkWidget      *menu_item,
                TerminalScreen *screen)
{
  terminal_widget_paste_clipboard (screen->priv->term);
}

static void
configuration_callback (GtkWidget      *menu_item,
                        TerminalScreen *screen)
{
  g_return_if_fail (screen->priv->profile);
  
  terminal_app_edit_profile (terminal_app_get (),
                             screen->priv->profile, 
                             GTK_WINDOW (screen->priv->window));
}

static void
choose_profile_callback (GtkWidget      *menu_item,
                         TerminalScreen *screen)
{
  TerminalProfile *profile;

  if (!gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (menu_item)))
    return;
  
  profile = g_object_get_data (G_OBJECT (menu_item),
                               "profile");

  g_assert (profile);

  if (!terminal_profile_get_forgotten (profile))
    {
      terminal_screen_set_profile (screen, profile);
    }
}

static void
show_menubar_callback (GtkWidget      *menu_item,
                       TerminalScreen *screen)
{
  if (terminal_window_get_menubar_visible (screen->priv->window))
    terminal_window_set_menubar_visible (screen->priv->window,
                                         FALSE);
  else
    terminal_window_set_menubar_visible (screen->priv->window,
                                         TRUE);
}

#if 0
static void
secure_keyboard_callback (GtkWidget      *menu_item,
                          TerminalScreen *screen)
{
  not_implemented ();
}
#endif

static void
open_url (TerminalScreen *screen,
          const char     *orig_url)
{
  GError *err;
  GtkWidget *dialog;
  char *url;
  
  g_return_if_fail (orig_url != NULL);

  /* this is to handle gnome_url_show reentrancy */
  url = g_strdup (orig_url);
  g_object_ref (G_OBJECT (screen));
  
  err = NULL;
  gnome_url_show (url, &err);

  if (err)
    {
      GtkWidget *window;

      if (screen->priv->term)
        window = gtk_widget_get_ancestor (screen->priv->term,
                                          GTK_TYPE_WINDOW);
      else
        window = NULL;
      
      dialog = gtk_message_dialog_new (window ? GTK_WINDOW (window) : NULL,
                                       GTK_DIALOG_DESTROY_WITH_PARENT,
                                       GTK_MESSAGE_ERROR,
                                       GTK_BUTTONS_CLOSE,
                                       _("Could not open the address \"%s\":\n%s"),
                                       url, err->message);
      
      g_error_free (err);
      
      g_signal_connect (G_OBJECT (dialog),
                        "response",
                        G_CALLBACK (gtk_widget_destroy),
                        NULL);
      
      gtk_widget_show (dialog);
    }

  g_free (url);
  g_object_unref (G_OBJECT (screen));
}

static void
open_url_callback (GtkWidget      *menu_item,
                   TerminalScreen *screen)
{
  if (screen->priv->matched_string)
    {
      open_url (screen, screen->priv->matched_string);
      g_free (screen->priv->matched_string);
      screen->priv->matched_string = NULL;
    }
}

static void
copy_url_callback (GtkWidget      *menu_item,
                   TerminalScreen *screen)
{
  if (screen->priv->matched_string)
    {
      gtk_clipboard_set_text (gtk_clipboard_get (GDK_NONE), 
                              screen->priv->matched_string, -1);
      g_free (screen->priv->matched_string);
      screen->priv->matched_string = NULL;
    }
}


static void
popup_menu_detach (GtkWidget *attach_widget,
		   GtkMenu   *menu)
{
  TerminalScreen *screen;

  screen = g_object_get_data (G_OBJECT (attach_widget), "terminal-screen");

  g_assert (screen);

  screen->priv->popup_menu = NULL;
}

static GtkWidget*
append_menuitem (GtkWidget  *menu,
                 const char *text,
                 GCallback   callback,
                 gpointer    data)
{
  GtkWidget *menu_item;
  
  menu_item = gtk_menu_item_new_with_mnemonic (text);
  gtk_widget_show (menu_item);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu),
                         menu_item);

  g_signal_connect (G_OBJECT (menu_item),
                    "activate",
                    callback, data);

  return menu_item;
}

static GtkWidget*
append_stock_menuitem (GtkWidget  *menu,
                       const char *text,
                       GCallback   callback,
                       gpointer    data)
{
  GtkWidget *menu_item;
  
  menu_item = gtk_image_menu_item_new_from_stock (text, NULL);
  gtk_widget_show (menu_item);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu),
                         menu_item);

  if (callback)
    g_signal_connect (G_OBJECT (menu_item),
                      "activate",
                      callback, data);

  return menu_item;
}

static void
terminal_screen_do_popup (TerminalScreen *screen,
                          GdkEventButton *event)
{
  GtkWidget *profile_menu;
  GtkWidget *menu_item;
  GList *profiles;
  GList *tmp;
  GSList *group;
  
  if (screen->priv->popup_menu)
    gtk_widget_destroy (screen->priv->popup_menu);

  g_assert (screen->priv->popup_menu == NULL);
  
  screen->priv->popup_menu = gtk_menu_new ();
  gtk_menu_set_accel_group (GTK_MENU (screen->priv->popup_menu),
                            terminal_accels_get_group_for_widget (screen->priv->popup_menu));
  
  gtk_menu_attach_to_widget (GTK_MENU (screen->priv->popup_menu),
                             GTK_WIDGET (screen->priv->term),
                             popup_menu_detach);

  menu_item = append_menuitem (screen->priv->popup_menu,
                               _("_New Window"),
                               G_CALLBACK (new_window_callback),
                               screen);
  gtk_menu_item_set_accel_path (GTK_MENU_ITEM (menu_item),
                                ACCEL_PATH_NEW_WINDOW);
  
  menu_item = append_menuitem (screen->priv->popup_menu,
                               _("New _Tab"),
                               G_CALLBACK (new_tab_callback),
                               screen);
  gtk_menu_item_set_accel_path (GTK_MENU_ITEM (menu_item),
                                ACCEL_PATH_NEW_TAB);

  menu_item = append_stock_menuitem (screen->priv->popup_menu,
                                     GTK_STOCK_COPY,
                                     G_CALLBACK (copy_callback),
                                     screen);
  gtk_menu_item_set_accel_path (GTK_MENU_ITEM (menu_item),
                                ACCEL_PATH_COPY);

  if (!terminal_screen_get_text_selected (screen))
    gtk_widget_set_sensitive (menu_item, FALSE);

  menu_item = append_stock_menuitem (screen->priv->popup_menu,
                                     GTK_STOCK_PASTE,
                                     G_CALLBACK (paste_callback),
                                     screen);
  gtk_menu_item_set_accel_path (GTK_MENU_ITEM (menu_item),
                                ACCEL_PATH_PASTE);
  
  profile_menu = gtk_menu_new ();
  menu_item = gtk_menu_item_new_with_mnemonic (_("_Profile"));

  gtk_menu_item_set_submenu (GTK_MENU_ITEM (menu_item),
                             profile_menu);

  gtk_widget_show (profile_menu);
  gtk_widget_show (menu_item);
  
  gtk_menu_shell_append (GTK_MENU_SHELL (screen->priv->popup_menu),
                         menu_item);

  group = NULL;
  profiles = terminal_profile_get_list ();
  tmp = profiles;
  while (tmp != NULL)
    {
      TerminalProfile *profile;
      
      profile = tmp->data;
      
      /* Profiles can go away while the menu is up. */
      g_object_ref (G_OBJECT (profile));

      menu_item = gtk_radio_menu_item_new_with_label (group,
                                                      terminal_profile_get_visible_name (profile));
      group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (menu_item));
      gtk_widget_show (menu_item);
      gtk_menu_shell_append (GTK_MENU_SHELL (profile_menu),
                             menu_item);

      if (profile == screen->priv->profile)
        gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (menu_item),
                                        TRUE);
      
      g_signal_connect (G_OBJECT (menu_item),
                        "toggled",
                        G_CALLBACK (choose_profile_callback),
                        screen);
      
      g_object_set_data_full (G_OBJECT (menu_item),
                              "profile",
                              profile,
                              (GDestroyNotify) g_object_unref);
      
      tmp = tmp->next;
    }

  g_list_free (profiles);

  append_menuitem (screen->priv->popup_menu,
		   _("_Edit Current Profile..."),
		   G_CALLBACK (configuration_callback),
		   screen);

  
  if (terminal_window_get_menubar_visible (screen->priv->window))
    menu_item = append_menuitem (screen->priv->popup_menu,
				 _("Hide _Menubar"),
				 G_CALLBACK (show_menubar_callback),
				 screen);
  else
    menu_item =append_menuitem (screen->priv->popup_menu,
				_("Show _Menubar"),
				G_CALLBACK (show_menubar_callback),
				screen);
  gtk_menu_item_set_accel_path (GTK_MENU_ITEM (menu_item),
				ACCEL_PATH_TOGGLE_MENUBAR);


#if 0
  append_menuitem (screen->priv->popup_menu,
                   _("Secure _Keyboard"),
                   G_CALLBACK (secure_keyboard_callback),
                   screen);
#endif

  menu_item = append_menuitem (screen->priv->popup_menu,
                               _("_Open Link"),
                               G_CALLBACK (open_url_callback),
                               screen);

  gtk_widget_set_sensitive (menu_item, screen->priv->matched_string != NULL);
  
  menu_item = append_menuitem (screen->priv->popup_menu,
                               _("_Copy Link Address"),
                               G_CALLBACK (copy_url_callback),
                               screen);

  gtk_widget_set_sensitive (menu_item, screen->priv->matched_string != NULL);
  
  gtk_menu_popup (GTK_MENU (screen->priv->popup_menu),
                  NULL, NULL,
                  NULL, NULL, 
                  event ? event->button : 0,
                  event ? event->time : gtk_get_current_event_time ());
}

static void
terminal_screen_popup_menu (GtkWidget      *term,
                            TerminalScreen *screen)
{
  terminal_screen_do_popup (screen, NULL);
}

static gboolean
terminal_screen_button_press_event (GtkWidget      *widget,
                                    GdkEventButton *event,
                                    TerminalScreen *screen)
{
  GtkWidget *term;
  int char_width, char_height;
  
  term = screen->priv->term;

  terminal_widget_get_cell_size (term, &char_width, &char_height);
  
  g_free (screen->priv->matched_string);
  screen->priv->matched_string =
    terminal_widget_check_match (term,
                                 event->x / char_width,
                                 event->y / char_height);
  
  if (event->button == 1 || event->button == 2)
    {
      gtk_widget_grab_focus (widget);
      
      if (screen->priv->matched_string &&
          (event->state & GDK_CONTROL_MASK))
        {
          open_url (screen, screen->priv->matched_string);
          g_free (screen->priv->matched_string);
          screen->priv->matched_string = NULL;
          return TRUE; /* don't do anything else such as select with the click */
        }
      
#if 0
      /* old gnome-terminal had this code, but I'm not sure if
       * it should be here. We always return FALSE, but
       * sometimes old gnome-terminal didn't, so maybe that's
       * why it had this.
       */
      if (event->button != 3
          || (!(event->state & GDK_CONTROL_MASK) && term->vx->selected)
          || (term->vx->vt.mode & VTMODE_SEND_MOUSE))
        return FALSE;
#endif      

      return FALSE; /* pass thru the click */
    }
  else if (event->button == 3)
    {
      terminal_screen_do_popup (screen, event);
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

void
terminal_screen_set_dynamic_title (TerminalScreen *screen,
                                   const char     *title)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));
  
  if (screen->priv->raw_title && title &&
      strcmp (screen->priv->raw_title, title) == 0)
    return;

  g_free (screen->priv->raw_title);
  screen->priv->raw_title = g_strdup (title);
  rebuild_title (screen);

  if (screen->priv->title_entry &&
      screen->priv->raw_title)
    {
      char *text;
      
      text = gtk_editable_get_chars (GTK_EDITABLE (screen->priv->title_entry),
                                     0, -1);

      if (strcmp (text, screen->priv->raw_title) != 0)
        gtk_entry_set_text (GTK_ENTRY (screen->priv->title_entry),
                            screen->priv->raw_title);
      
      g_free (text);
    }
}

const char*
terminal_screen_get_dynamic_title (TerminalScreen *screen)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), NULL);
  
  return screen->priv->raw_title;
}

void
terminal_screen_set_working_dir (TerminalScreen *screen,
                                 const char     *dirname)
{
  g_return_if_fail (TERMINAL_IS_SCREEN (screen));  

  g_free (screen->priv->working_dir);
  screen->priv->working_dir = g_strdup (dirname);
}

const char*
terminal_screen_get_working_dir (TerminalScreen *screen)
{
  g_return_val_if_fail (TERMINAL_IS_SCREEN (screen), NULL);

  /* If we're on Linux and have a child PID, try to update
   * the working dir
   */
  if (screen->priv->child_pid >= 0)
    {
      char *file;
      char buf[PATH_MAX+1];
      int len;
      
      file = g_strdup_printf ("/proc/%d/cwd", screen->priv->child_pid);

      /* Silently ignore failure here, since we may not be on Linux */
      len = readlink (file, buf, sizeof (buf) - 1);

      if (len >= 0)
        {
          buf[len] = '\0';
          
          g_free (screen->priv->working_dir);
          screen->priv->working_dir = g_strdup (buf);
        }
      
      g_free (file);
    }
  
  return screen->priv->working_dir;
}

static gboolean
recheck_dir (void *data)
{
  TerminalScreen *screen = data;

  screen->priv->recheck_working_dir_idle = 0;
  
  /* called just for side effect */
  terminal_screen_get_working_dir (screen);

  /* remove idle */
  return FALSE;
}

static void
queue_recheck_working_dir (TerminalScreen *screen)
{
  if (screen->priv->recheck_working_dir_idle == 0)
    {
      screen->priv->recheck_working_dir_idle =
        g_idle_add_full (G_PRIORITY_LOW + 50,
                         recheck_dir,
                         screen,
                         NULL);
    }
}

static void
terminal_screen_widget_title_changed (GtkWidget      *widget,
                                      TerminalScreen *screen)
{
  terminal_screen_set_dynamic_title (screen,
                                     terminal_widget_get_title (widget));  

  queue_recheck_working_dir (screen);
}

static void
terminal_screen_widget_child_died (GtkWidget      *term,
                                   TerminalScreen *screen)
{
  TerminalExitAction action;

  screen->priv->child_pid = -1;
  
  action = TERMINAL_EXIT_CLOSE;
  if (screen->priv->profile)
    action = terminal_profile_get_exit_action (screen->priv->profile);
  
  switch (action)
    {
    case TERMINAL_EXIT_CLOSE:
      terminal_screen_close (screen);
      break;
    case TERMINAL_EXIT_RESTART:
      terminal_screen_launch_child (screen);
      break;
    }
}

static void
terminal_screen_widget_selection_changed (GtkWidget      *term,
                                          TerminalScreen *screen)
{
  g_signal_emit (G_OBJECT (screen), signals[SELECTION_CHANGED], 0);
}

static void
title_entry_changed (GtkWidget      *entry,
                     TerminalScreen *screen)
{
  char *text;

  text = gtk_editable_get_chars (GTK_EDITABLE (entry), 0, -1);
  
  terminal_screen_set_dynamic_title (screen, text);

  g_free (text);
}

void
terminal_screen_edit_title (TerminalScreen *screen,
                            GtkWindow      *transient_parent)
{
  GtkWindow *old_transient_parent;
  
  if (screen->priv->title_editor == NULL)
    {
      GtkWidget *vbox;
      GtkWidget *hbox;
      GtkWidget *entry;
      GtkWidget *label;
      
      old_transient_parent = NULL;      
      
      screen->priv->title_editor =
        gtk_dialog_new_with_buttons (_("Change terminal title"),
                                     NULL,
                                     GTK_DIALOG_DESTROY_WITH_PARENT,
                                     GTK_STOCK_CLOSE,
                                     GTK_RESPONSE_ACCEPT,
                                     NULL);
      
      g_signal_connect (G_OBJECT (screen->priv->title_editor),
                        "response",
                        G_CALLBACK (gtk_widget_destroy),
                        NULL);

      g_object_add_weak_pointer (G_OBJECT (screen->priv->title_editor),
                                 (void**) &screen->priv->title_editor);

      gtk_window_set_resizable (GTK_WINDOW (screen->priv->title_editor),
                                TRUE);
      
#define PADDING 5
      
      vbox = gtk_vbox_new (FALSE, PADDING);
      gtk_container_set_border_width (GTK_CONTAINER (vbox), PADDING);
      gtk_box_pack_start (GTK_BOX (GTK_DIALOG (screen->priv->title_editor)->vbox),
                          vbox, TRUE, TRUE, 0);
      
      hbox = gtk_hbox_new (FALSE, PADDING);

      label = gtk_label_new_with_mnemonic (_("_Title:"));
      gtk_misc_set_alignment (GTK_MISC (label), 1.0, 0.5);
      entry = gtk_entry_new ();

      gtk_entry_set_width_chars (GTK_ENTRY (entry), 30);
      
      gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
      gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
      terminal_util_set_labelled_by (entry, GTK_LABEL (label));
      
      gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
      gtk_box_pack_end (GTK_BOX (hbox), entry, TRUE, TRUE, 0);
      
      gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);      
      
      gtk_widget_grab_focus (entry);
      gtk_dialog_set_default_response (GTK_DIALOG (screen->priv->title_editor),
                                       GTK_RESPONSE_ACCEPT);
      
      if (screen->priv->raw_title)
        gtk_entry_set_text (GTK_ENTRY (entry), screen->priv->raw_title);
      
      g_signal_connect (G_OBJECT (entry), "changed",
                        G_CALLBACK (title_entry_changed),
                        screen);

      screen->priv->title_entry = entry;
      g_object_add_weak_pointer (G_OBJECT (screen->priv->title_entry),
                                 (void**) &screen->priv->title_entry);

    }
  else
    {
      old_transient_parent =
        gtk_window_get_transient_for (GTK_WINDOW (screen->priv->title_editor));
    }
    
  if (old_transient_parent != transient_parent)
    {
      gtk_window_set_transient_for (GTK_WINDOW (screen->priv->title_editor),
                                    transient_parent);
      gtk_widget_hide (screen->priv->title_editor); /* re-show the window on its new parent */
    }
  
  gtk_widget_show_all (screen->priv->title_editor);
  gtk_window_present (GTK_WINDOW (screen->priv->title_editor));
}

enum
{
  TARGET_URI_LIST,
  TARGET_UTF8_STRING,
  TARGET_TEXT,
  TARGET_COMPOUND_TEXT,
  TARGET_STRING,
  TARGET_COLOR,
  TARGET_BGIMAGE,
  TARGET_RESET_BG,
  TARGET_TEXT_PLAIN,
  TARGET_MOZ_URL
};

static void  
drag_data_received  (GtkWidget *widget, GdkDragContext *context, 
		     gint x, gint y,
		     GtkSelectionData *selection_data, guint info,
		     guint time,
                     gpointer data)
{
  TerminalScreen *screen;

  screen = TERMINAL_SCREEN (data);

#if 0
  {
    GList *tmp;
    char *str;
    
    tmp = context->targets;
    while (tmp != NULL)
      {
        GdkAtom atom = GPOINTER_TO_UINT (tmp->data);

        g_print ("Target: %s\n", gdk_atom_name (atom));        
        
        tmp = tmp->next;
      }
  }
#endif
  
  switch (info)
    {
    case TARGET_STRING:
    case TARGET_UTF8_STRING:
    case TARGET_COMPOUND_TEXT:
    case TARGET_TEXT:
      {
        char *str;
        
        str = gtk_selection_data_get_text (selection_data);

        /*
	 * pass UTF-8 to the terminal widget. The terminal widget
	 * should know which encoding mode it's in and be able
	 * to perform the correct conversion.
         */
        if (str && *str)
	  terminal_widget_write_data_to_child (screen->priv->term,
					       str, strlen (str));
        g_free (str);
      }
      break;

    case TARGET_TEXT_PLAIN:
      {
        if (selection_data->format != 8 ||
            selection_data->length == 0)
          {
            g_printerr (_("text/plain dropped on terminal had wrong format (%d) or length (%d)\n"),
                        selection_data->format,
                        selection_data->length);
            return;
          }
        
        /* FIXME just brazenly ignoring encoding issues... */
        terminal_widget_write_data_to_child (screen->priv->term,
                                             selection_data->data,
                                             selection_data->length);
      }
      break;
      
    case TARGET_COLOR:
      {
        guint16 *data = (guint16 *)selection_data->data;
        GdkColor color;
        GdkColor fg;
        TerminalProfile *profile;

        if (selection_data->format != 16 ||
            selection_data->length != 8)
          {
            g_printerr (_("Color dropped on terminal had wrong format (%d) or length (%d)\n"),
                        selection_data->format,
                        selection_data->length);
            return;
          }

        color.red = data[0];
        color.green = data[1];
        color.blue = data[2];

        profile = terminal_screen_get_profile (screen);

        if (profile)
          {
            terminal_profile_set_background_type (profile,
                                                  TERMINAL_BACKGROUND_SOLID);
            terminal_profile_set_use_theme_colors (profile, FALSE);
            
            terminal_profile_get_color_scheme (profile,
                                               &fg, NULL);
            
            terminal_profile_set_color_scheme (profile, &fg, &color);
          }
      }
      break;

    case TARGET_MOZ_URL:
      {
        GString *str;
        int i;
        const guint16 *char_data;
        int char_len;
        char *filename;
        
        /* MOZ_URL is in UCS-2 but in format 8. BROKEN!
         *
         * The data contains the URL, a \n, then the
         * title of the web page.
         */
        if (selection_data->format != 8 ||
            selection_data->length == 0 ||
            (selection_data->length % 2) != 0)
          {
            g_printerr (_("Mozilla url dropped on terminal had wrong format (%d) or length (%d)\n"),
                        selection_data->format,
                        selection_data->length);
            return;
          }

        str = g_string_new (NULL);
        
        char_len = selection_data->length / 2;
        char_data = (const guint16*) selection_data->data;
        i = 0;
        while (i < char_len)
          {
            if (char_data[i] == '\n')
              break;
            
            g_string_append_unichar (str, (gunichar) char_data[i]);
            
            ++i;
          }

        /* drop file:///, else paste in URI literally */
        filename = g_filename_from_uri (str->str, NULL, NULL);
        
        /* FIXME just brazenly ignoring encoding issues, sending
         * child some UTF-8
         */
        if (filename)
          terminal_widget_write_data_to_child (screen->priv->term,
                                               filename, strlen (filename));
        else
          terminal_widget_write_data_to_child (screen->priv->term,
                                               str->str,
                                               str->len);

        g_free (filename);        
        g_string_free (str, TRUE);
      }
      break;
      
    case TARGET_URI_LIST:
      {
        char *uri_list;
        char **uris;
        int i;
        
        if (selection_data->format != 8 ||
            selection_data->length == 0)
          {
            g_printerr (_("URI list dropped on terminal had wrong format (%d) or length (%d)\n"),
                        selection_data->format,
                        selection_data->length);
            return;
          }
        
        uri_list = g_strndup (selection_data->data,
                              selection_data->length);

	uris = g_strsplit (uri_list, "\r\n", 0);

        i = 0;
        while (uris && uris[i])
          {
            char *old;
            
            old = uris[i];

            uris[i] = g_filename_from_uri (old, NULL, NULL);

            /* If the URI wasn't a filename, then pass it through
             * as a URI, so you can DND from Mozilla or whatever
             */
            if (uris[i] == NULL)
              uris[i] = old;
            else
              g_free (old);
            
            ++i;
          }

        if (uris)
          {
            char *flat;
            
            flat = g_strjoinv (" ", uris);
            terminal_widget_write_data_to_child (screen->priv->term,
                                                 flat, strlen (flat));
            g_free (flat);
          }

        g_strfreev (uris);
      }
      break;
      
    case TARGET_BGIMAGE:
      {
        char *uri_list;
        char **uris;
        gboolean exactly_one;
        
        if (selection_data->format != 8 ||
            selection_data->length == 0)
          {
            g_printerr (_("Image filename dropped on terminal had wrong format (%d) or length (%d)\n"),
                        selection_data->format,
                        selection_data->length);
            return;
          }
        
        uri_list = g_strndup (selection_data->data,
                              selection_data->length);

	uris = g_strsplit (uri_list, "\r\n", 0);

	exactly_one = uris[0] != NULL && (uris[1] == NULL || uris[1][0] == '\0');

        if (exactly_one)
          {
            TerminalProfile *profile;
            char *filename;
            GError *err;

            err = NULL;
            filename = g_filename_from_uri (uris[0],
                                            NULL,
                                            &err);

            if (err)
              {
                g_printerr (_("Error converting URI \"%s\" into filename: %s\n"),
                            uris[0], err->message);

                g_error_free (err);
              }

            profile = terminal_screen_get_profile (screen);
            
            if (filename && profile)
              {
                terminal_profile_set_background_type (profile,
                                                      TERMINAL_BACKGROUND_IMAGE);
                
                terminal_profile_set_background_image_file (profile,
                                                            filename);
              }

            g_free (filename);
          }

        g_strfreev (uris);
        g_free (uri_list);
      }
      break;
    case TARGET_RESET_BG:
      {
        TerminalProfile *profile;

        profile = terminal_screen_get_profile (screen);
        
        if (profile)
          {
            terminal_profile_set_background_type (profile,
                                                  TERMINAL_BACKGROUND_SOLID);
          }
      }
      break;
    }
}

static void
terminal_screen_setup_dnd (TerminalScreen *screen)
{
  static GtkTargetEntry target_table[] = {
    { "application/x-color", 0, TARGET_COLOR },
    { "property/bgimage",    0, TARGET_BGIMAGE },
    { "x-special/gnome-reset-background", 0, TARGET_RESET_BG },
    { "text/uri-list",  0, TARGET_URI_LIST },
    { "text/x-moz-url",  0, TARGET_MOZ_URL },
    { "UTF8_STRING", 0, TARGET_UTF8_STRING },
    { "COMPOUND_TEXT", 0, TARGET_COMPOUND_TEXT },
    { "TEXT", 0, TARGET_TEXT },
    { "STRING",     0, TARGET_STRING },
    /* text/plain problematic, we don't know its encoding */
    { "text/plain", 0, TARGET_TEXT_PLAIN }
    /* add when gtk supports it perhaps */
    /* { "text/unicode", 0, TARGET_TEXT_UNICODE } */
  };
  
  g_signal_connect (G_OBJECT (screen->priv->term), "drag_data_received",
                    G_CALLBACK (drag_data_received), screen);
  
  gtk_drag_dest_set (GTK_WIDGET (screen->priv->term),
                     GTK_DEST_DEFAULT_MOTION |
                     GTK_DEST_DEFAULT_HIGHLIGHT |
                     GTK_DEST_DEFAULT_DROP,
                     target_table, G_N_ELEMENTS (target_table),
                     GDK_ACTION_COPY);
}

/* Indices of some xlfd string entries.  The first entry is 1 */
#define XLFD_WEIGHT_INDEX			3
#define XLFD_SLANT_INDEX			4	
#define XLFD_SIZE_IN_PIXELS_INDEX		7	
#define XLFD_SIZE_IN_POINTS_INDEX		8	
#define XLFD_HORIZONTAL_RESOLUTION_INDEX	9	
#define XLFD_VERTICAL_RESOLUTION_INDEX		10	
#define XLFD_SPACING_INDEX		        11

#define XLFD_N_FIELDS 14

static char*
xlfd_get_nth_field (const char *xlfd,
                    int         n)
{
  char **split;
  char *ret;
  int i;
  
  /* Remember fontsets with the appended ",-*-" and stuff
   * like that...
   */
  
  split = g_strsplit (xlfd, "-", XLFD_N_FIELDS);

  i = 0;
  while (i < n)
    {
      if (split[i] == NULL)
        return NULL;

      ++i;
    }
  
  ret = g_strdup (split[n]);
  
  g_strfreev (split);

  return ret;
}

static char*
xlfd_replace_nth_field (const char *xlfd,
                        int         n,
                        const char *new_value)
{
  char **split;
  char *ret;
  int i;
  
  /* Remember fontsets with the appended ",-*-" and stuff
   * like that...
   */
  
  split = g_strsplit (xlfd, "-", XLFD_N_FIELDS);

  i = 0;
  while (i < n)
    {
      if (split[i] == NULL)
        return NULL;

      ++i;
    }

  g_free (split[n]);
  split[n] = g_strdup (new_value);
  
  ret = g_strjoinv ("-", split);
  
  g_strfreev (split);

  return ret;
}

static gboolean
xfont_is_monospace (const char *fontname)
{
  char *spacing;
  gboolean ret;
  
  spacing = xlfd_get_nth_field (fontname, XLFD_SPACING_INDEX);

  if (spacing == NULL)
    return FALSE;

  ret = (*spacing == 'm' || *spacing == 'c');

  g_free (spacing);

  return ret;
}

static char*
make_xfont_monospace (const char *fontname)
{
  char *ret;
  
  ret = xlfd_replace_nth_field (fontname, XLFD_SPACING_INDEX, "m");
  if (ret == NULL)
    ret = g_strdup (fontname);

  return ret;
}

static char*
make_xfont_char_cell (const char *fontname)
{
  char *ret;
  
  ret = xlfd_replace_nth_field (fontname, XLFD_SPACING_INDEX, "c");
  if (ret == NULL)
    ret = g_strdup (fontname);

  return ret;
}

static char*
make_xfont_have_size_from_other_font (const char *fontname,
                                      const char *other_font)
{
  char *size_pixels;
  char *size_points;
  char *ret;

  ret = NULL;
  size_pixels = xlfd_get_nth_field (other_font, XLFD_SIZE_IN_PIXELS_INDEX);
  size_points = xlfd_get_nth_field (other_font, XLFD_SIZE_IN_POINTS_INDEX);

  if (size_pixels && size_points)
    {
      ret = xlfd_replace_nth_field (fontname, XLFD_SIZE_IN_PIXELS_INDEX,
                                    size_pixels);
      if (ret)
        {
          char *tmp;

          tmp = xlfd_replace_nth_field (ret, XLFD_SIZE_IN_POINTS_INDEX,
                                        size_points);

          if (tmp)
            {
              g_free (ret);
              ret = tmp;
            }
        }
    }

  g_free (size_pixels);
  g_free (size_points);

  return ret;
}

static PangoFontDescription*
make_font_monospace (const PangoFontDescription *font)
{
  PangoFontDescription *ret;

  ret = pango_font_description_copy (font);

  /* FIXME */
  pango_font_description_set_family (ret, "monospace");
  
  return ret;
}

static GdkFont*
load_fonset_without_error (const gchar *fontset_name)
{
  XFontSet fontset;
  int  missing_charset_count;
  char **missing_charset_list;
  char *def_string;
  GdkFont *ret;
  
  fontset = XCreateFontSet (GDK_DISPLAY (), fontset_name,
			    &missing_charset_list, &missing_charset_count,
			    &def_string);

  if (missing_charset_count)
    {
      /* The whole point of this function is not to print the below stuff,
       * it's just here because I might change the function to optionally
       * return these things in a GError
       */
      
      int i;
      GString *str;

      str = g_string_new (NULL);
      g_string_append_printf (str,
                              "The font \"%s\" does not support all the required character sets for the current locale \"%s\"\n",
                              fontset_name, setlocale (LC_ALL, NULL));
      for (i=0;i<missing_charset_count;i++)
	g_string_append_printf (str,
                                "  (Missing character set \"%s\")\n",
                                missing_charset_list[i]);
      XFreeStringList (missing_charset_list);
      
      /* g_printerr ("%s", str->str); */

      g_string_free (str, TRUE);
    }

  if (fontset == NULL)
    return NULL;
  
  /* This should succeed, and be fast since we still have "fontset" open */     
  ret = gdk_fontset_load (fontset_name);

  XFreeFontSet (GDK_DISPLAY (), fontset);
  
  return ret;
}
