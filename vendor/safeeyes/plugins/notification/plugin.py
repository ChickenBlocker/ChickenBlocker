# Safe Eyes is a utility to remind you to take break frequently
# to protect your eyes from eye strain.

# Copyright (C) 2017  Gobinath

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
import time

import gi
from safeeyes.model import BreakType, State
from safeeyes.translations import translate as _

gi.require_version("Notify", "0.7")
from gi.repository import GLib, Notify

"""
Safe Eyes Notification plugin
"""

APPINDICATOR_ID = "safeeyes"
DAEMON_SCHEDULE_FILE = "/tmp/cb_se_schedule"
notification = None
context = None
warning_time = 10
schedule_timer = None
notified_deadline = None

Notify.init(APPINDICATOR_ID)


def init(ctx, safeeyes_config, plugin_config):
    """Initialize the plugin."""
    global context
    global warning_time
    global schedule_timer
    logging.debug("Initialize Notification plugin")
    context = ctx
    warning_time = safeeyes_config.get("pre_break_warning_time")
    # VENDORED PATCH (ChickenBlocker): daemon-imposed breaks bypass Safe Eyes'
    # PRE_BREAK state. Poll the daemon's authoritative deadline so the normal
    # warning is still shown at the configured lead time. Keep one timer across
    # config reloads.
    if schedule_timer is None:
        schedule_timer = GLib.timeout_add_seconds(1, _check_daemon_schedule)


def _read_daemon_schedule():
    """Return the daemon schedule as a key/value mapping, or None."""
    try:
        with open(DAEMON_SCHEDULE_FILE, encoding="ascii") as schedule_file:
            return dict(
                line.rstrip("\n").split("=", 1)
                for line in schedule_file
                if "=" in line
            )
    except (OSError, ValueError):
        return None


def _show_notification(is_long, seconds):
    """Show or replace the pre-break notification."""
    global notification
    message = "\n"
    if is_long:
        message += _("Ready for a long break in %s seconds") % seconds
    else:
        message += _("Ready for a short break in %s seconds") % seconds

    notification = Notify.Notification.new(
        "Safe Eyes", message, icon="io.github.slgobinath.SafeEyes-enabled"
    )
    try:
        notification.show()
    except BaseException:
        logging.error("Failed to show the notification")


def _check_daemon_schedule():
    """Show one warning when a daemon-owned daytime break is imminent."""
    global notified_deadline
    if context is None or context.state != State.WAITING:
        return GLib.SOURCE_CONTINUE

    schedule = _read_daemon_schedule()
    if not schedule or schedule.get("night") != "0":
        return GLib.SOURCE_CONTINUE
    try:
        deadline = int(schedule["next_break"])
        interval = int(schedule["interval"])
    except (KeyError, ValueError):
        return GLib.SOURCE_CONTINUE

    seconds_left = deadline - int(time.time())
    if deadline > 0 and 0 < seconds_left <= warning_time:
        if notified_deadline != deadline:
            logging.info("Show daemon-scheduled pre-break notification")
            _show_notification(interval == 4, seconds_left)
            notified_deadline = deadline
    return GLib.SOURCE_CONTINUE


def on_pre_break(break_obj):
    """Show the notification."""
    logging.info("Show the notification")
    _show_notification(break_obj.type != BreakType.SHORT_BREAK, warning_time)


def on_start_break(break_obj):
    """Close the notification."""
    global notification
    logging.info("Close pre-break notification")
    if notification:
        try:
            notification.close()
            notification = None
        except BaseException:
            # Some operating systems automatically close the notification.
            pass


def on_exit():
    """Uninitialize the registered notification."""
    global schedule_timer
    logging.debug("Stop Notification plugin")
    if schedule_timer is not None:
        GLib.source_remove(schedule_timer)
        schedule_timer = None
    Notify.uninit()
