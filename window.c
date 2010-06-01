#include "owl.h"

#include <assert.h>

struct _owl_window { /*noproto*/
  GObject object;
  /* hierarchy information */
  owl_window *parent;
  owl_window *child;
  owl_window *next, *prev;
  /* flags */
  int dirty : 1;
  int dirty_subtree : 1;
  int shown : 1;
  int is_screen : 1;
  /* window information */
  WINDOW *win;
  PANEL *pan;
  int nlines, ncols;
  int begin_y, begin_x;
};

enum {
  REDRAW,
  RESIZED,
  LAST_SIGNAL
};

static guint window_signals[LAST_SIGNAL] = { 0 };

static void owl_window_dispose(GObject *gobject);
static void owl_window_finalize(GObject *gobject);

static owl_window *_owl_window_new(owl_window *parent, int nlines, int ncols, int begin_y, int begin_x);

static void _owl_window_link(owl_window *w, owl_window *parent);

static void _owl_window_create_curses(owl_window *w);
static void _owl_window_destroy_curses(owl_window *w);

static void _owl_window_realize(owl_window *w);
static void _owl_window_unrealize(owl_window *w);

static void _owl_window_redraw_cleanup(owl_window *w, WINDOW *win);

static owl_window *cursor_owner;

G_DEFINE_TYPE (OwlWindow, owl_window, G_TYPE_OBJECT)

static void owl_window_class_init (OwlWindowClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  /* Set up the vtabl */
  gobject_class->dispose = owl_window_dispose;
  gobject_class->finalize = owl_window_finalize;

  klass->redraw = _owl_window_redraw_cleanup;
  klass->resized = NULL;

  /* Create the signals, remember IDs */
  window_signals[REDRAW] =
    g_signal_new("redraw",
                 G_TYPE_FROM_CLASS(gobject_class),
                 G_SIGNAL_RUN_CLEANUP,
                 G_STRUCT_OFFSET(OwlWindowClass, redraw),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__POINTER,
                 G_TYPE_NONE,
                 1,
                 G_TYPE_POINTER, NULL);

  /* TODO: maybe type should be VOID__INT_INT_INT_INT; will need to generate a
   * marshaller */
  window_signals[RESIZED] =
    g_signal_new("resized",
                 G_TYPE_FROM_CLASS(gobject_class),
                 G_SIGNAL_RUN_FIRST,
                 G_STRUCT_OFFSET(OwlWindowClass, resized),
                 NULL, NULL,
                 g_cclosure_marshal_VOID__VOID,
                 G_TYPE_NONE,
                 0,
                 NULL);
}

static void owl_window_dispose (GObject *object)
{
  owl_window *w = OWL_WINDOW (object);

  /* Unmap the window */
  owl_window_hide (w);

  /* Unlink and unref all children */
  while (w->child) {
    owl_window *child = w->child;
    owl_window_unlink (child);
  }

  /* Remove from hierarchy */
  owl_window_unlink (w);

  G_OBJECT_CLASS (owl_window_parent_class)->dispose (object);
}

static void owl_window_finalize (GObject *object)
{
  owl_window *w = OWL_WINDOW(object);

  if (w->pan) {
    del_panel(w->pan);
    w->pan = NULL;
  }

  G_OBJECT_CLASS (owl_window_parent_class)->finalize (object);
}

static void owl_window_init (owl_window *w)
{
}

/** singletons **/

static WINDOW *_dummy_window(void)
{
  static WINDOW *dummy = NULL;
  if (!dummy) {
    dummy = newwin(1, 1, 0, 0);
  }
  return dummy;
}

owl_window *owl_window_get_screen(void)
{
  static owl_window *screen = NULL;
  if (!screen) {
    /* The screen is magical. It's 'shown', but the only mean of it going
     * invisible is if we're tore down curses (i.e. screen resize) */
    screen = _owl_window_new(NULL, g.lines, g.cols, 0, 0);
    screen->is_screen = 1;
    owl_window_show(screen);
  }
  return screen;
}

/** Creation and Destruction **/

owl_window *owl_window_new(owl_window *parent)
{
  if (!parent)
    parent = owl_window_get_screen();
  return _owl_window_new(parent, 0, 0, 0, 0);
}

static owl_window *_owl_window_new(owl_window *parent, int nlines, int ncols, int begin_y, int begin_x)
{
  owl_window *w;

  w = g_object_new (OWL_TYPE_WINDOW, NULL);
  if (w == NULL) g_error("Failed to create owl_window instance");

  w->nlines = nlines;
  w->ncols = ncols;
  w->begin_y = begin_y;
  w->begin_x = begin_x;

  _owl_window_link(w, parent);
  if (parent && parent->is_screen) {
    w->pan = new_panel(_dummy_window());
    set_panel_userptr(w->pan, w);
  }

  return w;
}

/** Hierarchy **/

void owl_window_unlink(owl_window *w)
{
  /* make sure the window is unmapped first */
  _owl_window_unrealize(w);
  /* unlink parent/child information */
  if (w->parent) {
    if (w->prev)
      w->prev->next = w->next;
    if (w->next)
      w->next->prev = w->prev;
    if (w->parent->child == w)
      w->parent->child = w->next;
    w->parent = NULL;
    g_object_unref (w);
  }
}

static void _owl_window_link(owl_window *w, owl_window *parent)
{
  if (w->parent == parent)
    return;

  owl_window_unlink(w);
  if (parent) {
    w->parent = parent;
    w->next = parent->child;
    parent->child = w;
    g_object_ref (w);
  }
}

/* mimic g_list_foreach for consistency */
void owl_window_children_foreach(owl_window *parent, GFunc func, gpointer user_data)
{
  owl_window *w;
  for (w = parent->child;
       w != NULL;
       w = w->next) {
    func(w, user_data);
  }
}

owl_window *owl_window_get_parent(owl_window *w)
{
  return w->parent;
}

/** Internal window management **/

static void _owl_window_create_curses(owl_window *w)
{
  if (w->is_screen) {
    resizeterm(w->nlines, w->ncols);
    w->win = stdscr;
  } else if (w->pan) {
    w->win = newwin(w->nlines, w->ncols, w->begin_y, w->begin_x);
    replace_panel(w->pan, w->win);
  } else {
    if (w->parent == NULL || w->parent->win == NULL)
      return;
    w->win = derwin(w->parent->win, w->nlines, w->ncols, w->begin_y, w->begin_x);
  }
}

static void _owl_window_destroy_curses(owl_window *w)
{
  if (w->is_screen) {
    /* don't deallocate the dummy */
    w->win = NULL;
  } else {
    if (w->pan) {
      /* panels assume their windows always exist, so we put in a fake one */
      replace_panel(w->pan, _dummy_window());
    }
    if (w->win) {
      /* and destroy our own window */
      delwin(w->win);
      w->win = NULL;
    }
  }
}

void owl_window_show(owl_window *w)
{
  w->shown = 1;
  _owl_window_realize(w);
  if (w->pan)
    show_panel(w->pan);
}

void owl_window_show_all(owl_window *w)
{
  owl_window_show(w);
  owl_window_children_foreach(w, (GFunc)owl_window_show, 0);
}

void owl_window_hide(owl_window *w)
{
  /* you can't unmap the screen */
  if (w->is_screen)
    return;
  w->shown = 0;
  if (w->pan)
    hide_panel(w->pan);
  _owl_window_unrealize(w);
}

int owl_window_is_shown(owl_window *w)
{
  return w->shown;
}

int owl_window_is_realized(owl_window *w)
{
  return w->win != NULL;
}

int owl_window_is_toplevel(owl_window *w)
{
  return w->pan != NULL;
}

static void _owl_window_realize(owl_window *w)
{
  /* check if we can create a window */
  if ((w->parent && w->parent->win == NULL)
      || !w->shown
      || w->win != NULL)
    return;
  _owl_window_create_curses(w);
  if (w->win == NULL)
    return;
  /* schedule a repaint */
  owl_window_dirty(w);
  /* map the children */
  owl_window_children_foreach(w, (GFunc)_owl_window_realize, 0);
}

static void _owl_window_unrealize(owl_window *w)
{
  if (w->win == NULL)
    return;
  /* unmap all the children */
  owl_window_children_foreach(w, (GFunc)_owl_window_unrealize, 0);
  _owl_window_destroy_curses(w);
  w->dirty = w->dirty_subtree = 0;
  /* subwins leave a mess in the parent; dirty it */
  if (w->parent)
    owl_window_dirty(w->parent);
}

/** Painting and book-keeping **/

void owl_window_set_cursor(owl_window *w)
{
  cursor_owner = w;
  g_object_add_weak_pointer(G_OBJECT(w), (gpointer*) &cursor_owner);
}

void owl_window_dirty(owl_window *w)
{
  /* don't put the screen on this list; pointless */
  if (w->is_screen)
    return;
  if (!owl_window_is_realized(w))
    return;
  if (!w->dirty) {
    w->dirty = 1;
    while (w && !w->dirty_subtree) {
      w->dirty_subtree = 1;
      w = w->parent;
    }
    owl_global_set_needrefresh(&g);
  }
}

void owl_window_dirty_children(owl_window *w)
{
  owl_window_children_foreach(w, (GFunc)owl_window_dirty, 0);
}

static void _owl_window_redraw(owl_window *w)
{
  if (!w->dirty) return;
  if (w->win) {
    g_signal_emit(w, window_signals[REDRAW], 0, w->win);
  }
  w->dirty = 0;
}

static void _owl_window_redraw_subtree(owl_window *w)
{
  if (!w->dirty_subtree)
    return;
  _owl_window_redraw(w);
  owl_window_children_foreach(w, (GFunc)_owl_window_redraw_subtree, 0);
}

/*
Redraw all the windows with scheduled redraws.
NOTE: This function shouldn't be called outside the event loop
*/
void owl_window_redraw_scheduled(void)
{
  _owl_window_redraw_subtree(owl_window_get_screen());
  update_panels();
  if (cursor_owner && cursor_owner->win)
    wnoutrefresh(cursor_owner->win);
}

static void _owl_window_redraw_cleanup(owl_window *w, WINDOW *win)
{
  wsyncup(win);
}

void owl_window_erase_cb(owl_window *w, WINDOW *win, void *user_data)
{
  werase(win);
  owl_window_dirty_children(w);
}

/** Window position **/

void owl_window_get_position(owl_window *w, int *nlines, int *ncols, int *begin_y, int *begin_x)
{
  if (nlines)  *nlines  = w->nlines;
  if (ncols)   *ncols   = w->ncols;
  if (begin_y) *begin_y = w->begin_y;
  if (begin_x) *begin_x = w->begin_x;
}

void owl_window_move(owl_window *w, int begin_y, int begin_x)
{
  if (w->is_screen) return; /* can't move the screen */
  if (w->begin_y == begin_y && w->begin_x == begin_x) return;

  w->begin_y = begin_y;
  w->begin_x = begin_x;
  if (w->shown) {
    /* Window is shown, we must try to have a window at the end */
    if (w->win) {
      /* We actually do have a window; let's move it */
      if (w->pan) {
        if (move_panel(w->pan, begin_y, begin_x) == OK)
          return;
      } else {
        if (mvderwin(w->win, begin_y, begin_x) == OK) {
          /* now both we and the parent are dirty */
          owl_window_dirty(w->parent);
          owl_window_dirty(w);
          return;
        }
      }
    }
    /* We don't have a window or failed to move it. Fine. Brute force. */
    _owl_window_unrealize(w);
    _owl_window_realize(w);
  }
}

void owl_window_set_position(owl_window *w, int nlines, int ncols, int begin_y, int begin_x)
{
  /* can't move the screen */
  if (w->is_screen) {
    begin_y = begin_x = 0;
  }

  if (w->nlines == nlines && w->ncols == ncols) {
    /* moving is easier */
    owl_window_move(w, begin_y, begin_x);
    return;
  }

  w->begin_y = begin_y;
  w->begin_x = begin_x;
  w->nlines = nlines;
  w->ncols = ncols;
  /* window is shown, we must try to have a window at the end */
  if (w->shown) {
    /* resizing in ncurses is hard: give up do a unrealize/realize */
    _owl_window_unrealize(w);
  }
  g_signal_emit(w, window_signals[RESIZED], 0);
  if (w->shown) {
    _owl_window_realize(w);
  }
}

void owl_window_resize(owl_window *w, int nlines, int ncols)
{
  owl_window_set_position(w, nlines, ncols, w->begin_y, w->begin_x);
}

/** Stacking order **/

void owl_window_top(owl_window *w) {
  if (!w->pan) {
    owl_function_debugmsg("Warning: owl_window_top only makes sense on top-level window");
    return;
  }
  top_panel(w->pan);
}

owl_window *owl_window_above(owl_window *w) {
  PANEL *pan;

  if (!w->pan) {
    owl_function_debugmsg("Warning: owl_window_above only makes sense on top-level window");
    return NULL;
  }
  pan = panel_above(w->pan);
  /* cast because panel_userptr pointlessly returns a const void * */
  return pan ? (void*) panel_userptr(pan) : NULL;
}

owl_window *owl_window_below(owl_window *w) {
  PANEL *pan;

  if (!w->pan) {
    owl_function_debugmsg("Warning: owl_window_above only makes sense on top-level window");
    return NULL;
  }
  pan = panel_below(w->pan);
  /* cast because panel_userptr pointlessly returns a const void * */
  return pan ? (void*) panel_userptr(pan) : NULL;
}
