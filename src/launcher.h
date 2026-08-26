#ifndef XFREERDP_LAUNCHER_H
#define XFREERDP_LAUNCHER_H

#include <gtk/gtk.h>

/* 连接协议 */
typedef enum
{
	PROTO_RDP,   /* 远程桌面，用 xfreerdp */
	PROTO_SPICE  /* SPICE，用 remote-viewer */
} ConnProtocol;

/* 一条连接配置 */
typedef struct Conn
{
	gchar* name;       /* 显示名 */
	gchar* host;       /* 主机/IP */
	gint   port;       /* 端口，RDP 默认 3389，SPICE 默认 5900 */
	ConnProtocol protocol;
	gchar* username;
	gchar* password;
	gint   width;      /* >0 且非全屏时生效 */
	gint   height;
	gboolean fullscreen;
	gboolean gfx;      /* /gfx 使用 GFX/RemoteFX */
	gboolean clipboard;/* +clipboard 启用剪贴板重定向 */
	gboolean favorite; /* 是否收藏 */
	gchar* group;      /* 分组名（可空，未分组） */
	gchar* extra;      /* 额外参数（shell 风格，可多个） */
} Conn;

typedef struct Config
{
	GPtrArray* conns;  /* Conn* 列表 */
} Config;

Config* config_load(void);
gboolean config_save(Config* cfg, GError** err);

Conn* conn_new(void);
void conn_free(Conn* c);
void config_free(Config* cfg);

/* 构造连接命令行参数数组（RDP 用 xfreerdp，SPICE 用 remote-viewer），返回后需 g_strfreev 释放 */
gchar** conn_build_argv(Conn* c, const gchar* binary);

#endif /* XFREERDP_LAUNCHER_H */
