/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

event.c for the Openbox window manager
Copyright (c) 2003        Ben Jansens

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

See the COPYING file for a copy of the GNU General Public License.
*/

#include "event.h"
#include "debug.h"
#include "window.h"
#include "openbox.h"
#include "dock.h"
#include "client.h"
#include "xerror.h"
#include "prop.h"
#include "config.h"
#include "screen.h"
#include "frame.h"
#include "menu.h"
#include "menuframe.h"
#include "keyboard.h"
#include "mouse.h"
#include "mainloop.h"
#include "framerender.h"
#include "focus.h"
#include "moveresize.h"
#include "group.h"
#include "stacking.h"
#include "extensions.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <glib.h>

#ifdef HAVE_SYS_SELECT_H
#  include <sys/select.h>
#endif
#ifdef HAVE_SIGNAL_H
#  include <signal.h>
#endif

#ifdef USE_SM
#include <X11/ICE/ICElib.h>
#endif

static void event_process(const XEvent *e, gpointer data);
static void event_done(gpointer data);
static void event_client_dest(ObClient *client, gpointer data);
static void event_handle_root(XEvent *e);
static void event_handle_menu(XEvent *e);
static void event_handle_dock(ObDock *s, XEvent *e);
static void event_handle_dockapp(ObDockApp *app, XEvent *e);
static void event_handle_client(ObClient *c, XEvent *e);
static void event_handle_group(ObGroup *g, XEvent *e);

static gboolean focus_delay_func(gpointer data);
static void focus_delay_client_dest(ObClient *client, gpointer data);

static gboolean menu_hide_delay_func(gpointer data);

Time event_lasttime = 0;

/*! The value of the mask for the NumLock modifier */
unsigned int NumLockMask;
/*! The value of the mask for the ScrollLock modifier */
unsigned int ScrollLockMask;
/*! The key codes for the modifier keys */
static XModifierKeymap *modmap;
/*! Table of the constant modifier masks */
static const int mask_table[] = {
    ShiftMask, LockMask, ControlMask, Mod1Mask,
    Mod2Mask, Mod3Mask, Mod4Mask, Mod5Mask
};
static int mask_table_size;

static guint ignore_enter_focus = 0;

static gboolean menu_can_hide;

static ObClient *focus_in, *focus_out;

#ifdef USE_SM
static void ice_handler(int fd, gpointer conn)
{
    Bool b;
    IceProcessMessages(conn, NULL, &b);
}

static void ice_watch(IceConn conn, IcePointer data, Bool opening,
                      IcePointer *watch_data)
{
    static gint fd = -1;

    if (opening) {
        fd = IceConnectionNumber(conn);
        ob_main_loop_fd_add(ob_main_loop, fd, ice_handler, conn, NULL);
    } else {
        ob_main_loop_fd_remove(ob_main_loop, fd);
        fd = -1;
    }
}
#endif

void event_startup(gboolean reconfig)
{
    if (reconfig) return;

    mask_table_size = sizeof(mask_table) / sizeof(mask_table[0]);
     
    /* get lock masks that are defined by the display (not constant) */
    modmap = XGetModifierMapping(ob_display);
    g_assert(modmap);
    if (modmap && modmap->max_keypermod > 0) {
        size_t cnt;
        const size_t size = mask_table_size * modmap->max_keypermod;
        /* get the values of the keyboard lock modifiers
           Note: Caps lock is not retrieved the same way as Scroll and Num
           lock since it doesn't need to be. */
        const KeyCode num_lock = XKeysymToKeycode(ob_display, XK_Num_Lock);
        const KeyCode scroll_lock = XKeysymToKeycode(ob_display,
                                                     XK_Scroll_Lock);
	  
        for (cnt = 0; cnt < size; ++cnt) {
            if (! modmap->modifiermap[cnt]) continue;
	       
            if (num_lock == modmap->modifiermap[cnt])
                NumLockMask = mask_table[cnt / modmap->max_keypermod];
            if (scroll_lock == modmap->modifiermap[cnt])
                ScrollLockMask = mask_table[cnt / modmap->max_keypermod];
        }
    }

    ob_main_loop_x_add(ob_main_loop, event_process, event_done, NULL, NULL);

#ifdef USE_SM
    IceAddConnectionWatch(ice_watch, NULL);
#endif

    client_add_destructor(focus_delay_client_dest, NULL);
    client_add_destructor(event_client_dest, NULL);
}

void event_shutdown(gboolean reconfig)
{
    if (reconfig) return;

#ifdef USE_SM
    IceRemoveConnectionWatch(ice_watch, NULL);
#endif

    client_remove_destructor(focus_delay_client_dest);
    XFreeModifiermap(modmap);
}

static Window event_get_window(XEvent *e)
{
    Window window;

    /* pick a window */
    switch (e->type) {
    case SelectionClear:
        window = RootWindow(ob_display, ob_screen);
        break;
    case MapRequest:
        window = e->xmap.window;
        break;
    case UnmapNotify:
        window = e->xunmap.window;
        break;
    case DestroyNotify:
        window = e->xdestroywindow.window;
        break;
    case ConfigureRequest:
        window = e->xconfigurerequest.window;
        break;
    case ConfigureNotify:
        window = e->xconfigure.window;
        break;
    default:
#ifdef XKB
        if (extensions_xkb && e->type == extensions_xkb_event_basep) {
            switch (((XkbAnyEvent*)e)->xkb_type) {
            case XkbBellNotify:
                window = ((XkbBellNotifyEvent*)e)->window;
            default:
                window = None;
            }
        } else
#endif
            window = e->xany.window;
    }
    return window;
}

static void event_set_lasttime(XEvent *e)
{
    Time t = 0;

    /* grab the lasttime and hack up the state */
    switch (e->type) {
    case ButtonPress:
    case ButtonRelease:
        t = e->xbutton.time;
        break;
    case KeyPress:
        t = e->xkey.time;
        break;
    case KeyRelease:
        t = e->xkey.time;
        break;
    case MotionNotify:
        t = e->xmotion.time;
        break;
    case PropertyNotify:
        t = e->xproperty.time;
        break;
    case EnterNotify:
    case LeaveNotify:
        t = e->xcrossing.time;
        break;
    default:
        /* if more event types are anticipated, get their timestamp
           explicitly */
        break;
    }

    if (t > event_lasttime)
        event_lasttime = t;
}

#define STRIP_MODS(s) \
	s &= ~(LockMask | NumLockMask | ScrollLockMask), \
	/* kill off the Button1Mask etc, only want the modifiers */ \
	s &= (ControlMask | ShiftMask | Mod1Mask | \
              Mod2Mask | Mod3Mask | Mod4Mask | Mod5Mask) \

static void event_hack_mods(XEvent *e)
{
    KeyCode *kp;
    int i, k;

    switch (e->type) {
    case ButtonPress:
    case ButtonRelease:
        STRIP_MODS(e->xbutton.state);
        break;
    case KeyPress:
        STRIP_MODS(e->xkey.state);
        break;
    case KeyRelease:
        STRIP_MODS(e->xkey.state);
        /* remove from the state the mask of the modifier being released, if
           it is a modifier key being released (this is a little ugly..) */
        kp = modmap->modifiermap;
        for (i = 0; i < mask_table_size; ++i) {
            for (k = 0; k < modmap->max_keypermod; ++k) {
                if (*kp == e->xkey.keycode) { /* found the keycode */
                    /* remove the mask for it */
                    e->xkey.state &= ~mask_table[i];
                    /* cause the first loop to break; */
                    i = mask_table_size;
                    break; /* get outta here! */
                }
                ++kp;
            }
        }
        break;
    case MotionNotify:
        STRIP_MODS(e->xmotion.state);
        /* compress events */
        {
            XEvent ce;
            while (XCheckTypedWindowEvent(ob_display, e->xmotion.window,
                                          e->type, &ce)) {
                e->xmotion.x_root = ce.xmotion.x_root;
                e->xmotion.y_root = ce.xmotion.y_root;
            }
        }
        break;
    }
}

static gboolean event_ignore(XEvent *e, ObClient *client)
{
    switch(e->type) {
    case EnterNotify:
    case LeaveNotify:
        if (e->xcrossing.detail == NotifyInferior)
            return TRUE;
        break;
    case FocusIn:
        if (e->xfocus.detail > NotifyNonlinearVirtual)
            return TRUE;
        break;
    case FocusOut:
        if (e->xfocus.detail > NotifyNonlinearVirtual)
            return TRUE;
        if (e->xfocus.detail == NotifyInferior ||
            e->xfocus.mode == NotifyGrab)
            return TRUE;
        break;
    }
    return FALSE;
}

static void event_client_dest(ObClient *client, gpointer data)
{
    if (client == focus_in)
        focus_in = NULL;
    if (client == focus_out)
        focus_out = NULL;
}

static void event_done(gpointer data)
{
    static ObClient *last = NULL;

    if (focus_in) {
        if (focus_in != focus_client) {
            focus_set_client(focus_in);
            frame_adjust_focus(focus_in->frame, TRUE);
            client_calc_layer(focus_in);
        }
    } 
    if (focus_out) {
        if (focus_out == focus_client)
            focus_set_client(NULL);
        frame_adjust_focus(focus_out->frame, FALSE);
        client_calc_layer(focus_out);
    }

    if (focus_client != last) {
        if (!focus_client)
            focus_fallback(OB_FOCUS_FALLBACK_NOFOCUS);
        last = focus_client;
    }

    focus_in = focus_out = NULL;
}

static void event_process(const XEvent *ec, gpointer data)
{
    Window window;
    ObGroup *group = NULL;
    ObClient *client = NULL;
    ObDock *dock = NULL;
    ObDockApp *dockapp = NULL;
    ObWindow *obwin = NULL;
    XEvent ee, *e;

    /* make a copy we can mangle */
    ee = *ec;
    e = &ee;

    window = event_get_window(e);
    if (!(e->type == PropertyNotify &&
          (group = g_hash_table_lookup(group_map, &window))))
        if ((obwin = g_hash_table_lookup(window_map, &window))) {
            switch (obwin->type) {
            case Window_Dock:
                dock = WINDOW_AS_DOCK(obwin);
                break;
            case Window_DockApp:
                dockapp = WINDOW_AS_DOCKAPP(obwin);
                break;
            case Window_Client:
                client = WINDOW_AS_CLIENT(obwin);
                break;
            case Window_Menu:
            case Window_Internal:
                /* not to be used for events */
                g_assert_not_reached();
                break;
            }
        }

    event_set_lasttime(e);
    event_hack_mods(e);
    if (event_ignore(e, client))
        return;

    /* deal with it in the kernel */
    if (group)
        event_handle_group(group, e);
    else if (client)
        event_handle_client(client, e);
    else if (dockapp)
        event_handle_dockapp(dockapp, e);
    else if (dock)
        event_handle_dock(dock, e);
    else if (window == RootWindow(ob_display, ob_screen))
        event_handle_root(e);
    else if (e->type == MapRequest)
        client_manage(window);
    else if (e->type == ConfigureRequest) {
        /* unhandled configure requests must be used to configure the
           window directly */
        XWindowChanges xwc;
	       
        xwc.x = e->xconfigurerequest.x;
        xwc.y = e->xconfigurerequest.y;
        xwc.width = e->xconfigurerequest.width;
        xwc.height = e->xconfigurerequest.height;
        xwc.border_width = e->xconfigurerequest.border_width;
        xwc.sibling = e->xconfigurerequest.above;
        xwc.stack_mode = e->xconfigurerequest.detail;
       
        /* we are not to be held responsible if someone sends us an
           invalid request! */
        xerror_set_ignore(TRUE);
        XConfigureWindow(ob_display, window,
                         e->xconfigurerequest.value_mask, &xwc);
        xerror_set_ignore(FALSE);
    }

    /* user input (action-bound) events */
    if (e->type == ButtonPress || e->type == ButtonRelease ||
        e->type == MotionNotify || e->type == KeyPress ||
        e->type == KeyRelease)
    {
        if (menu_frame_visible)
            event_handle_menu(e);
        else {
            if (!keyboard_process_interactive_grab(e, &client)) {
                if (moveresize_in_progress) {
                    moveresize_event(e);

                    /* make further actions work on the client being
                       moved/resized */
                    client = moveresize_client;
                }

                menu_can_hide = FALSE;
                ob_main_loop_timeout_add(ob_main_loop,
                                         G_USEC_PER_SEC / 4,
                                         menu_hide_delay_func,
                                         NULL, NULL);

                if (e->type == ButtonPress || e->type == ButtonRelease ||
                    e->type == MotionNotify)
                    mouse_event(client, e);
                else if (e->type == KeyPress)
                    /* when in the middle of a focus cycling action, this
                       causes the window which appears to be focused to be
                       the one on which the actions will be executed */
                    keyboard_event((focus_cycle_target ?
                                    focus_cycle_target : client), e);
            }
        }
    }
}

static void event_handle_root(XEvent *e)
{
    Atom msgtype;
     
    switch(e->type) {
    case SelectionClear:
        ob_debug("Another WM has requested to replace us. Exiting.\n");
        ob_exit(0);
        break;

    case ClientMessage:
        if (e->xclient.format != 32) break;

        msgtype = e->xclient.message_type;
        if (msgtype == prop_atoms.net_current_desktop) {
            unsigned int d = e->xclient.data.l[0];
            if (d < screen_num_desktops)
                screen_set_desktop(d);
        } else if (msgtype == prop_atoms.net_number_of_desktops) {
            unsigned int d = e->xclient.data.l[0];
            if (d > 0)
                screen_set_num_desktops(d);
        } else if (msgtype == prop_atoms.net_showing_desktop) {
            screen_show_desktop(e->xclient.data.l[0] != 0);
        }
        break;
    case PropertyNotify:
        if (e->xproperty.atom == prop_atoms.net_desktop_names)
            screen_update_desktop_names();
        else if (e->xproperty.atom == prop_atoms.net_desktop_layout)
            screen_update_layout();
        break;
    case ConfigureNotify:
#ifdef XRANDR
        XRRUpdateConfiguration(e);
#endif
        screen_resize();
        break;
    default:
        ;
#ifdef VIDMODE
        if (extensions_vidmode && e->type == extensions_vidmode_event_basep) {
            ob_debug("VIDMODE EVENT\n");
        }
#endif
    }
}

static void event_handle_group(ObGroup *group, XEvent *e)
{
    GSList *it;

    g_assert(e->type == PropertyNotify);

    for (it = group->members; it; it = g_slist_next(it))
        event_handle_client(it->data, e);
}

void event_enter_client(ObClient *client)
{
    g_assert(config_focus_follow);

    if (client_normal(client) && client_can_focus(client)) {
        if (config_focus_delay) {
            ob_main_loop_timeout_remove(ob_main_loop, focus_delay_func);
            ob_main_loop_timeout_add(ob_main_loop,
                                     config_focus_delay,
                                     focus_delay_func,
                                     client, NULL);
        } else
            focus_delay_func(client);
    }
}

static void event_handle_client(ObClient *client, XEvent *e)
{
    XEvent ce;
    Atom msgtype;
    int i=0;
    ObFrameContext con;
     
    switch (e->type) {
    case VisibilityNotify:
        client->frame->obscured = e->xvisibility.state != VisibilityUnobscured;
        break;
    case ButtonPress:
    case ButtonRelease:
        /* Wheel buttons don't draw because they are an instant click, so it
           is a waste of resources to go drawing it. */
        if (!(e->xbutton.button == 4 || e->xbutton.button == 5)) {
            con = frame_context(client, e->xbutton.window);
            con = mouse_button_frame_context(con, e->xbutton.button);
            switch (con) {
            case OB_FRAME_CONTEXT_MAXIMIZE:
                client->frame->max_press = (e->type == ButtonPress);
                framerender_frame(client->frame);
                break;
            case OB_FRAME_CONTEXT_CLOSE:
                client->frame->close_press = (e->type == ButtonPress);
                framerender_frame(client->frame);
                break;
            case OB_FRAME_CONTEXT_ICONIFY:
                client->frame->iconify_press = (e->type == ButtonPress);
                framerender_frame(client->frame);
                break;
            case OB_FRAME_CONTEXT_ALLDESKTOPS:
                client->frame->desk_press = (e->type == ButtonPress);
                framerender_frame(client->frame);
                break; 
            case OB_FRAME_CONTEXT_SHADE:
                client->frame->shade_press = (e->type == ButtonPress);
                framerender_frame(client->frame);
                break;
            default:
                /* nothing changes with clicks for any other contexts */
                break;
            }
        }
        break;
    case FocusIn:
#ifdef DEBUG_FOCUS
        ob_debug("FocusIn on client for %lx (client %lx) mode %d detail %d\n",
                 e->xfocus.window, client->window,
                 e->xfocus.mode, e->xfocus.detail);
#endif
        focus_in = client;
        if (focus_out == client)
            focus_out = NULL;
        break;
    case FocusOut:
#ifdef DEBUG_FOCUS
        ob_debug("FocusOut on client for %lx (client %lx) mode %d detail %d\n",
                 e->xfocus.window, client->window,
                 e->xfocus.mode, e->xfocus.detail);
#endif
        if (focus_in == client)
            focus_in = NULL;
        if (client == focus_client)
            focus_out = client;
        break;
    case LeaveNotify:
        con = frame_context(client, e->xcrossing.window);
        switch (con) {
        case OB_FRAME_CONTEXT_MAXIMIZE:
            client->frame->max_hover = FALSE;
            frame_adjust_state(client->frame);
            break;
        case OB_FRAME_CONTEXT_ALLDESKTOPS:
            client->frame->desk_hover = FALSE;
            frame_adjust_state(client->frame);
            break;
        case OB_FRAME_CONTEXT_SHADE:
            client->frame->shade_hover = FALSE;
            frame_adjust_state(client->frame);
            break;
        case OB_FRAME_CONTEXT_ICONIFY:
            client->frame->iconify_hover = FALSE;
            frame_adjust_state(client->frame);
            break;
        case OB_FRAME_CONTEXT_CLOSE:
            client->frame->close_hover = FALSE;
            frame_adjust_state(client->frame);
            break;
        case OB_FRAME_CONTEXT_FRAME:
            /*
              if (config_focus_follow && config_focus_delay)
              ob_main_loop_timeout_remove_data(ob_main_loop,
              focus_delay_func,
              client);
            */
            break;
        default:
            break;
        }
        break;
    case EnterNotify:
    {
        gboolean nofocus = FALSE;

        if (ignore_enter_focus) {
            ignore_enter_focus--;
            nofocus = TRUE;
        }

        con = frame_context(client, e->xcrossing.window);
        switch (con) {
        case OB_FRAME_CONTEXT_MAXIMIZE:
            client->frame->max_hover = TRUE;
            frame_adjust_state(client->frame);
            break;
        case OB_FRAME_CONTEXT_ALLDESKTOPS:
            client->frame->desk_hover = TRUE;
            frame_adjust_state(client->frame);
            break;
        case OB_FRAME_CONTEXT_SHADE:
            client->frame->shade_hover = TRUE;
            frame_adjust_state(client->frame);
            break;
        case OB_FRAME_CONTEXT_ICONIFY:
            client->frame->iconify_hover = TRUE;
            frame_adjust_state(client->frame);
            break;
        case OB_FRAME_CONTEXT_CLOSE:
            client->frame->close_hover = TRUE;
            frame_adjust_state(client->frame);
            break;
        case OB_FRAME_CONTEXT_FRAME:
            if (e->xcrossing.mode == NotifyGrab ||
                e->xcrossing.mode == NotifyUngrab)
            {
#ifdef DEBUG_FOCUS
                ob_debug("%sNotify mode %d detail %d on %lx IGNORED\n",
                         (e->type == EnterNotify ? "Enter" : "Leave"),
                         e->xcrossing.mode,
                         e->xcrossing.detail, client?client->window:0);
#endif
            } else {
#ifdef DEBUG_FOCUS
                ob_debug("%sNotify mode %d detail %d on %lx, "
                         "focusing window: %d\n",
                         (e->type == EnterNotify ? "Enter" : "Leave"),
                         e->xcrossing.mode,
                         e->xcrossing.detail, (client?client->window:0),
                         !nofocus);
#endif
                if (!nofocus && config_focus_follow)
                    event_enter_client(client);
            }
            break;
        default:
            break;
        }
        break;
    }
    case ConfigureRequest:
        /* compress these */
        while (XCheckTypedWindowEvent(ob_display, client->window,
                                      ConfigureRequest, &ce)) {
            ++i;
            /* XXX if this causes bad things.. we can compress config req's
               with the same mask. */
            e->xconfigurerequest.value_mask |=
                ce.xconfigurerequest.value_mask;
            if (ce.xconfigurerequest.value_mask & CWX)
                e->xconfigurerequest.x = ce.xconfigurerequest.x;
            if (ce.xconfigurerequest.value_mask & CWY)
                e->xconfigurerequest.y = ce.xconfigurerequest.y;
            if (ce.xconfigurerequest.value_mask & CWWidth)
                e->xconfigurerequest.width = ce.xconfigurerequest.width;
            if (ce.xconfigurerequest.value_mask & CWHeight)
                e->xconfigurerequest.height = ce.xconfigurerequest.height;
            if (ce.xconfigurerequest.value_mask & CWBorderWidth)
                e->xconfigurerequest.border_width =
                    ce.xconfigurerequest.border_width;
            if (ce.xconfigurerequest.value_mask & CWStackMode)
                e->xconfigurerequest.detail = ce.xconfigurerequest.detail;
        }

        /* if we are iconic (or shaded (fvwm does this)) ignore the event */
        if (client->iconic || client->shaded) return;

        /* resize, then move, as specified in the EWMH section 7.7 */
        if (e->xconfigurerequest.value_mask & (CWWidth | CWHeight |
                                               CWX | CWY |
                                               CWBorderWidth)) {
            int x, y, w, h;
            ObCorner corner;

            if (e->xconfigurerequest.value_mask & CWBorderWidth)
                client->border_width = e->xconfigurerequest.border_width;

            x = (e->xconfigurerequest.value_mask & CWX) ?
                e->xconfigurerequest.x : client->area.x;
            y = (e->xconfigurerequest.value_mask & CWY) ?
                e->xconfigurerequest.y : client->area.y;
            w = (e->xconfigurerequest.value_mask & CWWidth) ?
                e->xconfigurerequest.width : client->area.width;
            h = (e->xconfigurerequest.value_mask & CWHeight) ?
                e->xconfigurerequest.height : client->area.height;

            {
                int newx = x;
                int newy = y;
                int fw = w +
                    client->frame->size.left + client->frame->size.right;
                int fh = h +
                    client->frame->size.top + client->frame->size.bottom;
                client_find_onscreen(client, &newx, &newy, fw, fh,
                                     client_normal(client));
                if (e->xconfigurerequest.value_mask & CWX)
                    x = newx;
                if (e->xconfigurerequest.value_mask & CWY)
                    y = newy;
            }
	       
            switch (client->gravity) {
            case NorthEastGravity:
            case EastGravity:
                corner = OB_CORNER_TOPRIGHT;
                break;
            case SouthWestGravity:
            case SouthGravity:
                corner = OB_CORNER_BOTTOMLEFT;
                break;
            case SouthEastGravity:
                corner = OB_CORNER_BOTTOMRIGHT;
                break;
            default:     /* NorthWest, Static, etc */
                corner = OB_CORNER_TOPLEFT;
            }

            client_configure_full(client, corner, x, y, w, h, FALSE, TRUE,
                                  TRUE);
        }

        if (e->xconfigurerequest.value_mask & CWStackMode) {
            switch (e->xconfigurerequest.detail) {
            case Below:
            case BottomIf:
                client_lower(client);
                break;

            case Above:
            case TopIf:
            default:
                client_raise(client);
                break;
            }
        }
        break;
    case UnmapNotify:
        if (client->ignore_unmaps) {
            client->ignore_unmaps--;
            break;
        }
        client_unmanage(client);
        break;
    case DestroyNotify:
        client_unmanage(client);
        break;
    case ReparentNotify:
        /* this is when the client is first taken captive in the frame */
        if (e->xreparent.parent == client->frame->plate) break;

        /*
          This event is quite rare and is usually handled in unmapHandler.
          However, if the window is unmapped when the reparent event occurs,
          the window manager never sees it because an unmap event is not sent
          to an already unmapped window.
        */

        /* we don't want the reparent event, put it back on the stack for the
           X server to deal with after we unmanage the window */
        XPutBackEvent(ob_display, e);
     
        client_unmanage(client);
        break;
    case MapRequest:
        ob_debug("MapRequest for 0x%lx\n", client->window);
        if (!client->iconic) break; /* this normally doesn't happen, but if it
                                       does, we don't want it! */
        client_activate(client, FALSE);
        break;
    case ClientMessage:
        /* validate cuz we query stuff off the client here */
        if (!client_validate(client)) break;

        if (e->xclient.format != 32) return;

        msgtype = e->xclient.message_type;
        if (msgtype == prop_atoms.wm_change_state) {
            /* compress changes into a single change */
            while (XCheckTypedWindowEvent(ob_display, client->window,
                                          e->type, &ce)) {
                /* XXX: it would be nice to compress ALL messages of a
                   type, not just messages in a row without other
                   message types between. */
                if (ce.xclient.message_type != msgtype) {
                    XPutBackEvent(ob_display, &ce);
                    break;
                }
                e->xclient = ce.xclient;
            }
            client_set_wm_state(client, e->xclient.data.l[0]);
        } else if (msgtype == prop_atoms.net_wm_desktop) {
            /* compress changes into a single change */
            while (XCheckTypedWindowEvent(ob_display, client->window,
                                          e->type, &ce)) {
                /* XXX: it would be nice to compress ALL messages of a
                   type, not just messages in a row without other
                   message types between. */
                if (ce.xclient.message_type != msgtype) {
                    XPutBackEvent(ob_display, &ce);
                    break;
                }
                e->xclient = ce.xclient;
            }
            if ((unsigned)e->xclient.data.l[0] < screen_num_desktops ||
                (unsigned)e->xclient.data.l[0] == DESKTOP_ALL)
                client_set_desktop(client, (unsigned)e->xclient.data.l[0],
                                   FALSE);
        } else if (msgtype == prop_atoms.net_wm_state) {
            /* can't compress these */
            ob_debug("net_wm_state %s %ld %ld for 0x%lx\n",
                     (e->xclient.data.l[0] == 0 ? "Remove" :
                      e->xclient.data.l[0] == 1 ? "Add" :
                      e->xclient.data.l[0] == 2 ? "Toggle" : "INVALID"),
                     e->xclient.data.l[1], e->xclient.data.l[2],
                     client->window);
            client_set_state(client, e->xclient.data.l[0],
                             e->xclient.data.l[1], e->xclient.data.l[2]);
        } else if (msgtype == prop_atoms.net_close_window) {
            ob_debug("net_close_window for 0x%lx\n", client->window);
            client_close(client);
        } else if (msgtype == prop_atoms.net_active_window) {
            ob_debug("net_active_window for 0x%lx\n", client->window);
            client_activate(client, FALSE);
        } else if (msgtype == prop_atoms.net_wm_moveresize) {
            ob_debug("net_wm_moveresize for 0x%lx\n", client->window);
            if ((Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_size_topleft ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_size_top ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_size_topright ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_size_right ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_size_right ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_size_bottomright ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_size_bottom ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_size_bottomleft ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_size_left ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_move ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_size_keyboard ||
                (Atom)e->xclient.data.l[2] ==
                prop_atoms.net_wm_moveresize_move_keyboard) {

                moveresize_start(client, e->xclient.data.l[0],
                                 e->xclient.data.l[1], e->xclient.data.l[3],
                                 e->xclient.data.l[2]);
            }
        } else if (msgtype == prop_atoms.net_moveresize_window) {
            int oldg = client->gravity;
            int tmpg, x, y, w, h;

            if (e->xclient.data.l[0] & 0xff)
                tmpg = e->xclient.data.l[0] & 0xff;
            else
                tmpg = oldg;

            if (e->xclient.data.l[0] & 1 << 8)
                x = e->xclient.data.l[1];
            else
                x = client->area.x;
            if (e->xclient.data.l[0] & 1 << 9)
                y = e->xclient.data.l[2];
            else
                y = client->area.y;
            if (e->xclient.data.l[0] & 1 << 10)
                w = e->xclient.data.l[3];
            else
                w = client->area.width;
            if (e->xclient.data.l[0] & 1 << 11)
                h = e->xclient.data.l[4];
            else
                h = client->area.height;
            client->gravity = tmpg;

            {
                int newx = x;
                int newy = y;
                int fw = w +
                    client->frame->size.left + client->frame->size.right;
                int fh = h +
                    client->frame->size.top + client->frame->size.bottom;
                client_find_onscreen(client, &newx, &newy, fw, fh,
                                     client_normal(client));
                if (e->xclient.data.l[0] & 1 << 8)
                    x = newx;
                if (e->xclient.data.l[0] & 1 << 9)
                    y = newy;
            }
	       
            client_configure(client, OB_CORNER_TOPLEFT,
                             x, y, w, h, FALSE, TRUE);

            client->gravity = oldg;
        }
        break;
    case PropertyNotify:
        /* validate cuz we query stuff off the client here */
        if (!client_validate(client)) break;
  
        /* compress changes to a single property into a single change */
        while (XCheckTypedWindowEvent(ob_display, client->window,
                                      e->type, &ce)) {
            Atom a, b;

            /* XXX: it would be nice to compress ALL changes to a property,
               not just changes in a row without other props between. */

            a = ce.xproperty.atom;
            b = e->xproperty.atom;

            if (a == b)
                continue;
            if ((a == prop_atoms.net_wm_name ||
                 a == prop_atoms.wm_name ||
                 a == prop_atoms.net_wm_icon_name ||
                 a == prop_atoms.wm_icon_name)
                &&
                (b == prop_atoms.net_wm_name ||
                 b == prop_atoms.wm_name ||
                 b == prop_atoms.net_wm_icon_name ||
                 b == prop_atoms.wm_icon_name)) {
                continue;
            }
            if ((a == prop_atoms.net_wm_icon ||
                 a == prop_atoms.kwm_win_icon)
                &&
                (b == prop_atoms.net_wm_icon ||
                 b == prop_atoms.kwm_win_icon))
                continue;

            XPutBackEvent(ob_display, &ce);
            break;
        }

        msgtype = e->xproperty.atom;
        if (msgtype == XA_WM_NORMAL_HINTS) {
            client_update_normal_hints(client);
            /* normal hints can make a window non-resizable */
            client_setup_decor_and_functions(client);
        } else if (msgtype == XA_WM_HINTS) {
            client_update_wmhints(client);
        } else if (msgtype == XA_WM_TRANSIENT_FOR) {
            client_update_transient_for(client);
            client_get_type(client);
            /* type may have changed, so update the layer */
            client_calc_layer(client);
            client_setup_decor_and_functions(client);
        } else if (msgtype == prop_atoms.net_wm_name ||
                   msgtype == prop_atoms.wm_name ||
                   msgtype == prop_atoms.net_wm_icon_name ||
                   msgtype == prop_atoms.wm_icon_name) {
            client_update_title(client);
        } else if (msgtype == prop_atoms.wm_class) {
            client_update_class(client);
        } else if (msgtype == prop_atoms.wm_protocols) {
            client_update_protocols(client);
            client_setup_decor_and_functions(client);
        }
        else if (msgtype == prop_atoms.net_wm_strut) {
            client_update_strut(client);
        }
        else if (msgtype == prop_atoms.net_wm_icon ||
                 msgtype == prop_atoms.kwm_win_icon) {
            client_update_icons(client);
        }
        else if (msgtype == prop_atoms.sm_client_id) {
            client_update_sm_client_id(client);
        }
    default:
        ;
#ifdef SHAPE
        if (extensions_shape && e->type == extensions_shape_event_basep) {
            client->shaped = ((XShapeEvent*)e)->shaped;
            frame_adjust_shape(client->frame);
        }
#endif
    }
}

static void event_handle_dock(ObDock *s, XEvent *e)
{
    switch (e->type) {
    case ButtonPress:
        stacking_raise(DOCK_AS_WINDOW(s));
        break;
    case EnterNotify:
        dock_hide(FALSE);
        break;
    case LeaveNotify:
        dock_hide(TRUE);
        break;
    }
}

static void event_handle_dockapp(ObDockApp *app, XEvent *e)
{
    switch (e->type) {
    case MotionNotify:
        dock_app_drag(app, &e->xmotion);
        break;
    case UnmapNotify:
        if (app->ignore_unmaps) {
            app->ignore_unmaps--;
            break;
        }
        dock_remove(app, TRUE);
        break;
    case DestroyNotify:
        dock_remove(app, FALSE);
        break;
    case ReparentNotify:
        dock_remove(app, FALSE);
        break;
    case ConfigureNotify:
        dock_app_configure(app, e->xconfigure.width, e->xconfigure.height);
        break;
    }
}

ObMenuFrame* find_active_menu()
{
    GList *it;
    ObMenuFrame *ret = NULL;

    for (it = menu_frame_visible; it; it = g_list_next(it)) {
        ret = it->data;
        if (ret->selected)
            break;
        ret = NULL;
    }
    return ret;
}

ObMenuFrame* find_active_or_last_menu()
{
    ObMenuFrame *ret = NULL;

    ret = find_active_menu();
    if (!ret && menu_frame_visible)
        ret = menu_frame_visible->data;
    return ret;
}

static void event_handle_menu(XEvent *ev)
{
    ObMenuFrame *f;
    ObMenuEntryFrame *e;

    switch (ev->type) {
    case ButtonRelease:
        if (menu_can_hide) {
            if ((e = menu_entry_frame_under(ev->xbutton.x_root,
                                            ev->xbutton.y_root)))
                menu_entry_frame_execute(e, ev->xbutton.state);
            else
                menu_frame_hide_all();
        }
        break;
    case MotionNotify:
        if ((f = menu_frame_under(ev->xmotion.x_root,
                                  ev->xmotion.y_root))) {
            menu_frame_move_on_screen(f);
            if ((e = menu_entry_frame_under(ev->xmotion.x_root,
                                            ev->xmotion.y_root)))
                menu_frame_select(f, e);
        }
        {
            ObMenuFrame *a;

            a = find_active_menu();
            if (a && a != f &&
                a->selected->entry->type != OB_MENU_ENTRY_TYPE_SUBMENU)
            {
                menu_frame_select(a, NULL);
            }
        }
        break;
    case KeyPress:
        if (ev->xkey.keycode == ob_keycode(OB_KEY_ESCAPE))
            menu_frame_hide_all();
        else if (ev->xkey.keycode == ob_keycode(OB_KEY_RETURN)) {
            ObMenuFrame *f;
            if ((f = find_active_menu()))
                menu_entry_frame_execute(f->selected, ev->xkey.state);
        } else if (ev->xkey.keycode == ob_keycode(OB_KEY_LEFT)) {
            ObMenuFrame *f;
            if ((f = find_active_or_last_menu()) && f->parent)
                menu_frame_select(f, NULL);
        } else if (ev->xkey.keycode == ob_keycode(OB_KEY_RIGHT)) {
            ObMenuFrame *f;
            if ((f = find_active_or_last_menu()) && f->child)
                menu_frame_select_next(f->child);
        } else if (ev->xkey.keycode == ob_keycode(OB_KEY_UP)) {
            ObMenuFrame *f;
            if ((f = find_active_or_last_menu()))
                menu_frame_select_previous(f);
        } else if (ev->xkey.keycode == ob_keycode(OB_KEY_DOWN)) {
            ObMenuFrame *f;
            if ((f = find_active_or_last_menu()))
                menu_frame_select_next(f);
        }
        break;
    }
}

static gboolean menu_hide_delay_func(gpointer data)
{
    menu_can_hide = TRUE;
    return FALSE; /* no repeat */
}

static gboolean focus_delay_func(gpointer data)
{
    ObClient *c = data;

    if (focus_client != c) {
        client_focus(c);
        if (config_focus_raise)
            client_raise(c);
    }
    return FALSE; /* no repeat */
}

static void focus_delay_client_dest(ObClient *client, gpointer data)
{
    ob_main_loop_timeout_remove_data(ob_main_loop, focus_delay_func, client);
}

void event_ignore_queued_enters()
{
    GSList *saved = NULL, *it;
    XEvent *e;
                
    XSync(ob_display, FALSE);

    /* count the events */
    while (TRUE) {
        e = g_new(XEvent, 1);
        if (XCheckTypedEvent(ob_display, EnterNotify, e)) {
            ObWindow *win;
            
            win = g_hash_table_lookup(window_map, &e->xany.window);
            if (win && WINDOW_IS_CLIENT(win))
                ++ignore_enter_focus;
            
            saved = g_slist_append(saved, e);
        } else {
            g_free(e);
            break;
        }
    }
    /* put the events back */
    for (it = saved; it; it = g_slist_next(it)) {
        XPutBackEvent(ob_display, it->data);
        g_free(it->data);
    }
    g_slist_free(saved);
}
