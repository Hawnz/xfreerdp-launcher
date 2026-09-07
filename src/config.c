#include "launcher.h"
#include <glib/gstdio.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

/* 密码密文字段前缀：enc:v1:<base64(iv|tag|ciphertext)> */
#define PASS_PREFIX "enc:v1:"
#define KEY_LEN 32
#define IV_LEN 12
#define TAG_LEN 16

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

/* 密钥文件位置：~/.config/xfreerdp-launcher/secret.key（权限 0600） */
static gchar* secret_key_path(void)
{
	const gchar* base = g_get_user_config_dir();
	gchar* dir = g_build_filename(base, "xfreerdp-launcher", NULL);
	g_mkdir_with_parents(dir, 0700);
	gchar* path = g_build_filename(dir, "secret.key", NULL);
	g_free(dir);
	return path;
}

/* 读取密钥；若不存在或长度不符则生成新的随机密钥并落盘（0600）。 */
static gboolean secret_key_obtain(guchar key[KEY_LEN])
{
	gchar* path = secret_key_path();
	gsize len = 0;
	gchar* raw = NULL;
	gboolean ok = FALSE;

	if (g_file_get_contents(path, &raw, &len, NULL) && len == KEY_LEN)
	{
		memcpy(key, raw, KEY_LEN);
		ok = TRUE;
	}
	else
	{
		if (RAND_bytes(key, KEY_LEN) == 1)
		{
			if (g_file_set_contents(path, (const gchar*)key, KEY_LEN, NULL))
			{
				g_chmod(path, 0600);
				ok = TRUE;
			}
		}
	}

	g_free(raw);
	g_free(path);
	return ok;
}

/* 加密明文密码，返回 "enc:v1:<base64>" 形式的新字符串；失败回退为明文字符串。
 * 空密码返回空串。 */
static gchar* password_encrypt(const gchar* plain)
{
	if (!plain || !*plain)
		return g_strdup("");

	guchar key[KEY_LEN];
	if (!secret_key_obtain(key))
		return g_strdup(plain);

	guchar iv[IV_LEN];
	if (RAND_bytes(iv, IV_LEN) != 1)
		return g_strdup(plain);

	GByteArray* out = g_byte_array_new();
	g_byte_array_append(out, iv, IV_LEN); /* 前 12 字节 IV */

	gsize pt_len = strlen(plain);
	guchar* ct = g_malloc(pt_len); /* GCM 密文与明文等长 */

	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	gboolean ok = FALSE;
	if (ctx)
	{
		int len = 0, fin = 0;
		if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
		    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) == 1 &&
		    EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) == 1 &&
		    EVP_EncryptUpdate(ctx, ct, &len, (const guchar*)plain, (int)pt_len) == 1 &&
		    EVP_EncryptFinal_ex(ctx, ct + len, &fin) == 1)
		{
			g_byte_array_append(out, ct, len + fin);
			guchar tag[TAG_LEN];
			if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag) == 1)
			{
				g_byte_array_append(out, tag, TAG_LEN); /* 末尾 16 字节 tag */
				ok = TRUE;
			}
		}
		EVP_CIPHER_CTX_free(ctx);
	}
	g_free(ct);

	gchar* result = NULL;
	if (ok)
	{
		gchar* b64 = g_base64_encode(out->data, out->len);
		result = g_strconcat(PASS_PREFIX, b64, NULL);
		g_free(b64);
	}
	else
		result = g_strdup(plain);
	g_byte_array_free(out, TRUE);
	return result;
}

/* 解密密码字段：带 enc:v1: 前缀则解密，否则视为旧明文直接返回。
 * 解密失败返回 NULL（视为无密码）。 */
static gchar* password_decrypt(const gchar* stored)
{
	if (!stored || !*stored)
		return g_strdup("");

	if (!g_str_has_prefix(stored, PASS_PREFIX))
		return g_strdup(stored); /* 旧明文，向后兼容 */

	gsize b64_len = 0;
	guchar* raw = g_base64_decode(stored + strlen(PASS_PREFIX), &b64_len);
	if (!raw || b64_len < IV_LEN + TAG_LEN)
	{
		g_free(raw);
		return NULL;
	}

	guchar key[KEY_LEN];
	if (!secret_key_obtain(key))
	{
		g_free(raw);
		return NULL;
	}

	const guchar* iv = raw;
	const guchar* tag = raw + b64_len - TAG_LEN;
	const guchar* ct = raw + IV_LEN;
	int ct_len = (int)(b64_len - IV_LEN - TAG_LEN);

	guchar* pt = g_malloc((size_t)(ct_len > 0 ? ct_len : 1) + 1);
	gchar* result = NULL;

	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if (ctx)
	{
		int len = 0, fin = 0;
		if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
		    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) == 1 &&
		    EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) == 1 &&
		    EVP_DecryptUpdate(ctx, pt, &len, ct, ct_len) == 1 &&
		    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, (void*)tag) == 1 &&
		    EVP_DecryptFinal_ex(ctx, pt + len, &fin) == 1)
		{
			pt[len + fin] = '\0';
			result = g_strdup((gchar*)pt);
		}
		EVP_CIPHER_CTX_free(ctx);
	}

	g_free(pt);
	g_free(raw);
	return result;
}

Config* config_load(void)
{
	Config* cfg = g_new0(Config, 1);
	cfg->conns = g_ptr_array_new_with_free_func((GDestroyNotify)conn_free);

	gchar* path = config_path();
	GKeyFile* kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
	{
		/* 读取全局设置 */
		cfg->rdp_bin = g_key_file_get_string(kf, "settings", "rdp_bin", NULL);
		cfg->spice_bin = g_key_file_get_string(kf, "settings", "spice_bin", NULL);
		cfg->spice_prefix = g_key_file_get_string(kf, "settings", "spice_prefix", NULL);

		gchar** groups = g_key_file_get_groups(kf, NULL);
		for (gint i = 0; groups && groups[i]; i++)
		{
			if (!g_str_has_prefix(groups[i], "conn:"))
				continue;

			Conn* c = conn_new();
			/* 新格式 section 键为 conn:host|port|protocol，name 独立存于 "name" 字段；
			 * 旧格式 section 键为 conn:名称（无 "name" 字段），做向后兼容。 */
			if (g_key_file_has_key(kf, groups[i], "name", NULL))
				c->name = g_key_file_get_string(kf, groups[i], "name", NULL);
			else
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
			{
				gchar* stored_pass = g_key_file_get_string(kf, groups[i], "password", NULL);
				c->password = password_decrypt(stored_pass);
				g_free(stored_pass);
			}
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
		/* 用 host|port|protocol 作为唯一 section 键，避免同名连接互相覆盖 */
		gchar* group = g_strdup_printf("conn:%s|%d|%s",
		                               c->host ? c->host : "", c->port,
		                               c->protocol == PROTO_SPICE ? "spice" : "rdp");
		g_key_file_set_string(kf, group, "name", c->name ? c->name : "");
		g_key_file_set_string(kf, group, "host", c->host ? c->host : "");
		g_key_file_set_integer(kf, group, "port", c->port);
		g_key_file_set_string(kf, group, "protocol", c->protocol == PROTO_SPICE ? "spice" : "rdp");
		g_key_file_set_string(kf, group, "username", c->username ? c->username : "");
		{
			gchar* enc_pass = password_encrypt(c->password);
			g_key_file_set_string(kf, group, "password", enc_pass ? enc_pass : "");
			g_free(enc_pass);
		}
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

	/* 写入全局设置 */
	if (cfg->rdp_bin && *cfg->rdp_bin)
		g_key_file_set_string(kf, "settings", "rdp_bin", cfg->rdp_bin);
	if (cfg->spice_bin && *cfg->spice_bin)
		g_key_file_set_string(kf, "settings", "spice_bin", cfg->spice_bin);
	if (cfg->spice_prefix && *cfg->spice_prefix)
		g_key_file_set_string(kf, "settings", "spice_prefix", cfg->spice_prefix);

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
	g_free(cfg->rdp_bin);
	g_free(cfg->spice_bin);
	g_free(cfg->spice_prefix);
	g_free(cfg);
}

/* 构造连接命令行参数数组 */
gchar** conn_build_argv(Conn* c, const gchar* binary, const gchar* spice_prefix)
{
	GPtrArray* a = g_ptr_array_new();
	const gchar* prefix = spice_prefix && *spice_prefix ? spice_prefix : "spice";

	if (c->protocol == PROTO_SPICE)
	{
		/* SPICE：remote-viewer <prefix>://host:port[?password=xxx] [-f] [extra] */
		g_ptr_array_add(a, g_strdup(binary));
		gchar* uri = (c->password && *c->password)
		                 ? g_strdup_printf("%s://%s:%d?password=%s", prefix, c->host, c->port, c->password)
		                 : g_strdup_printf("%s://%s:%d", prefix, c->host, c->port);
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
