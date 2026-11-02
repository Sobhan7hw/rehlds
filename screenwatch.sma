/*
 * ScreenWatch Admin Menu v1.4 - AMXX plugin
 * Author: FaTaL-[Hit]  https://t.me/Neo_Norm
 * Drives the ScreenWatch metamod; results on server console + BMP files.
 */

#include <amxmodx>
#include <amxmisc>

#define PLUGIN "ScreenWatch Menu"
#define VERSION "1.4"
#define AUTHOR "FaTaL-[Hit]"

public plugin_init() {
register_plugin(PLUGIN, VERSION, AUTHOR)
register_clcmd("say /screenwatch", sw_menu_show)
register_clcmd("say_team /screenwatch", sw_menu_show)
}

public sw_menu_show(id) {
if (!is_user_connected(id))
return PLUGIN_HANDLED
if (!(get_user_flags(id) & ADMIN_RCON)) {
client_print(id, print_chat, "[SW] admin only")
return PLUGIN_HANDLED
}

new menu = menu_create("\yScreenWatch \d(by FaTaL-[Hit])", sw_menu_handler)
menu_additem(menu, "\wScreenshot of a player", "shot", 0)
menu_additem(menu, "\wWindows + Processes of a player", "win", 0)
menu_additem(menu, "\wFull scan (both)", "full", 0)
menu_additem(menu, "\d-----------------", "", 0)
menu_additem(menu, "\wWho has the client module?", "list", 0)
menu_additem(menu, "\wAdd pattern to watchlist", "watch", 0)
menu_additem(menu, "\wShow current watchlist", "showwl", 0)
menu_setprop(menu, MPROP_EXIT, MEXIT_ALL)
menu_display(id, menu, 0)
return PLUGIN_HANDLED
}

sw_run_cmd(id, const cmd[]) {
/* run as if typed from server console/rcon so the metamod sees it */
new buf[128]
formatex(buf, charsmax(buf), "%s", cmd)
server_cmd("%s", buf)
client_print(id, print_chat, "[SW] sent: %s (result: server console)", cmd)
}

public sw_menu_handler(id, menu, item) {
if (item == MENU_EXIT) {
menu_destroy(menu)
return PLUGIN_HANDLED
}

new data[16], name[32], access, callback
menu_item_getinfo(menu, item, access, data, charsmax(data), name, charsmax(name), callback)

if (equal(data, "list")) {
sw_run_cmd(id, "sw_list")
} else if (equal(data, "showwl")) {
sw_run_cmd(id, "sw_watch")
} else if (equal(data, "shot") || equal(data, "win") || equal(data, "full")) {
sw_pick_player(id, data)
} else if (equal(data, "watch")) {
client_print(id, print_chat, "[SW] use: sw_watch add <pattern> from rcon/console")
}

menu_destroy(menu)
return PLUGIN_HANDLED
}

sw_pick_player(id, const action[]) {
new menu = menu_create("\ySelect player:", sw_pick_handler)

new plname[33], info[40]
for (new p = 1; p <= GetMaxPlayers(); p++) {
if (!is_user_connected(p) || is_user_bot(p))
continue
get_user_name(p, plname, charsmax(plname))
formatex(info, charsmax(info), "%s #%d", action, p)
menu_additem(menu, plname, info, 0)
}

menu_setprop(menu, MPROP_EXIT, MEXIT_ALL)
menu_display(id, menu, 0)
}

public sw_pick_handler(id, menu, item) {
if (item != MENU_EXIT) {
new data[24], name[32], access, callback
menu_item_getinfo(menu, item, access, data, charsmax(data), name, charsmax(name), callback)
sw_run_cmd(id, data)
}
menu_destroy(menu)
return PLUGIN_HANDLED
}
