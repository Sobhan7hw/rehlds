/*
 * ScreenWatch Admin Menu v1.0 - AMXX plugin by FaTaL-[Hit] - https://t.me/Neo_Norm
 * /screenwatch -> menu to shoot / list windows+procs / module status / watchlist,
 * powered by the "ScreenWatch" metamod plugin (sw_shot, sw_win, sw_full, sw_list, sw_watch).
 */

#include <amxmodx>
#include <amxmisc>

#define PLUGIN "ScreenWatch Menu"
#define VERSION "1.0"
#define AUTHOR "FaTaL-[Hit]"

public plugin_init() {
	register_plugin(PLUGIN, VERSION, AUTHOR)
	register_clcmd("say /screenwatch", sw_menu_show)
	register_clcmd("say_team /screenwatch", sw_menu_show)
	register_concmd("screenwatch", sw_menu_show, ADMIN_RCON, "- open screenwatch menu")
}

public sw_menu_show(id) {
	if (!is_user_connected(id) || !user_has_flags(id, "z")) {
		client_print(id, print_chat, "[SW] admin only")
		return HANDLED
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
	return MENU_OK
}

public sw_menu_handler(id, menu, item) {
	if (item == MENU_EXIT) { menu_destroy(menu); return }

	new data[6], name[64], access, callback
	menu_item_getinfo(menu, item, access, data, charsmax(data), name, charsmax(name), callback)

	if (equal(data, "list") || equal(data, "showwl")) {
		client_cmd(id, "sw_%s", equal(data, "list") ? "list" : "watch")
		client_print(id, print_chat, "[SW] result printed to your console (~)")
	} else if (equal(data, "shot") || equal(data, "win") || equal(data, "full")) {
		sw_pick_player(id, data)
	} else if (equal(data, "watch")) {
		client_cmd(id, "messagemode sw_watch add")
		client_print(id, print_chat, "[SW] type the pattern in chat")
	}
	menu_destroy(menu)
}

sw_pick_player(id, const action[]) {
new menu = menu_create("\ySelect player:", sw_pick_handler)

new pid[16], plname[33]
for (new p = 1; p <= GetMaxPlayers(); p++) {
if (!is_user_connected(p) || is_user_bot(p)) continue
get_user_name(p, plname, charsmax(plname))
formatex(pid, charsmax(pid), "%s %d", action, p)
menu_additem(menu, plname, pid, 0)
}

menu_setprop(menu, MPROP_EXIT, MEXIT_ALL)
menu_display(id, menu, 0)
}

public sw_pick_handler(id, menu, item) {
if (item != MENU_EXIT) {
new data[16], name[64], access, callback
menu_item_getinfo(menu, item, access, data, charsmax(data), name, charsmax(name), callback)

new action[8], idxStr[8]
parse(data, action, charsmax(action), idxStr, charsmax(idxStr))
new target = str_to_num(idxStr)
if (target > 0) {
client_cmd(id, "sw_%s #%d", action, target)
client_print(id, print_chat, "[SW] request sent to slot %d (result: server console + saved BMP)", target)
}
}
menu_destroy(menu)
}
