/*
 * Copyright (C) 2007 OpenedHand Ltd
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <config.h>
#include <gtk/gtk.h>
#include <string.h>
#include "taku-table.h"
#include "taku-tile.h"

G_DEFINE_TYPE (TakuTable, taku_table, GTK_TYPE_GRID);

#define GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), TAKU_TYPE_TABLE, TakuTablePrivate))

#define DEFAULT_WIDTH 30

struct _TakuTablePrivate
{
  int columns;

  int x, y;
  gboolean reflowing;

  GSequence *seq;

  gpointer filter;

  GList *dummies;

  GtkIMContext *im_context;
};

/* Compare two tiles lexographically */
static int
compare_tiles (gconstpointer a,
               gconstpointer b,
               gpointer      user_data)
{
  const char *ka, *kb;

  ka = taku_tile_get_sort_key (TAKU_TILE (a));
  kb = taku_tile_get_sort_key (TAKU_TILE (b));

  if (ka != NULL && kb == NULL)
    return 1;
  else if (ka == NULL && kb != NULL)
    return -1;
  else if (ka == NULL && kb == NULL)
    return 0;
  else
    return strcmp (ka, kb);
}

/* Compare two tiles exactly */
static int
compare_tiles_exact (gconstpointer a,
                     gconstpointer b,
                     gpointer      user_data)
{
  int ret = compare_tiles (a, b, user_data);

  if (ret == 0) {
    if (a < b)
      ret = -1;
    else if (a > b)
      ret = 1;
  }

  return ret;
}

/* Normalize strings for case and representation insensitive comparison */
static char *
utf8_normalize_and_casefold (const char *str)
{
  char *norm = g_utf8_normalize (str, -1, G_NORMALIZE_ALL);
  char *fold = g_utf8_casefold (str, -1);
  g_free (norm);
  return fold;
}

/* Process keypress: Focus next tile which's primary text starts with the
 * entered character */
static void
im_context_commit_cb (GtkIMContext *context,
                      const char   *str,
                      gpointer      user_data)
{
  TakuTable *table = TAKU_TABLE (user_data);
  GSequenceIter *begin_iter, *iter;
  GtkWidget *toplevel, *focused;
  char *norm_str;

  norm_str = utf8_normalize_and_casefold (str);

  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (table));
  focused = gtk_window_get_focus (GTK_WINDOW (toplevel));
  if (focused)
    begin_iter = g_sequence_search (table->priv->seq, 
                                      focused, compare_tiles, NULL);
  else
    begin_iter = g_sequence_get_begin_iter (table->priv->seq);

  iter = begin_iter;
  do {
    TakuTile *tile;
    const char *text;
    char *norm_text;

    if (g_sequence_iter_is_end (iter)) {
      iter = g_sequence_get_begin_iter (table->priv->seq);
      if (iter == begin_iter)
        break;
    }

    tile = g_sequence_get (iter);
    if (!gtk_widget_get_visible (GTK_WIDGET (tile)))
      goto next;

    text = taku_tile_get_search_key (tile);
    if (text == NULL)
      goto next;

    norm_text = utf8_normalize_and_casefold (text);

    if (strncmp (norm_str, norm_text, strlen (norm_str)) == 0) {
      g_free (norm_text);
      gtk_widget_grab_focus (GTK_WIDGET (tile));
      break;
    }

    g_free (norm_text);

next:
    iter = g_sequence_iter_next (iter);
  } while (iter != begin_iter);

  g_free (norm_str);
}

static gboolean
on_tile_focus (GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  GtkWidget *container = GTK_WIDGET (user_data);
  GtkWidget *viewport = gtk_widget_get_parent (container);
  GtkAllocation widget_alloc, viewport_alloc;
  GtkAdjustment *adjustment;

  gtk_widget_get_allocation (widget, &widget_alloc);
  gtk_widget_get_allocation (viewport, &viewport_alloc);

  /* If the lowest point of the tile is lower than the height of the viewport,
   * or if the top of the tile is higher than the viewport is... */
  if (widget_alloc.y +
      widget_alloc.height > viewport_alloc.height ||
      widget_alloc.y < viewport_alloc.height) {
    adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (viewport));

    gtk_adjustment_clamp_page (adjustment,
                               widget_alloc.y,
                               widget_alloc.y +
                               widget_alloc.height);
  }

  return FALSE;
}

static void
reflow_foreach (gpointer widget, gpointer user_data)
{
  TakuTile *tile = TAKU_TILE (widget);
  TakuTable *table = TAKU_TABLE (user_data);
  GtkContainer *container = GTK_CONTAINER (user_data);

  /* Filter out unwanted items */
  if (table->priv->filter != NULL) {
    if (!taku_tile_matches_filter (tile, table->priv->filter)) {
      gtk_container_child_set (container, GTK_WIDGET (widget),
                               "left-attach", 0,
                               "top-attach", 0,
                               "width", 1,
                               "height", 1,
                               NULL);
      gtk_widget_hide (widget);
      return;
    }
  }

  /* We want this item. Align. */
  gtk_widget_show (widget);

  gtk_container_child_set (container, GTK_WIDGET (widget),
                           "left-attach", table->priv->x,
                           "top-attach", table->priv->y,
                           "width", 1,
                           "height", 1,
                           NULL);
  if (++table->priv->x >= table->priv->columns) {
    table->priv->x = 0;
    table->priv->y++;
  }
}

static void
reflow (TakuTable *table)
{
  int i;

  /* Only reflow when necessary */
  if (!gtk_widget_get_realized (GTK_WIDGET (table)))
    return;

  /* Remove dummies */
  while (table->priv->dummies) {
    /* Call into the parent class straight away in order to bypass our
     * own remove implementation. */
    (* GTK_CONTAINER_CLASS (taku_table_parent_class)->remove)
      (GTK_CONTAINER (table), table->priv->dummies->data);

    table->priv->dummies = g_list_delete_link (table->priv->dummies,
                                               table->priv->dummies);
  }

  /* Reflow children */
  table->priv->x = table->priv->y = 0;

  table->priv->reflowing = TRUE;
  g_sequence_foreach (table->priv->seq, reflow_foreach, table);
  table->priv->reflowing = FALSE;

  /* Add dummy items if necessary to get required amount of columns */
  for (i = g_sequence_get_length (table->priv->seq);
       i < table->priv->columns; i++) {
    GtkWidget *dummy = gtk_label_new (NULL);
    gtk_widget_show (dummy);

    gtk_grid_attach (GTK_GRID (table),
                     dummy,
                     table->priv->x, table->priv->y,
                     1, 1);
    table->priv->x++;

    table->priv->dummies = g_list_prepend (table->priv->dummies, dummy);
  }
}

/*
 * Implementation of gtk_container_add, so that applications can just call that
 * and this class manages the position.
 */
static void
container_add (GtkContainer *container, GtkWidget *widget)
{
  TakuTable *self = TAKU_TABLE (container);

  g_return_if_fail (self);
  g_return_if_fail (TAKU_IS_TILE (widget));

  g_sequence_insert_sorted (self->priv->seq, widget, compare_tiles, NULL);

  gtk_grid_attach (GTK_GRID (container), widget, 0, 1, 1, 1);

  reflow (self);

  g_signal_connect (widget, "focus-in-event", G_CALLBACK (on_tile_focus), self);
}

static void
container_remove (GtkContainer *container, GtkWidget *widget)
{
  TakuTable *self = TAKU_TABLE (container);
  GSequenceIter *iter;

  g_return_if_fail (self);

  /* Find the appropriate iter first */
  iter = g_sequence_search (self->priv->seq,
                              widget, compare_tiles_exact, NULL);
  iter = g_sequence_iter_prev (iter);
  if (g_sequence_iter_is_end (iter) || g_sequence_get (iter) != widget) {
    /* We have here a dummy, or something that is not contained */
    (* GTK_CONTAINER_CLASS (taku_table_parent_class)->remove)
                                                (container, widget);

    return;
  }

  /* And then remove it */
  g_sequence_remove (iter);

  (* GTK_CONTAINER_CLASS (taku_table_parent_class)->remove) (container, widget);

  reflow (self);
}

static void
calculate_columns (GtkWidget *widget)
{
  TakuTable *table = TAKU_TABLE (widget);
  PangoContext *context;
  PangoFontMetrics *metrics;
  int width, new_cols;
  guint cell_text_width = DEFAULT_WIDTH;
  GtkAllocation allocation;

  gtk_widget_get_allocation (widget, &allocation);

  /* If we are currently reflowing the tiles, or the final allocation hasn't
     been decided yet, return */
  if (!gtk_widget_get_realized (widget) ||
      table->priv->reflowing ||
      allocation.width <= 1)
    return;

  context = gtk_widget_get_pango_context (widget);
  metrics = pango_context_get_metrics (context, gtk_widget_get_style (widget)->font_desc, NULL);

  gtk_widget_style_get (widget, "cell-text-width", &cell_text_width, NULL);

  width = PANGO_PIXELS
          (cell_text_width * pango_font_metrics_get_approximate_char_width (metrics));
  new_cols = MAX (1, allocation.width / width);

  if (table->priv->columns != new_cols) {
    table->priv->columns = new_cols;

    reflow (table);
  }

  pango_font_metrics_unref (metrics);
}

static void
taku_table_realize (GtkWidget *widget)
{
  TakuTable *self = TAKU_TABLE (widget);

  (* GTK_WIDGET_CLASS (taku_table_parent_class)->realize) (widget);

  gtk_im_context_set_client_window (self->priv->im_context, gtk_widget_get_window (widget));

  calculate_columns (widget);
}

static void
taku_table_unrealize (GtkWidget *widget)
{
  TakuTable *self = TAKU_TABLE (widget);

  gtk_im_context_set_client_window (self->priv->im_context, NULL);

  (* GTK_WIDGET_CLASS (taku_table_parent_class)->unrealize) (widget);
}

static void
on_size_allocate (GtkWidget     *widget,
                          GtkAllocation *allocation,
                          gpointer       user_data)
{
  calculate_columns (widget);
}

static void
taku_table_style_set (GtkWidget *widget,
                      GtkStyle  *previous_style)
{
  (* GTK_WIDGET_CLASS (taku_table_parent_class)->style_set)
    (widget, previous_style);

  calculate_columns (widget);
}

static int
taku_table_focus_in_event (GtkWidget *widget, GdkEventFocus *event)
{
  TakuTable *self = TAKU_TABLE (widget);

  gtk_im_context_focus_in (self->priv->im_context);

  (* GTK_WIDGET_CLASS (taku_table_parent_class)->focus_in_event)
    (widget, event);

  return FALSE;
}

static int
taku_table_focus_out_event (GtkWidget *widget, GdkEventFocus *event)
{
  TakuTable *self = TAKU_TABLE (widget);

  gtk_im_context_focus_out (self->priv->im_context);

  (* GTK_WIDGET_CLASS (taku_table_parent_class)->focus_out_event)
    (widget, event);

  return FALSE;
}

static gboolean
taku_table_key_press_event (GtkWidget   *widget,
                            GdkEventKey *event)
{
  TakuTable *table = TAKU_TABLE (widget);

  if (gtk_im_context_filter_keypress (table->priv->im_context, event))
    return TRUE;

  return (* GTK_WIDGET_CLASS (taku_table_parent_class)->key_press_event)
           (widget, event);
}

static void
taku_table_grab_focus (GtkWidget *widget)
{
  TakuTable *table = TAKU_TABLE (widget);
  GSequenceIter *iter;

  iter = g_sequence_get_begin_iter (table->priv->seq);
  while (!g_sequence_iter_is_end (iter)) {
    GtkWidget *widget = g_sequence_get (iter);

    if (gtk_widget_get_visible (widget)) {
      gtk_widget_grab_focus (widget);

      break;
    }

    iter = g_sequence_iter_next (iter);
  }
}

static void
taku_table_finalize (GObject *object)
{
  TakuTable *table = TAKU_TABLE (object);

  g_sequence_free (table->priv->seq);

  g_list_free (table->priv->dummies);

  g_object_unref (table->priv->im_context);

  G_OBJECT_CLASS (taku_table_parent_class)->finalize (object);
}

static void
taku_table_class_init (TakuTableClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkContainerClass *container_class = GTK_CONTAINER_CLASS (klass);

  g_type_class_add_private (klass, sizeof (TakuTablePrivate));

  object_class->finalize     = taku_table_finalize;

  widget_class->realize         = taku_table_realize;
  widget_class->unrealize       = taku_table_unrealize;
  widget_class->style_set       = taku_table_style_set;
  widget_class->focus_in_event  = taku_table_focus_in_event;
  widget_class->focus_out_event = taku_table_focus_out_event;
  widget_class->key_press_event = taku_table_key_press_event;
  widget_class->grab_focus      = taku_table_grab_focus;

  container_class->add    = container_add;
  container_class->remove = container_remove;

  gtk_widget_class_install_style_property (widget_class, g_param_spec_uint
                                           ("cell-text-width", "cell text width",
                                            "Width of the tiles in characters",
                                            0, G_MAXUINT, DEFAULT_WIDTH,
                                            G_PARAM_READABLE));

}

static void
taku_table_init (TakuTable *self)
{
  self->priv = GET_PRIVATE (self);

  g_signal_connect (self, "size-allocate", G_CALLBACK (on_size_allocate), NULL);

  g_object_set (self,
                "border-width", 6,
                "row-spacing", 6,
                "column-spacing", 6,
                "row-homogeneous", TRUE,
                "column-homogeneous", TRUE,
                NULL);

  self->priv->columns = 0;

  self->priv->reflowing = FALSE;

  self->priv->seq = g_sequence_new (NULL);

  self->priv->filter = NULL;

  self->priv->dummies = NULL;

  self->priv->im_context = gtk_im_multicontext_new ();
  g_signal_connect (self->priv->im_context, "commit",
                    G_CALLBACK (im_context_commit_cb), self);
}

GtkWidget *
taku_table_new (void)
{
  return g_object_new (TAKU_TYPE_TABLE, NULL);
}

void
taku_table_set_filter (TakuTable *table, gpointer filter)
{
  g_return_if_fail (TAKU_IS_TABLE (table));

  table->priv->filter = filter;

  reflow (table);
}

gpointer
taku_table_get_filter (TakuTable *table)
{
  g_return_val_if_fail (TAKU_IS_TABLE (table), NULL);

  return table->priv->filter;
}
