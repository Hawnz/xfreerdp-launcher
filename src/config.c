#include "launcher.h"
#include <glib/gstdio.h>

/* 配置文件位置：~/.config/xfreerdp-launcher/config.ini */
static gchar* config_path(void)
{
	const gchar* base = g_get_user_config_dir();
	gchar* dir = g_build_filename(base, "xfreerdp-launcher", NULL);
	g_mkdir_with_parents(dir, 0700);
	gchar* path = g_build_filename(dir, "config.ini", NULL);
	g_free(dir);
	return path;
}

Config* config_load(void)
{
	Config* cfg = g_new0(Config, 1);
	cfg->conns = g_ptr_array_new_with_free_func((GDestroyNotify)conn_free);

	gchar* path = config_path();
	GKeyFile* kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
	{
		gchar** groups = g_key_file_get_groups(kf, NULL);
		for (gint i = 0; groups && groups[i]; i++)
		{
			if (!g_str_has_prefix(groups[i], "conn:"))
				continue;

			Conn* c = conn_new();
			c->name = g_strdup(groups[i] + 5);
			c->host = g_key_file_get_string(kf, groups[i], "host", NULL);
			if (!c->host || !*c->host)
			{
				conn_free(c);
				continue;
			}
			c->port = g_key_file_get_integer(kf, groups[i], "port", NULL);
			{
				const gchar* proto = g_key_file_get_string(kf, groups[i], "protocol", NULL);
				if (proto && g_str_equal(proto, "spice"))
					c->protocol = PROTO_SPICE;
			}
			c->username = g_key_file_get_string(kf, groups[i], "username", NULL);
			c->password = g_key_file_get_string(kf, groups[i], "password", NULL);
			c->width = g_key_file_get_integer(kf, groups[i], "width", NULL);
			c->height = g_key_file_get_integer(kf, groups[i], "height", NULL);
			c->fullscreen = g_key_file_get_boolean(kf, groups[i], "fullscreen", NULL);
		c->gfx = g_key_file_get_boolean(kf, groups[i], "gfx", NULL);
		c->clipboard = g_key_file_get_boolean(kf, groups[i], "clipboard", NULL);
		c->favorite = g_key_file_get_boolean(kf, groups[i], "favorite", NULL);
		c->group = g_key_file_get_string(kf, groups[i], "group", NULL);
		c->extra = g_key_file_get_string(kf, groups[i], "extra", NULL);
			if (c->port <= 0)
				c->port = (c->protocol == PROTO_SPICE) ? 5900 : 3389;
			g_ptr_array_add(cfg->conns, c);
		}
		g_strfreev(groups);
	}
	g_key_file_free(kf);
	g_free(path);
	return cfg;
}

gboolean config_save(Config* cfg, GError** err)
{
	GKeyFile* kf = g_key_file_new();
	for (guint i = 0; i < cfg->conns->len; i++)
	{
		Conn* c = g_ptr_array_index(cfg->conns, i);
		gchar* group = g_strdup_printf("conn:%s", c->name ? c->name : "");
		g_key_file_set_string(kf, group, "host", c->host ? c->host : "");
		g_key_file_set_integer(kf, group, "port", c->port);
		g_key_file_set_string(kf, group, "protocol", c->protocol == PROTO_SPICE ? "spice" : "rdp");
		g_key_file_set_string(kf, group, "username", c->username ? c->username : "");
		g_key_file_set_string(kf, group, "password", c->password ? c->password : "");
		g_key_file_set_integer(kf, group, "width", c->width);
		g_key_file_set_integer(kf, group, "height", c->height);
		g_key_file_set_boolean(kf, group, "fullscreen", c->fullscreen);
		g_key_file_set_boolean(kf, group, "gfx", c->gfx);
		g_key_file_set_boolean(kf, group, "clipboard", c->clipboard);
		g_key_file_set_boolean(kf, group, "favorite", c->favorite);
		g_key_file_set_string(kf, group, "group", c->group ? c->group : "");
		g_key_file_set_string(kf, group, "extra", c->extra ? c->extra : "");
		g_free(group);
	}

	gchar* path = config_path();
	gchar* data = g_key_file_to_data(kf, NULL, NULL);
	gboolean ok = g_file_set_contents(path, data, -1, err);
	g_free(data);
	g_free(path);
	g_key_file_free(kf);
	return ok;
}

Conn* conn_new(void)
{
	Conn* c = g_new0(Conn, 1);
	c->port = 3389;
	c->protocol = PROTO_RDP;
	return c;
}

void conn_free(Conn* c)
{
	if (!c)
		return;
	g_free(c->name);
	g_free(c->host);
	g_free(c->username);
	g_free(c->password);
	g_free(c->group);
	g_free(c->extra);
	g_free(c);
}

void config_free(Config* cfg)
{
	if (!cfg)
		return;
	g_ptr_array_free(cfg->conns, TRUE);
	g_free(cfg);
}

/* 构造连接命令行参数数组 */
gchar** conn_build_argv(Conn* c, const gchar* binary)
{
	GPtrArray* a = g_ptr_array_new();

	if (c->protocol == PROTO_SPICE)
	{
		/* SPICE：remote-viewer spice://host:port[?password=xxx] [-f] [extra] */
		g_ptr_array_add(a, g_strdup(binary));
		gchar* uri = (c->password && *c->password)
		                 ? g_strdup_printf("spice://%s:%d?password=%s", c->host, c->port, c->password)
		                 : g_strdup_printf("spice://%s:%d", c->host, c->port);
		g_ptr_array_add(a, uri);
		if (c->fullscreen)
			g_ptr_array_add(a, g_strdup("-f"));
		if (c->extra && *c->extra)
		{
			gchar** extra = NULL;
			if (g_shell_parse_argv(c->extra, NULL, &extra, NULL))
			{
				for (gint i = 0; extra[i]; i++)
					g_ptr_array_add(a, g_strdup(extra[i]));
			}
			g_strfreev(extra);
		}
		g_ptr_array_add(a, NULL);
		return (gchar**)g_ptr_array_free(a, FALSE);
	}

	g_ptr_array_add(a, g_strdup(binary));
	g_ptr_array_add(a, g_strdup_printf("/v:%s:%d", c->host, c->port));

	if (c->username && *c->username)
		g_ptr_array_add(a, g_strdup_printf("/u:%s", c->username));
	if (c->password && *c->password)
		g_ptr_array_add(a, g_strdup_printf("/p:%s", c->password));

	if (c->fullscreen)
		g_ptr_array_add(a, g_strdup("/f"));
	else if (c->width > 0 && c->height > 0)
		g_ptr_array_add(a, g_strdup_printf("/size:%dx%d", c->width, c->height));

	if (c->gfx)
		g_ptr_array_add(a, g_strdup("/gfx"));
	if (c->clipboard)
		g_ptr_array_add(a, g_strdup("+clipboard"));

	g_ptr_array_add(a, g_strdup("/dynamic-resolution"));
	g_ptr_array_add(a, g_strdup("/log-level:INFO"));
	/* 忽略证书校验，避免 -v（版本输出）与证书错误拦截远程连接 */
	g_ptr_array_add(a, g_strdup("/cert-ignore"));

	if (c->extra && *c->extra)
	{
		gchar** extra = NULL;
		if (g_shell_parse_argv(c->extra, NULL, &extra, NULL))
		{
			for (gint i = 0; extra[i]; i++)
				g_ptr_array_add(a, g_strdup(extra[i]));
		}
		g_strfreev(extra);
	}

	g_ptr_array_add(a, NULL);
	return (gchar**)g_ptr_array_free(a, FALSE);
}
