#!/usr/bin/env python
# Safe Eyes is a utility to remind you to take break frequently
# to protect your eyes from eye strain.

# Copyright (C) 2016  Gobinath

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import logging
import os
import time
import typing

import gi
from safeeyes import utility
from safeeyes.configuration import Config
from safeeyes.context import Context
from safeeyes.model import Break, TrayAction
from safeeyes.translations import translate as _

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk
from gi.repository import GdkPixbuf
from gi.repository import GdkX11
from gi.repository import GLib
from gi.repository import Gtk

BREAK_SCREEN_GLADE = os.path.join(utility.BIN_DIRECTORY, "glade/break_screen.glade")


def _read_first_line(path: str) -> typing.Optional[str]:
    try:
        with open(path, "r") as file:
            return file.readline().strip()
    except OSError:
        return None


def _status_text() -> str:
    """VENDORED PATCH (ChickenBlocker): top-left status line on the break
    screen: current time + battery percentage (from sysfs; the battery part
    is omitted on machines without one).
    """
    parts = [time.strftime("%H:%M")]
    power_supply_dir = "/sys/class/power_supply"
    try:
        supplies = sorted(os.listdir(power_supply_dir))
    except OSError:
        supplies = []
    for name in supplies:
        supply_dir = os.path.join(power_supply_dir, name)
        if _read_first_line(os.path.join(supply_dir, "type")) != "Battery":
            continue
        capacity = _read_first_line(os.path.join(supply_dir, "capacity"))
        if capacity is None or not capacity.isdigit():
            continue
        battery = "Battery {}%".format(capacity)
        status = _read_first_line(os.path.join(supply_dir, "status"))
        if status == "Charging":
            battery += " (charging)"
        parts.append(battery)
        break
    return "   ·   ".join(parts)


class BreakScreen:
    """The fullscreen windows which prevent users from using the computer.

    This class creates and manages the fullscreen windows for every monitor.
    """

    windows: list["BreakScreenWindow"]

    def __init__(
        self,
        application: Gtk.Application,
        context: Context,
        on_skipped: typing.Callable[[], None],
        on_postponed: typing.Callable[[], None],
    ):
        self.application = application
        self.context = context
        self.x11_display = None
        self.enable_postpone = False
        self.enable_shortcut = False
        self.is_pretified = False
        self.keycode_shortcut_postpone = 65  # Space
        self.keycode_shortcut_skip = 9  # Escape
        self.on_postponed = on_postponed
        self.on_skipped = on_skipped
        self.fade_in_break_screen = True
        self.fade_in_break_screen_duration = 1500
        self.shortcut_disable_time = 2
        self.strict_break = False
        self.windows = []
        self.show_skip_button = False
        self.show_postpone_button = False

        if not self.context.is_wayland:
            import Xlib.display

            self.x11_display = Xlib.display.Display()

    def initialize(self, config: Config) -> None:
        """Initialize the internal properties from configuration."""
        logging.info("Initialize the break screen")
        self.enable_postpone = config.get("allow_postpone", False)
        self.keycode_shortcut_postpone = config.get("shortcut_postpone", 65)
        self.keycode_shortcut_skip = config.get("shortcut_skip", 9)

        if self.context.is_wayland and (
            self.keycode_shortcut_postpone != 65 or self.keycode_shortcut_skip != 9
        ):
            logging.warning(
                _(
                    "Customizing the postpone and skip shortcuts does not work on "
                    "Wayland."
                )
            )

        # TODO: shortcut_disable_time should be renamed
        # it used to be just about keyboard shortcuts - now it also controls whether
        # the buttons are locked
        self.shortcut_disable_time = config.get("shortcut_disable_time", 2)
        self.fade_in_break_screen = config.get("fade_in_break_screen", True)
        self.fade_in_break_screen_duration = config.get(
            "fade_in_break_screen_duration", 1500
        )
        self.strict_break = config.get("strict_break", False)

    def skip_break(self) -> None:
        """Skip the break from the break screen."""
        logging.info("User skipped the break")
        # Must call on_skipped before close to lock screen before closing the break
        # screen
        self.on_skipped()
        self.close()

    def postpone_break(self) -> None:
        """Postpone the break from the break screen."""
        logging.info("User postponed the break")
        self.on_postponed()
        self.close()

    def on_skip_clicked(self, button) -> None:
        """Skip button press event handler."""
        if self.enable_shortcut:
            self.skip_break()

    def on_postpone_clicked(self, button) -> None:
        """Postpone button press event handler."""
        if self.enable_shortcut:
            self.postpone_break()

    def show_count_down(self, countdown: int, seconds: int) -> None:
        """Show/update the count down on all screens."""
        self.enable_shortcut = self.shortcut_disable_time <= seconds
        mins, secs = divmod(countdown, 60)
        timeformat = "{:02d}:{:02d}".format(mins, secs)
        self.__update_count_down(timeformat, self.enable_shortcut)
        self.__update_status()

    def show_message(
        self, break_obj: Break, widget: str, tray_actions: list[TrayAction] = []
    ) -> None:
        """Show the break screen with the given message on all displays."""
        message = break_obj.name
        image_path = break_obj.image
        self.enable_shortcut = self.shortcut_disable_time <= 0
        self.__show_break_screen(message, image_path, widget, tray_actions)
        self.__update_status()

    def close(self) -> None:
        """Hide the break screen from active window and destroy all other
        windows.
        """
        logging.info("Close the break screen(s)")
        if not self.context.is_wayland:
            self.__release_keyboard_x11()

        # Destroy other windows if exists
        self.__destroy_all_screens()

    def __show_break_screen(
        self,
        message: str,
        image_path: typing.Optional[str],
        widget: str,
        tray_actions: list[TrayAction],
    ) -> None:
        """Show an empty break screen on all screens."""
        # Lock the keyboard
        if not self.context.is_wayland:
            utility.start_thread(self.__lock_keyboard_x11)

        display = Gdk.Display.get_default()

        if display is None:
            raise Exception("display not found")

        monitors = typing.cast(typing.Sequence[Gdk.Monitor], display.get_monitors())
        logging.info("Show break screens in %d display(s)", len(monitors))

        skip_button_disabled = self.context.get("skip_button_disabled", False)
        self.show_skip_button = not self.strict_break and not skip_button_disabled

        postpone_button_disabled = self.context.get("postpone_button_disabled", False)
        self.show_postpone_button = (
            self.enable_postpone and not postpone_button_disabled
        )

        i = 0

        for monitor in monitors:
            monitor_geometry = monitor.get_geometry()
            window = BreakScreenWindow(
                self.application,
                message,
                image_path,
                widget,
                monitor_geometry.width,
                monitor_geometry.height,
                tray_actions,
                lambda: self.close(),
                self.show_postpone_button,
                self.on_postpone_clicked,
                self.show_skip_button,
                self.on_skip_clicked,
                self.enable_shortcut,
                self.fade_in_break_screen_duration,
            )

            if self.context.is_wayland:
                # Note: in theory, this could also be used on X11
                # however, that already has its own implementation below
                controller = Gtk.EventControllerKey()
                controller.connect("key_pressed", self.on_key_pressed_wayland)
                controller.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
                window.add_controller(controller)

            window.set_title("SafeEyes-" + str(i))

            self.windows.append(window)

            target_opacity = 0.9 if self.context.desktop == "kde" else 1.0
            window.configure_opacity(target_opacity, self.fade_in_break_screen)

            window.present()

            # Apparently this needs to run after present() (as of GTK 4.20)
            # On Wayland, either work seems to work fine
            # On X11, calling this before present() always fullscreens only on the
            # focused monitor regardless
            window.fullscreen_on_monitor(monitor)

            # this ensures that none of the buttons is in focus immediately
            # otherwise, pressing space presses that button instead of triggering the
            # shortcut
            window.set_focus(None)

            if not self.context.is_wayland:
                self.__window_set_keep_above_x11(window)

            if self.context.is_wayland:
                # this may or may not be granted by the window system
                surface = window.get_surface()
                if surface is not None:
                    typing.cast(Gdk.Toplevel, surface).inhibit_system_shortcuts(None)

            if self.fade_in_break_screen:
                window.start_fade_in()

            # VENDORED PATCH (ChickenBlocker): re-apply fullscreen if the WM
            # unfullscreens the break window (e.g. xfwm4's <Alt>F11 toggle).
            # Attached LAST, after the whole map/fullscreen/keep-above
            # handshake, so the watcher never fights the show sequence.
            window.enforce_fullscreen_on_monitor(monitor)

            i = i + 1

    def __update_count_down(self, count: str, enable_shortcut: bool) -> None:
        """Update the countdown on all break screens."""
        for window in self.windows:
            window.set_count_down(count, enable_shortcut)

    def __update_status(self) -> None:
        """Update the time/battery status line on all break screens."""
        status = _status_text()
        for window in self.windows:
            window.set_status(status)

    def __window_set_keep_above_x11(self, window: "BreakScreenWindow") -> None:
        """Use EWMH hints to keep window above and on all desktops."""
        if self.x11_display is None:
            return

        import Xlib

        NET_WM_STATE = self.x11_display.intern_atom("_NET_WM_STATE")
        NET_WM_STATE_ABOVE = self.x11_display.intern_atom("_NET_WM_STATE_ABOVE")
        NET_WM_STATE_STICKY = self.x11_display.intern_atom("_NET_WM_STATE_STICKY")

        # To change the _NET_WM_STATE, we cannot simply set the
        # property - we must send a ClientMessage event
        # See https://specifications.freedesktop.org/wm-spec/1.3/ar01s05.html#id-1.6.8
        root_window = self.x11_display.screen().root

        surface = window.get_surface()

        if surface is None or not isinstance(surface, GdkX11.X11Surface):
            return

        xid = GdkX11.X11Surface.get_xid(surface)

        root_window.send_event(
            Xlib.protocol.event.ClientMessage(
                window=xid,
                client_type=NET_WM_STATE,
                data=(
                    32,
                    [
                        1,  # _NET_WM_STATE_ADD
                        NET_WM_STATE_ABOVE,
                        NET_WM_STATE_STICKY,  # other property
                        1,  # source indication
                        0,  # must be 0
                    ],
                ),
            ),
            event_mask=(
                Xlib.X.SubstructureRedirectMask | Xlib.X.SubstructureNotifyMask
            ),
        )

        self.x11_display.sync()

    def __lock_keyboard_x11(self) -> None:
        """Lock the keyboard to prevent the user from using keyboard shortcuts.

        (X11 only)

        VENDORED PATCH (ChickenBlocker): keyboard grab removed. An active X
        keyboard grab blocks the screen locker (xfce4-screensaver /
        light-locker / xflock4) from grabbing the keyboard, so the idle
        screen lock never fires while a break window is shown. The break
        screen still covers the whole screen (fullscreen + set_focus(None))
        and stays on top via _NET_WM_STATE_ABOVE + STICKY
        (__window_set_keep_above_x11), so the user cannot actually interact
        with other windows even though input focus may move. Skip/postpone
        shortcuts are disabled in this setup anyway (strict_break + shortcut
        keys = 0), so losing the grab loses nothing. self.lock_keyboard is
        kept False so __release_keyboard_x11 (which only sets it False) is a
        harmless no-op.
        """
        self.lock_keyboard = False
        return

    def on_key_pressed_wayland(
        self, event_controller_key, keyval, keycode, state
    ) -> bool:
        if self.enable_shortcut:
            if keyval == Gdk.KEY_space and self.show_postpone_button:
                self.postpone_break()
                return True
            elif keyval == Gdk.KEY_Escape and self.show_skip_button:
                self.skip_break()
                return True

        return False

    def __release_keyboard_x11(self) -> None:
        """Release the locked keyboard."""
        logging.info("Unlock the keyboard")
        self.lock_keyboard = False

    def __destroy_all_screens(self) -> None:
        """Close all the break screens."""
        for win in self.windows:
            win.stop_enforce_fullscreen()
            win.stop_fade_in()
            win.destroy()
        del self.windows[:]


@Gtk.Template(filename=BREAK_SCREEN_GLADE)
class BreakScreenWindow(Gtk.Window):
    """This class manages the UI for the break screen window.

    Each instance is a single window, covering a single monitor.
    """

    __gtype_name__ = "BreakScreenWindow"

    grid1: Gtk.Grid = Gtk.Template.Child()
    lbl_message: Gtk.Label = Gtk.Template.Child()
    lbl_count: Gtk.Label = Gtk.Template.Child()
    lbl_status: Gtk.Label = Gtk.Template.Child()
    lbl_widget: Gtk.Label = Gtk.Template.Child()
    img_break: Gtk.Picture = Gtk.Template.Child()
    box_buttons: Gtk.Box = Gtk.Template.Child()
    toolbar: Gtk.Box = Gtk.Template.Child()

    def __init__(
        self,
        application: Gtk.Application,
        message: str,
        image_path: typing.Optional[str],
        widget: str,
        monitor_width: int,
        monitor_height: int,
        tray_actions: list[TrayAction],
        on_close: typing.Callable[[], None],
        show_postpone: bool,
        on_postpone: typing.Callable[[Gtk.Button], None],
        show_skip: bool,
        on_skip: typing.Callable[[Gtk.Button], None],
        enable_shortcut: bool,
        fade_in_duration_ms: int,
    ):
        super().__init__(application=application)

        self.on_close = on_close
        self.img_break.set_content_fit(Gtk.ContentFit.SCALE_DOWN)
        self.grid1.add_css_class("break_screen_root")
        self.button_widgets: list[Gtk.Button] = []
        self.fade_in_target_opacity = 1.0
        self.fade_in_enabled = False
        self.fade_in_css_provider: typing.Optional[Gtk.CssProvider] = None
        self.fade_in_duration_ms = max(fade_in_duration_ms, 1)
        # VENDORED PATCH (ChickenBlocker): fullscreen enforcement state
        self.enforce_fullscreen = False
        self.fullscreen_monitor: typing.Optional[Gdk.Monitor] = None

        for tray_action in tray_actions:
            # TODO: apparently, this would be better served with an icon theme
            # + Gtk.button.new_from_icon_name
            icon = tray_action.get_icon()
            toolbar_button = Gtk.Button()
            toolbar_button.set_child(icon)
            tray_action.add_toolbar_button(toolbar_button)
            toolbar_button.connect(
                "clicked",
                lambda button, action: self.__tray_action(button, action),
                tray_action,
            )
            toolbar_button.set_tooltip_text(_(tray_action.name))
            self.toolbar.append(toolbar_button)
            toolbar_button.show()

        # Add the buttons
        if show_postpone:
            # Add postpone button
            btn_postpone = Gtk.Button.new_with_label(_("Postpone"))
            btn_postpone.get_style_context().add_class("btn_postpone")
            btn_postpone.connect("clicked", on_postpone)
            btn_postpone.set_visible(True)
            btn_postpone.set_sensitive(enable_shortcut)
            self.box_buttons.append(btn_postpone)
            self.button_widgets.append(btn_postpone)

        if show_skip:
            # Add the skip button
            btn_skip = Gtk.Button.new_with_label(_("Skip"))
            btn_skip.get_style_context().add_class("btn_skip")
            btn_skip.connect("clicked", on_skip)
            btn_skip.set_visible(True)
            btn_skip.set_sensitive(enable_shortcut)
            self.box_buttons.append(btn_skip)
            self.button_widgets.append(btn_skip)

        # Set values
        if image_path:
            self.__set_break_image(image_path, monitor_width, monitor_height)
        self.lbl_message.set_label(message)
        self.lbl_widget.set_markup(widget)

    def configure_opacity(self, target_opacity: float, fade_in_enabled: bool) -> None:
        self.fade_in_target_opacity = target_opacity
        self.fade_in_enabled = fade_in_enabled
        self.set_opacity(target_opacity)
        self.grid1.remove_css_class("break_screen_fade_in")

        if fade_in_enabled:
            self.__set_fade_in_animation_duration(self.fade_in_duration_ms)

    def start_fade_in(self) -> None:
        if self.fade_in_enabled:
            self.grid1.add_css_class("break_screen_fade_in")

    def stop_fade_in(self) -> None:
        self.grid1.remove_css_class("break_screen_fade_in")

    def __set_fade_in_animation_duration(self, fade_in_duration_ms: int) -> None:
        css_provider = Gtk.CssProvider()
        css_provider.load_from_data(
            f".break_screen_fade_in {{ animation-duration: {fade_in_duration_ms}ms; }}"
        )

        display = Gdk.Display.get_default()
        if display is not None:
            Gtk.StyleContext.add_provider_for_display(
                display,
                css_provider,
                Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 1,
            )

        self.fade_in_css_provider = css_provider

    def set_count_down(self, count: str, enable_shortcut: bool) -> None:
        self.lbl_count.set_text(count)

        for button in self.button_widgets:
            button.set_sensitive(enable_shortcut)

    def set_status(self, status: str) -> None:
        self.lbl_status.set_text(status)

    def __tray_action(self, button, tray_action: TrayAction) -> None:
        """Tray action handler.

        Hides all toolbar buttons for this action and call the action
        provided by the plugin.
        """
        if tray_action.single_use:
            tray_action.reset()
        tray_action.action()

    def __set_break_image(
        self, image_path: str, monitor_width: int, monitor_height: int
    ) -> None:
        """Load the break image and cap it relative to the current monitor size."""
        max_width = max(1, (monitor_width * 8) // 10 - 1)
        max_height = max(1, (monitor_height * 3) // 10 - 1)

        try:
            loaded = GdkPixbuf.Pixbuf.new_from_file(image_path)
            if loaded is None:
                raise ValueError("Failed to load break image")
        except Exception:
            logging.exception("Failed to load break image: %s", image_path)
            return
        pixbuf: GdkPixbuf.Pixbuf = loaded

        width, height = pixbuf.get_width(), pixbuf.get_height()
        scale = min(1, max_width / width, max_height / height)
        if scale < 1:
            pixbuf = (
                pixbuf.scale_simple(
                    max(1, int(width * scale)),
                    max(1, int(height * scale)),
                    GdkPixbuf.InterpType.BILINEAR,
                )
                or pixbuf
            )

        self.img_break.set_paintable(Gdk.Texture.new_for_pixbuf(pixbuf))

    def enforce_fullscreen_on_monitor(self, monitor: Gdk.Monitor) -> None:
        """VENDORED PATCH (ChickenBlocker): keep the break window fullscreen.

        A WM keybinding (xfwm4's fullscreen toggle, default <Alt>F11) can
        unfullscreen even an undecorated window while it has input focus,
        uncovering the desktop behind the break screen. Watch the FULLSCREEN
        bit of the toplevel state and re-apply fullscreen when it drops.

        Hardened against suspend/VT-switch storms: the toplevel state also
        flickers (FOCUSED/SUSPENDED/etc.) when the session suspends or the
        screen locks, and an eager re-apply loop fought the WM there
        (unfullscreen -> re-fullscreen -> ... at full speed, 100% CPU, until
        the window ended up with all WM state stripped). Therefore this only
        reacts to an actual FULLSCREEN-bit DROP (never to other bits), rate
        limits re-applies to one per second, verifies the result lazily, and
        stands down after a few failed attempts (re-arming as soon as the
        fullscreen state is regained).
        """
        self.fullscreen_monitor = monitor
        self.enforce_fullscreen = True
        self._fs_last_seen: typing.Optional[bool] = None
        self._fs_failures = 0
        self._fs_last_reapply = 0.0
        self._fs_verify_pending = False
        self.connect("notify::surface", self._on_surface_replaced)
        self.connect("destroy", self._on_destroyed)
        self.__watch_surface_fullscreen()

    def stop_enforce_fullscreen(self) -> None:
        """Stop re-applying fullscreen (teardown: closing/destroying)."""
        self.enforce_fullscreen = False

    def _on_destroyed(self, *_args) -> None:
        self.enforce_fullscreen = False

    def _on_surface_replaced(self, *_args) -> None:
        # A new surface (unrealize/realize) may come up without the
        # fullscreen state; re-attach the watcher (the initial check below
        # schedules a lazy verify instead of fighting the map handshake).
        if self.enforce_fullscreen:
            self._fs_last_seen = None
            self.__watch_surface_fullscreen()

    def __watch_surface_fullscreen(self) -> None:
        surface = self.get_surface()
        if surface is None:
            return
        surface.connect("notify::state", self._on_toplevel_state_changed)
        try:
            fs = bool(surface.get_state() & Gdk.ToplevelState.FULLSCREEN)
        except Exception:
            return
        self._fs_last_seen = fs
        # A re-created surface may come up without FULLSCREEN: verify lazily
        # (after the WM settled) rather than re-applying during the handshake.
        if not fs:
            self.__schedule_fs_verify(2000)

    def _on_toplevel_state_changed(self, surface: Gdk.Surface, _pspec) -> None:
        if not self.enforce_fullscreen:
            return
        try:
            fs = bool(surface.get_state() & Gdk.ToplevelState.FULLSCREEN)
        except Exception:
            return
        # Only react to an actual FULLSCREEN-bit transition; ignore flicker
        # of the other bits (focus, suspend, tiling, ...).
        if self._fs_last_seen is not None and fs == self._fs_last_seen:
            return
        self._fs_last_seen = fs
        if fs:
            # Fullscreen (re)gained: reset the failure budget.
            self._fs_failures = 0
            return
        self.__maybe_reapply_fullscreen()

    def __maybe_reapply_fullscreen(self) -> None:
        if not self.enforce_fullscreen:
            return
        if self._fs_failures >= 5:
            # The WM keeps refusing (or we are mid VT-switch/screensaver
            # transition). Give up instead of fighting; re-arms automatically
            # if the fullscreen bit is regained later.
            return
        now = time.monotonic()
        elapsed = now - self._fs_last_reapply
        if elapsed < 1.0:
            # Rate limited: re-check later; if still not fullscreen by then,
            # the verify callback re-applies.
            self.__schedule_fs_verify(int((1.0 - elapsed) * 1000) + 200)
            return
        self.__reapply_fullscreen()

    def __reapply_fullscreen(self) -> None:
        if not self.enforce_fullscreen:
            return
        self._fs_last_reapply = time.monotonic()
        self._fs_failures += 1
        if self._fs_failures == 1:
            logging.info("Break screen lost fullscreen state; re-applying")
        elif self._fs_failures >= 5:
            logging.info(
                "Break screen fullscreen not regained after %d attempts; "
                "standing down",
                self._fs_failures,
            )
        try:
            if self.fullscreen_monitor is not None:
                self.fullscreen_on_monitor(self.fullscreen_monitor)
            else:
                self.fullscreen()
        except Exception:
            logging.exception("Failed to re-apply fullscreen on break screen")
        self.__schedule_fs_verify(1200)

    def __schedule_fs_verify(self, delay_ms: int) -> None:
        if self._fs_verify_pending:
            return
        self._fs_verify_pending = True

        def _verify() -> bool:
            self._fs_verify_pending = False
            if not self.enforce_fullscreen:
                return False
            surface = self.get_surface()
            if surface is None:
                return False
            try:
                fs = bool(surface.get_state() & Gdk.ToplevelState.FULLSCREEN)
            except Exception:
                return False
            self._fs_last_seen = fs
            if not fs:
                self.__maybe_reapply_fullscreen()
            return False

        GLib.timeout_add(delay_ms, _verify)

    @Gtk.Template.Callback()
    def on_window_delete(self, *args) -> None:
        """Window close event handler."""
        logging.info("Closing the break screen")
        self.stop_fade_in()
        self.on_close()
