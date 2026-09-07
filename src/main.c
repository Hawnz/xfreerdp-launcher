/* xfreerdp-launcher - 基于 GTK3 的 xfreerdp 启动器
 * 参照微软 Windows App（原 Remote Desktop）Connection Center 卡片式布局：
 *   卡片网格主页 + 右键菜单 + 收藏/分组筛选 + 搜索框 + 可折叠日志
 * 直接调用系统 PATH 中的 xfreerdp（可用环境变量 XFREERDP_BIN 覆盖）
 */
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <gtk/gtk.h>

#include "launcher.h"

/* ---------------- 样式表 ---------------- */
static const gchar* CSS_DATA =
    ".conn-card {"
    "  background-color: alpha(@theme_fg_color, 0.10);"
    "  border: 1px solid alpha(@theme_fg_color, 0.25);"
    "  border-radius: 12px;"
    "  box-shadow: 0 1px 3px alpha(#000, 0.12);"
    "  min-width: 210px;"
    "  min-height: 100px;"
    "  cursor: pointer;"
    "}"
    ".conn-card:hover {"
    "  background-color: alpha(@theme_selected_bg_color, 0.08);"
    "  border-color: alpha(@theme_selected_bg_color, 0.40);"
    "  box-shadow: 0 1px 4px alpha(#000, 0.15);"
    "}"
    /* FlowBoxChild 的 CSS 节点已改名为 "card-cell"（见 rename_flowboxchild_css_node），
     * 主题的 .flowboxchild:selected 整格蓝色高亮不再匹配。这里显式保持单元格透明，
     * 选中高亮完全由卡片自身的 .conn-card.card-selected 样式呈现。 */
    ".card-cell,"
    ".card-cell:selected {"
    "  background-color: transparent;"
    "}"
    ".conn-card.card-selected {"
    "  background-color: alpha(@theme_selected_bg_color, 0.12);"
    "  border-color: alpha(@theme_selected_bg_color, 0.80);"
    "  box-shadow: 0 0 0 1px alpha(@theme_selected_bg_color, 0.30),"
    "              0 2px 8px alpha(#000, 0.22);"
    "}"
    ".conn-name { font-size: 14px; font-weight: bold; }"
    ".conn-sub { font-size: 12px; opacity: 0.65; }"
    ".group-pill { font-size: 11px; opacity: 0.75; padding: 1px 8px; border-radius: 999px;"
    "  border: 1px solid alpha(@theme_fg_color, 0.25); background-color: alpha(@theme_fg_color, 0.06); }"
    ".star { color: #ffb400; }"
    ".star-empty { color: alpha(@theme_fg_color, 0.75); }"
    ".proto-pill { font-size: 10px; font-weight: bold; padding: 1px 8px; border-radius: 999px; }"
    ".proto-rdp { color: @theme_selected_fg_color; background-color: @theme_selected_bg_color; }"
    ".proto-spice { color: #ffffff; background-color: alpha(#c9641d, 0.88); }"
    ".filter-bar { padding: 8px 14px 2px 14px; }"
    ".chip { border-radius: 999px; padding: 2px 14px; margin: 2px 4px; }"
    ".chip:hover { background-color: alpha(@theme_selected_bg_color, 0.10); }"
    ".chip:checked { background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; }"
    ".chip radio,"
    ".chip radio:checked,"
    ".chip radio:dir(ltr),"
    ".chip radio:dir(rtl) {"
    "  min-width: 0; min-height: 0; margin: 0; padding: 0;"
    "  border: none; background: none; box-shadow: none;"
    "  -gtk-icon-source: none;"
    "}"
    ".log-view { font-family: 'DejaVu Sans Mono', monospace; font-size: 12px; }"
    ".section-title { font-weight: bold; opacity: 0.7; padding: 10px 14px 4px 14px; }"
    ".placeholder { opacity: 0.55; }"
    ".statusbar { padding: 4px 12px; border-top: 1px solid alpha(#000, 0.12);"
    "  background-color: alpha(@theme_fg_color, 0.05); }"
    ".conn-dialog entry { border-radius: 6px; padding: 4px 8px; min-width: 300px; }"
    ".conn-dialog .field-label { font-weight: 600; opacity: 0.7; }";

typedef struct Launcher
{
	Config* config;
	gchar* binary;        /* RDP 客户端（xfreerdp） */
	gchar* spice_binary;  /* SPICE 客户端（remote-viewer） */

	GtkWidget* window;
	GtkWidget* flow;         /* GtkFlowBox 卡片网格 */
	GtkWidget* left_stack;   /* 网格/空状态/无匹配 切换 */
	GtkWidget* filter_bar;   /* 全部/收藏/分组 chips */
	GtkWidget* search;       /* GtkSearchEntry */
	GtkWidget* search_btn;   /* 搜索图标按钮（点击展开为搜索框） */
	GtkWidget* log_revealer; /* 可折叠日志 */
	GtkWidget* log_view;     /* GtkTextView */
	GtkWidget* log_toggle;
	GtkWidget* status;
	GtkWidget* connect_btn;
	GtkWidget* disconnect_btn;

	GPtrArray* groups;       /* 全部分组名（gchar*，去重） */
	gint filter_mode;        /* 0=全部 1=收藏 2=按分组 */
	gchar* filter_group;     /* filter_mode==2 时的分组名（指向 groups） */

	GPid child_pid;
	gboolean child_running;
	GIOChannel* out_ch;
	GIOChannel* err_ch;
	guint out_watch;
	guint err_watch;
	guint child_watch;
} Launcher;

static void populate_grid(Launcher* self);
static void filter_bar_rebuild(Launcher* self);
static gboolean on_card_button_press(GtkWidget* evbox, GdkEventButton* ev, gpointer data);
static void on_chip_toggled(GtkToggleButton* tb, gpointer data);

static void load_css(void)
{
	GtkCssProvider* provider = gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider, CSS_DATA, -1, NULL);
	gtk_style_context_add_provider_for_screen(
	    gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
	    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);
}

/* 带图标的按钮 */
static GtkWidget* btn_icon(const gchar* label, const gchar* icon)
{
	GtkWidget* b = gtk_button_new_with_label(label);
	gtk_button_set_image(GTK_BUTTON(b),
	                     gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON));
	gtk_button_set_always_show_image(GTK_BUTTON(b), TRUE);
	return b;
}

/* 日志缓冲上限（字符数），超出后从头部裁剪，防止长时间运行内存无限增长 */
#define LOG_MAX_CHARS (200 * 1024)

static void log_append(Launcher* self, const gchar* text)
{
	GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->log_view));
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(buf, &end);
	gtk_text_buffer_insert(buf, &end, text, -1);

	/* 限制长度：超过上限时删除最早的部分 */
	gint total = gtk_text_buffer_get_char_count(buf);
	if (total > LOG_MAX_CHARS)
	{
		GtkTextIter start, cut;
		gtk_text_buffer_get_start_iter(buf, &start);
		gtk_text_buffer_get_iter_at_offset(buf, &cut, total - LOG_MAX_CHARS);
		gtk_text_buffer_delete(buf, &start, &cut);
	}

	/* 滚动到底部 */
	gtk_text_buffer_get_end_iter(buf, &end);
	GtkTextMark* mark = gtk_text_buffer_create_mark(buf, NULL, &end, FALSE);
	gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(self->log_view), mark, 0.0, FALSE, 0.0, 0.0);
	gtk_text_buffer_delete_mark(buf, mark);
}

/* 读取子进程 stdout/stderr 并追加到日志 */
static gboolean on_child_output(GIOChannel* ch, GIOCondition cond, gpointer data)
{
	Launcher* self = data;
	gchar buf[4096];
	gsize sz = 0;
	GError* err = NULL;

	GIOStatus st = g_io_channel_read_chars(ch, buf, sizeof(buf) - 1, &sz, &err);
	if (sz > 0)
	{
		buf[sz] = '\0';
		log_append(self, buf);
	}
	if (err)
	{
		log_append(self, err->message);
		g_error_free(err);
		return FALSE;
	}
	if ((cond & (G_IO_HUP | G_IO_ERR)) || st == G_IO_STATUS_EOF)
		return FALSE; /* 移除监听 */
	return TRUE;
}

static void child_cleanup(Launcher* self)
{
	if (self->out_watch)
	{
		g_source_remove(self->out_watch);
		self->out_watch = 0;
	}
	if (self->err_watch)
	{
		g_source_remove(self->err_watch);
		self->err_watch = 0;
	}
	if (self->out_ch)
	{
		g_io_channel_shutdown(self->out_ch, TRUE, NULL);
		g_io_channel_unref(self->out_ch);
		self->out_ch = NULL;
	}
	if (self->err_ch)
	{
		g_io_channel_shutdown(self->err_ch, TRUE, NULL);
		g_io_channel_unref(self->err_ch);
		self->err_ch = NULL;
	}
}

/* 终止子进程：POSIX 发 SIGTERM，Windows 调 TerminateProcess。 */
static void terminate_child(GPid pid)
{
	if (!pid)
		return;
#ifdef _WIN32
	TerminateProcess((HANDLE)pid, 1);
#else
	kill(pid, SIGTERM);
#endif
}

static void on_child_exit(GPid pid, gint status, gpointer data)
{
	Launcher* self = data;
	self->child_running = FALSE;
	self->child_pid = 0;
	if (self->child_watch)
	{
		g_source_remove(self->child_watch);
		self->child_watch = 0;
	}
	child_cleanup(self);
	g_spawn_close_pid(pid);

	gtk_widget_set_sensitive(self->connect_btn, TRUE);
	gtk_widget_set_sensitive(self->disconnect_btn, FALSE);

	/* 区分正常退出与异常退出，给出更明确的状态提示。
	 * POSIX 下 status 是 wait 状态需用宏解析；Windows 下 g_child_watch 直接给退出码。 */
#ifdef _WIN32
	gboolean ok_exit = (status == 0);
#else
	gboolean ok_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
	gtk_label_set_text(GTK_LABEL(self->status), ok_exit ? "已断开" : "连接异常结束");

	gchar* msg = ok_exit
	                 ? g_strdup("\n[已正常退出]\n")
	                 : g_strdup_printf("\n[异常退出，状态码 %d]\n", status);
	log_append(self, msg);
	g_free(msg);
}

/* 子进程 setup 回调：把 xfreerdp 的 stdin 显式重定向到空设备（/dev/null 或 NUL）。
 * 未配置密码时 xfreerdp 会从 stdin 交互式提示输入密码；读到 EOF 后立即以
 * “缺少凭据”失败退出，而不是挂起等待输入。 */
static void child_setup_stdin_devnull(gpointer user_data)
{
	(void)user_data;
#ifdef _WIN32
	int fd = _open("NUL", _O_RDONLY);
	if (fd >= 0)
	{
		_dup2(fd, 0); /* STDIN_FILENO == 0 */
		if (fd != 0)
			_close(fd);
	}
#else
	int fd = open("/dev/null", O_RDONLY);
	if (fd >= 0)
	{
		dup2(fd, STDIN_FILENO);
		if (fd != STDIN_FILENO)
			close(fd);
	}
#endif
}

/* 连接缺少密码（或用户名）时弹窗补录，不写入配置。
 * 成功返回新分配的密码字符串（可为空串），取消返回 NULL。
 * 若需补录用户名，通过 out_user 返回（由调用方 g_free）。 */
static gchar* prompt_credentials(Launcher* self, Conn* c, gchar** out_user)
{
	*out_user = NULL;
	GtkWidget* dlg = gtk_dialog_new_with_buttons(
	    "输入凭据", GTK_WINDOW(self->window),
	    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	    "_取消", GTK_RESPONSE_CANCEL, "_连接", GTK_RESPONSE_ACCEPT, NULL);
	gtk_style_context_add_class(gtk_widget_get_style_context(dlg), "conn-dialog");
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);

	GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	GtkWidget* grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
	gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

	gchar* mark = g_markup_printf_escaped(
	    "<b>%s</b> 需要输入凭据：", c->name && *c->name ? c->name : c->host);
	GtkWidget* tip = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(tip), mark);
	g_free(mark);
	gtk_widget_set_halign(tip, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), tip, 0, 0, 2, 1);

	GtkWidget *e_user = NULL, *e_pass = NULL;
	gint row = 1;

#define ADD_LABEL_ENTRY(label, entry)                                       \
	do                                                                       \
	{                                                                        \
		GtkWidget* lab = gtk_label_new(label);                               \
		gtk_widget_set_halign(lab, GTK_ALIGN_START);                         \
		gtk_style_context_add_class(gtk_widget_get_style_context(lab), "field-label"); \
		gtk_grid_attach(GTK_GRID(grid), lab, 0, row, 1, 1);                  \
		entry = gtk_entry_new();                                             \
		gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);                \
		row++;                                                               \
	} while (0)

	if ((!c->username || !*c->username) && c->protocol == PROTO_RDP)
	{
		ADD_LABEL_ENTRY("用户名", e_user);
		gtk_entry_set_placeholder_text(GTK_ENTRY(e_user), "可留空");
	}
	ADD_LABEL_ENTRY("密码", e_pass);
	gtk_entry_set_visibility(GTK_ENTRY(e_pass), FALSE);
	gtk_entry_set_placeholder_text(GTK_ENTRY(e_pass), "密码（可留空）");
	gtk_entry_set_activates_default(GTK_ENTRY(e_pass), TRUE);

#undef ADD_LABEL_ENTRY

	gtk_widget_show_all(dlg);
	gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
	gchar* pass = NULL;
	if (resp == GTK_RESPONSE_ACCEPT)
	{
		if (e_user)
			*out_user = g_strdup(gtk_entry_get_text(GTK_ENTRY(e_user)));
		pass = g_strdup(gtk_entry_get_text(GTK_ENTRY(e_pass)));
	}
	gtk_widget_destroy(dlg);
	return pass;
}

/* 将命令行中的密码（/p: 与 SPICE 的 password=）掩码为 ***（返回新字符串，调用方 g_free） */
static gchar* mask_cmdline(const gchar* cmdline)
{
	GString* s = g_string_new(NULL);
	const gchar* cur = cmdline;
	while (TRUE)
	{
		const gchar* p = strstr(cur, "/p:");
		const gchar* q = strstr(cur, "password=");
		const gchar* hit = NULL;
		gsize prefix = 0;
		const gchar* repl = NULL;
		if (p && (!q || p < q))
		{
			hit = p;
			prefix = 3;
			repl = "/p:***";
		}
		else if (q)
		{
			hit = q;
			prefix = 9;
			repl = "password=***";
		}
		if (!hit)
		{
			g_string_append(s, cur);
			break;
		}
		g_string_append_len(s, cur, hit - cur);
		g_string_append(s, repl);
		const gchar* end = hit + prefix;
		while (*end && *end != ' ')
			end++;
		cur = end;
	}
	return g_string_free(s, FALSE);
}

static void connect_conn(Launcher* self, Conn* c)
{
	if (!c)
		return;
	if (self->child_running)
	{
		gtk_label_set_text(GTK_LABEL(self->status), "已有连接在运行");
		return;
	}

	/* 缺少密码时先弹窗补录（不写入配置）；取消则中止连接。
	 * SPICE 密码可以为空，不弹窗。 */
	gchar* tmp_user = NULL;
	gchar* tmp_pass = NULL;
	if (c->protocol == PROTO_RDP && (!c->password || !*c->password))
	{
		tmp_pass = prompt_credentials(self, c, &tmp_user);
		if (!tmp_pass)
		{
			gtk_label_set_text(GTK_LABEL(self->status), "已取消");
			return;
		}
	}

	Conn tmp = *c; /* 浅拷贝：仅本次连接覆盖凭据，不改配置 */
	if (tmp_pass)
		tmp.password = tmp_pass;
	if (tmp_user && *tmp_user)
		tmp.username = tmp_user;

	/* 按协议选择客户端：RDP 用 xfreerdp，SPICE 用 remote-viewer */
	const gchar* bin = (tmp.protocol == PROTO_SPICE) ? self->spice_binary : self->binary;
	gchar** argv = conn_build_argv(&tmp, bin, self->config->spice_prefix);
	g_free(tmp_user);
	g_free(tmp_pass);
	gchar* cmdline = g_strjoinv(" ", argv);
	/* 日志脱敏：把命令行里所有 /p:密码 掩成 /p:***，避免明文密码入日志 */
	gchar* masked = mask_cmdline(cmdline);

	GPid pid = 0;
	gint out_fd = -1, err_fd = -1;
	GError* err = NULL;
	gboolean ok = g_spawn_async_with_pipes(
	    NULL, argv, NULL,
	    G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
	    child_setup_stdin_devnull, NULL, &pid, NULL, &out_fd, &err_fd, &err);

	if (!ok)
	{
		gchar* m = g_strdup_printf("启动远程桌面客户端失败：%s\n", err ? err->message : "未知错误");
		log_append(self, m);
		if (err)
			g_error_free(err);
		g_free(m);
		g_strfreev(argv);
		g_free(cmdline);
		g_free(masked);
		gtk_label_set_text(GTK_LABEL(self->status), "启动失败");
		return;
	}

	self->child_pid = pid;
	self->child_running = TRUE;
	gtk_widget_set_sensitive(self->connect_btn, FALSE);
	gtk_widget_set_sensitive(self->disconnect_btn, TRUE);
	{
		gchar* st = g_strdup_printf("连接中：%s:%d（%s）", c->host, c->port,
		                           c->protocol == PROTO_SPICE ? "SPICE" : "RDP");
		gtk_label_set_text(GTK_LABEL(self->status), st);
		g_free(st);
	}
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->log_toggle), TRUE); /* 自动展开日志 */

	gchar* msg = g_strdup_printf("正在连接 %s:%d ...\n$ %s\n", c->host, c->port, masked);
	log_append(self, msg);
	g_free(msg);
	g_free(cmdline);
	g_free(masked);

	#ifdef _WIN32
	self->out_ch = g_io_channel_win32_new_fd(out_fd);
	self->err_ch = g_io_channel_win32_new_fd(err_fd);
#else
	self->out_ch = g_io_channel_unix_new(out_fd);
	self->err_ch = g_io_channel_unix_new(err_fd);
#endif
	g_io_channel_set_encoding(self->out_ch, NULL, NULL);
	g_io_channel_set_encoding(self->err_ch, NULL, NULL);
	self->out_watch = g_io_add_watch(self->out_ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_child_output, self);
	self->err_watch = g_io_add_watch(self->err_ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_child_output, self);
	self->child_watch = g_child_watch_add(pid, on_child_exit, self);

	g_strfreev(argv);
}

static void on_connect(GtkButton* btn, gpointer data)
{
	(void)btn;
	Launcher* self = data;
	GList* sel = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(self->flow));
	Conn* c = NULL;
	if (sel)
	{
		c = g_object_get_data(G_OBJECT(sel->data), "conn");
		g_list_free(sel);
	}
	if (!c)
	{
		gtk_label_set_text(GTK_LABEL(self->status), "请先选择要连接的卡片");
		return;
	}
	connect_conn(self, c);
}

static void on_disconnect(GtkButton* btn, gpointer data)
{
	(void)btn;
	Launcher* self = data;
	if (self->child_running)
	{
		terminate_child(self->child_pid);
		log_append(self, "\n[已发送终止信号]\n");
		gtk_label_set_text(GTK_LABEL(self->status), "正在终止…");
	}
}

static void on_window_destroy(GtkWidget* widget, gpointer data)
{
	(void)widget;
	Launcher* self = data;
	if (self->child_running)
		terminate_child(self->child_pid);
}

/* ---------------- 卡片网格 ---------------- */

/* 把分组字符串按逗号拆分，去首尾空白并忽略空项。
 * 例如 "生产,  办公 ," → ["生产", "办公"]。返回需 g_ptr_array_unref。 */
static GPtrArray* group_split(const gchar* group)
{
	GPtrArray* arr = g_ptr_array_new_with_free_func(g_free);
	if (!group || !*group)
		return arr;
	gchar** parts = g_strsplit(group, ",", -1);
	for (gint i = 0; parts && parts[i]; i++)
	{
		gchar* t = g_strstrip(parts[i]);
		if (*t)
			g_ptr_array_add(arr, g_strdup(t));
	}
	g_strfreev(parts);
	return arr;
}

static GtkWidget* card_new(Launcher* self, Conn* c)
{
	GtkWidget* ev = gtk_event_box_new();
	gtk_style_context_add_class(gtk_widget_get_style_context(ev), "conn-card");
	g_object_set_data(G_OBJECT(ev), "conn", c);
	/* 防止卡片被 FlowBox 拉伸占满整行高度；横向铺满列宽保持统一 */
	gtk_widget_set_valign(ev, GTK_ALIGN_START);
	gtk_widget_set_hexpand(ev, TRUE);

	GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
	gtk_widget_set_margin_start(vbox, 14);
	gtk_widget_set_margin_end(vbox, 14);
	gtk_widget_set_margin_top(vbox, 12);
	gtk_widget_set_margin_bottom(vbox, 12);
	gtk_container_add(GTK_CONTAINER(ev), vbox);

	/* 顶行：图标 + 协议徽标 + 收藏星标 */
	GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_pack_start(GTK_BOX(top),
	                   gtk_image_new_from_icon_name("computer", GTK_ICON_SIZE_LARGE_TOOLBAR),
	                   FALSE, FALSE, 0);

	/* 协议徽标（RDP / SPICE） */
	GtkWidget* proto = gtk_label_new(c->protocol == PROTO_SPICE ? "SPICE" : "RDP");
	gtk_widget_set_valign(proto, GTK_ALIGN_CENTER);
	gtk_style_context_add_class(gtk_widget_get_style_context(proto), "proto-pill");
	gtk_style_context_add_class(gtk_widget_get_style_context(proto),
	                            c->protocol == PROTO_SPICE ? "proto-spice" : "proto-rdp");
	gtk_box_pack_start(GTK_BOX(top), proto, FALSE, FALSE, 0);

	GtkWidget* star = gtk_image_new_from_icon_name(c->favorite ? "starred" : "non-starred",
	                                               GTK_ICON_SIZE_LARGE_TOOLBAR);
	gtk_style_context_add_class(gtk_widget_get_style_context(star),
	                            c->favorite ? "star" : "star-empty");
	gtk_widget_set_halign(star, GTK_ALIGN_END);
	gtk_widget_set_valign(star, GTK_ALIGN_START);
	gtk_box_pack_end(GTK_BOX(top), star, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);

	/* 名称 */
	GtkWidget* name = gtk_label_new(c->name && *c->name ? c->name : c->host);
	gtk_label_set_xalign(GTK_LABEL(name), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	gtk_widget_set_halign(name, GTK_ALIGN_START);
	gtk_style_context_add_class(gtk_widget_get_style_context(name), "conn-name");
	gtk_box_pack_start(GTK_BOX(vbox), name, FALSE, FALSE, 0);

	/* 主机:端口 */
	gchar* sub_txt = g_strdup_printf("%s:%d", c->host, c->port);
	GtkWidget* sub = gtk_label_new(sub_txt);
	g_free(sub_txt);
	gtk_label_set_xalign(GTK_LABEL(sub), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
	gtk_widget_set_halign(sub, GTK_ALIGN_START);
	gtk_widget_set_hexpand(sub, TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(sub), "conn-sub");
	gtk_box_pack_start(GTK_BOX(vbox), sub, FALSE, FALSE, 0);

	/* 分组标签（支持逗号分隔的多分组，横向排列） */
	GPtrArray* gs = group_split(c->group);
	if (gs->len > 0)
	{
		GtkWidget* gbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
		gtk_widget_set_halign(gbox, GTK_ALIGN_START);
		for (guint k = 0; k < gs->len; k++)
		{
			GtkWidget* gp = gtk_label_new(g_ptr_array_index(gs, k));
			gtk_label_set_xalign(GTK_LABEL(gp), 0.0);
			gtk_label_set_ellipsize(GTK_LABEL(gp), PANGO_ELLIPSIZE_END);
			gtk_style_context_add_class(gtk_widget_get_style_context(gp), "group-pill");
			gtk_box_pack_start(GTK_BOX(gbox), gp, FALSE, FALSE, 0);
		}
		gtk_box_pack_start(GTK_BOX(vbox), gbox, FALSE, FALSE, 0);
	}
	g_ptr_array_unref(gs);

	g_signal_connect(ev, "button-press-event", G_CALLBACK(on_card_button_press), self);
	return ev;
}

static gboolean conn_match_filter(Launcher* self, Conn* c)
{
	if (self->filter_mode == 1 && !c->favorite)
		return FALSE;
	if (self->filter_mode == 2)
	{
		if (!self->filter_group)
			return FALSE;
		/* 连接可能属于多个分组（逗号分隔），任一分组命中即可 */
		gboolean hit = FALSE;
		GPtrArray* gs = group_split(c->group);
		for (guint k = 0; k < gs->len; k++)
		{
			if (g_strcmp0(g_ptr_array_index(gs, k), self->filter_group) == 0)
			{
				hit = TRUE;
				break;
			}
		}
		g_ptr_array_unref(gs);
		if (!hit)
			return FALSE;
	}

	const gchar* q = self->search ? gtk_entry_get_text(GTK_ENTRY(self->search)) : "";
	if (q && *q)
	{
		gchar* ql = g_utf8_strdown(q, -1);
		gchar* nl = g_utf8_strdown(c->name ? c->name : "", -1);
		gchar* hl = g_utf8_strdown(c->host ? c->host : "", -1);
		gchar* ul = g_utf8_strdown(c->username ? c->username : "", -1);
		gchar* gl = g_utf8_strdown(c->group ? c->group : "", -1);
		const gchar* proto = (c->protocol == PROTO_SPICE) ? "spice" : "rdp";
		gboolean hit = strstr(nl, ql) != NULL || strstr(hl, ql) != NULL ||
		               strstr(ul, ql) != NULL || strstr(gl, ql) != NULL ||
		               strstr(proto, ql) != NULL;
		g_free(ql);
		g_free(nl);
		g_free(hl);
		g_free(ul);
		g_free(gl);
		if (!hit)
			return FALSE;
	}
	return TRUE;
}

static void populate_grid(Launcher* self)
{
	GList* children = gtk_container_get_children(GTK_CONTAINER(self->flow));
	for (GList* it = children; it; it = it->next)
		gtk_widget_destroy(GTK_WIDGET(it->data));
	g_list_free(children);

	guint shown = 0;
	for (guint i = 0; i < self->config->conns->len; i++)
	{
		Conn* c = g_ptr_array_index(self->config->conns, i);
		if (!conn_match_filter(self, c))
			continue;
		GtkWidget* card = card_new(self, c);
		gtk_flow_box_insert(GTK_FLOW_BOX(self->flow), card, -1);
		g_object_set_data(G_OBJECT(gtk_widget_get_parent(card)), "conn", c);
		/* 窗口已 show_all 后新插入的卡片不会被自动显示：gtk_widget_show 只显示
		 * 顶层 widget，其内部 vbox/标签/图标仍是隐藏的，隐藏子项使卡片高度请求
		 * 变为 0，最终整格 1x1 不可见（“切换标签卡消失”根因）。
		 * 必须用 show_all 递归显示整棵卡片子树。 */
		gtk_widget_show_all(card);
		shown++;
	}

	if (self->config->conns->len == 0)
		gtk_stack_set_visible_child_name(GTK_STACK(self->left_stack), "empty");
	else if (shown == 0)
		gtk_stack_set_visible_child_name(GTK_STACK(self->left_stack), "no-match");
	else
		gtk_stack_set_visible_child_name(GTK_STACK(self->left_stack), "grid");
}

/* ---------------- 筛选 chips（全部/收藏/分组） ---------------- */

/* 用 GtkRadioButton 实现互斥且不可取消的筛选 chip */
static GtkWidget* chip_new(const gchar* label, gint mode, gint group_idx,
                           GtkRadioButton* group, Launcher* self)
{
	GtkWidget* b = group
	                   ? gtk_radio_button_new_with_label_from_widget(group, label)
	                   : gtk_radio_button_new_with_label(NULL, label);
	gtk_style_context_add_class(gtk_widget_get_style_context(b), "chip");
	g_object_set_data(G_OBJECT(b), "mode", GINT_TO_POINTER(mode));
	g_object_set_data(G_OBJECT(b), "group_idx", GINT_TO_POINTER(group_idx));
	g_signal_connect(b, "toggled", G_CALLBACK(on_chip_toggled), self);
	return b;
}

static void on_chip_toggled(GtkToggleButton* tb, gpointer data)
{
	Launcher* self = data;
	if (!gtk_toggle_button_get_active(tb))
		return;
	self->filter_mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tb), "mode"));
	if (self->filter_mode == 2)
	{
		gint idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tb), "group_idx"));
		self->filter_group = g_ptr_array_index(self->groups, idx);
	}
	else
		self->filter_group = NULL;
	populate_grid(self);
}

static void filter_bar_rebuild(Launcher* self)
{
	GList* children = gtk_container_get_children(GTK_CONTAINER(self->filter_bar));
	for (GList* it = children; it; it = it->next)
		gtk_widget_destroy(GTK_WIDGET(it->data));
	g_list_free(children);

	/* 收集去重分组（每个连接的 group 字段按逗号拆分） */
	GPtrArray* groups = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < self->config->conns->len; i++)
	{
		Conn* c = g_ptr_array_index(self->config->conns, i);
		GPtrArray* gs = group_split(c->group);
		for (guint m = 0; m < gs->len; m++)
		{
			const gchar* g = g_ptr_array_index(gs, m);
			gboolean dup = FALSE;
			for (guint k = 0; k < groups->len; k++)
			{
				if (g_strcmp0(g_ptr_array_index(groups, k), g) == 0)
				{
					dup = TRUE;
					break;
				}
			}
			if (!dup)
				g_ptr_array_add(groups, g_strdup(g));
		}
		g_ptr_array_unref(gs);
	}

	/* 保存当前筛选分组名（旧数组即将释放，先复制出来避免悬空） */
	gchar* keep_group = (self->filter_mode == 2 && self->filter_group)
	                        ? g_strdup(self->filter_group)
	                        : NULL;

	if (self->groups)
		g_ptr_array_unref(self->groups);
	self->groups = groups;

	/* 重新定位 filter_group 到新数组；分组已不存在则回退「全部」 */
	self->filter_group = NULL;
	if (keep_group)
	{
		for (guint k = 0; k < groups->len; k++)
		{
			if (g_strcmp0(g_ptr_array_index(groups, k), keep_group) == 0)
			{
				self->filter_group = g_ptr_array_index(groups, k);
				break;
			}
		}
		g_free(keep_group);
		if (!self->filter_group)
			self->filter_mode = 0;
	}

	/* 「全部」为组内第一个单选按钮 */
	GtkWidget* c_all = chip_new("全部", 0, -1, NULL, self);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c_all), self->filter_mode == 0);
	gtk_box_pack_start(GTK_BOX(self->filter_bar), c_all, FALSE, FALSE, 0);
	GtkRadioButton* group = GTK_RADIO_BUTTON(c_all);

	GtkWidget* c_fav = chip_new("收藏", 1, -1, group, self);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c_fav), self->filter_mode == 1);
	gtk_box_pack_start(GTK_BOX(self->filter_bar), c_fav, FALSE, FALSE, 0);

	for (guint k = 0; k < groups->len; k++)
	{
		GtkWidget* chip = chip_new(g_ptr_array_index(groups, k), 2, k, group, self);
		gboolean active = (self->filter_mode == 2 &&
		                   g_strcmp0(self->filter_group, g_ptr_array_index(groups, k)) == 0);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chip), active);
		gtk_box_pack_start(GTK_BOX(self->filter_bar), chip, FALSE, FALSE, 0);
	}

	/* 新 chips 已被加入 filter_bar，但 filter_bar 在初始 show_all 后新增的子 widget
	 * 不会自动显示，必须显式显示整条 filter_bar 才能让新 chips 可见。 */
	gtk_widget_show_all(self->filter_bar);
}

static void on_search_changed(GtkSearchEntry* entry, gpointer data)
{
	(void)entry;
	Launcher* self = data;
	populate_grid(self);
}

/* 点击搜索图标：展开为搜索框并聚焦 */
static void on_search_btn_clicked(GtkButton* btn, gpointer data)
{
	(void)btn;
	Launcher* self = data;
	gtk_widget_hide(self->search_btn);
	gtk_widget_show(self->search);
	gtk_widget_grab_focus(self->search);
}

/* 搜索框失去焦点：内容为空则收起回图标 */
static gboolean on_search_focus_out(GtkWidget* entry, GdkEventFocus* ev, gpointer data)
{
	(void)ev;
	Launcher* self = data;
	const gchar* txt = gtk_entry_get_text(GTK_ENTRY(entry));
	if (!txt || !*txt)
	{
		gtk_widget_hide(self->search);
		gtk_widget_show(self->search_btn);
	}
	return FALSE;
}

/* 按 Esc 停止搜索：清空并收起 */
static void on_search_stop(GtkSearchEntry* entry, gpointer data)
{
	(void)entry;
	Launcher* self = data;
	gtk_entry_set_text(GTK_ENTRY(self->search), "");
	gtk_widget_hide(self->search);
	gtk_widget_show(self->search_btn);
}

/* 选中态视觉反馈 */
static void on_card_selected(GtkFlowBox* fb, gpointer data)
{
	Launcher* self = data;
	GList* all = gtk_container_get_children(GTK_CONTAINER(self->flow));
	for (GList* it = all; it; it = it->next)
	{
		GtkWidget* card = gtk_bin_get_child(GTK_BIN(it->data));
		gtk_style_context_remove_class(gtk_widget_get_style_context(card), "card-selected");
	}
	g_list_free(all);

	GList* sel = gtk_flow_box_get_selected_children(fb);
	if (sel)
	{
		GtkWidget* card = gtk_bin_get_child(GTK_BIN(sel->data));
		gtk_style_context_add_class(gtk_widget_get_style_context(card), "card-selected");
		g_list_free(sel);
	}
}

/* ---------------- 新建/编辑连接对话框 ---------------- */

/* 从 "主机[:端口]" 组合文本中解析主机与端口。
 * 仅当最后一个 ":" 之后为纯数字且范围合法时才作为端口解析，
 * 否则整串视为主机（端口用默认 3389）。返回的主机需 g_free。 */
static gchar* parse_host_port(const gchar* text, gint* port_out, gint default_port)
{
	gchar* host = g_strdup(text);
	gint port = default_port;
	gchar* colon = strrchr(host, ':');
	if (colon)
	{
		const gchar* ps = colon + 1;
		gboolean numeric = (*ps != '\0');
		if (numeric)
		{
			for (const gchar* p = ps; *p; p++)
			{
				if (!g_ascii_isdigit(*p))
				{
					numeric = FALSE;
					break;
				}
			}
		}
		if (numeric)
		{
			gint p = (gint)g_ascii_strtoll(ps, NULL, 10);
			if (p > 0 && p <= 65535)
			{
				port = p;
				*colon = '\0';
			}
			else
				*colon = '\0'; /* 端口非法，丢弃尾部分隔符，用默认端口 */
		}
		else if (*ps == '\0')
			*colon = '\0'; /* 结尾裸冒号，去掉 */
	}
	if (port_out)
		*port_out = port;
	return host;
}

/* 协议切换时显示/隐藏用户名行（SPICE 无用户名） */
typedef struct
{
	GtkGrid* grid;
	gint user_row;
	GtkToggleButton* r_rdp;
} ProtoUi;

static void on_proto_toggled(GtkToggleButton* tb, gpointer data)
{
	(void)tb;
	ProtoUi* ui = data;
	gboolean rdp = gtk_toggle_button_get_active(ui->r_rdp);
	if (ui->user_row < 0)
		return;
	GtkWidget* lab = gtk_grid_get_child_at(ui->grid, 0, ui->user_row);
	GtkWidget* ent = gtk_grid_get_child_at(ui->grid, 1, ui->user_row);
	if (lab)
		gtk_widget_set_visible(lab, rdp);
	if (ent)
		gtk_widget_set_visible(ent, rdp);
}

static gboolean conn_dialog(Launcher* self, Conn* c, GtkWindow* parent)
{
	gboolean is_new = (c == NULL);
	GtkWidget* dlg = gtk_dialog_new_with_buttons(
	    is_new ? "新建连接" : "编辑连接", parent,
	    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	    "_取消", GTK_RESPONSE_CANCEL, "_保存", GTK_RESPONSE_ACCEPT, NULL);
	gtk_style_context_add_class(gtk_widget_get_style_context(dlg), "conn-dialog");

	GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	GtkWidget* grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
	gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

	GtkWidget *e_name, *e_host, *e_user, *e_pass, *e_group, *e_extra;
	GtkWidget *c_full, *c_fav, *r_rdp, *r_spice;
	gint row = 0;

	/* 协议选择：RDP / SPICE */
	{
		GtkWidget* lab = gtk_label_new("类型");
		gtk_widget_set_halign(lab, GTK_ALIGN_START);
		gtk_style_context_add_class(gtk_widget_get_style_context(lab), "field-label");
		gtk_grid_attach(GTK_GRID(grid), lab, 0, row, 1, 1);
		GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
		r_rdp = gtk_radio_button_new_with_label(NULL, "RDP（远程桌面）");
		r_spice = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(r_rdp), "SPICE");
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r_rdp),
		                             is_new ? TRUE : (c->protocol == PROTO_RDP));
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r_spice),
		                             !is_new && c->protocol == PROTO_SPICE);
		gtk_box_pack_start(GTK_BOX(box), r_rdp, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(box), r_spice, FALSE, FALSE, 0);
		gtk_widget_set_halign(box, GTK_ALIGN_START);
		gtk_grid_attach(GTK_GRID(grid), box, 1, row, 1, 1);
		row++;
	}

#define ADD_LABEL_ENTRY(label, entry, text)                                  \
	do                                                                       \
	{                                                                        \
		GtkWidget* lab = gtk_label_new(label);                               \
		gtk_widget_set_halign(lab, GTK_ALIGN_START);                         \
		gtk_style_context_add_class(gtk_widget_get_style_context(lab), "field-label"); \
		gtk_grid_attach(GTK_GRID(grid), lab, 0, row, 1, 1);                  \
		entry = gtk_entry_new();                                             \
		if ((text))                                                          \
			gtk_entry_set_text(GTK_ENTRY(entry), (text));                    \
		gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);                \
		row++;                                                               \
	} while (0)

	ADD_LABEL_ENTRY("名称", e_name, is_new ? NULL : c->name);
	{
		/* 端口合并进主机：不写端口用协议默认（RDP 3389 / SPICE 5900） */
		gint def_port = (!is_new && c->protocol == PROTO_SPICE) ? 5900 : 3389;
		gchar* host_txt = NULL;
		if (!is_new)
			host_txt = (c->port != def_port) ? g_strdup_printf("%s:%d", c->host, c->port)
			                                 : g_strdup(c->host);
		ADD_LABEL_ENTRY("主机", e_host, host_txt);
		gtk_entry_set_placeholder_text(GTK_ENTRY(e_host), "节点:端口（RDP 默认 3389 / SPICE 默认 5900）");
		g_free(host_txt);
	}
	gint user_row = row;
	ADD_LABEL_ENTRY("用户名", e_user, is_new ? NULL : c->username);
	ADD_LABEL_ENTRY("密码", e_pass, is_new ? NULL : c->password);
	gtk_entry_set_visibility(GTK_ENTRY(e_pass), FALSE);
	ADD_LABEL_ENTRY("分组", e_group, is_new ? NULL : c->group);
	gtk_entry_set_placeholder_text(GTK_ENTRY(e_group), "多个分组用英文逗号隔开，如：生产,办公");
	ADD_LABEL_ENTRY("额外参数", e_extra, is_new ? NULL : c->extra);

	c_full = gtk_check_button_new_with_label("全屏 (-f)");
	c_fav = gtk_check_button_new_with_label("收藏");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c_full), !is_new && c->fullscreen);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c_fav), !is_new && c->favorite);
	GtkWidget* opts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
	gtk_box_pack_start(GTK_BOX(opts), c_full, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(opts), c_fav, FALSE, FALSE, 0);
	gtk_widget_set_halign(opts, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), opts, 1, row++, 1, 1);

#undef ADD_LABEL_ENTRY

	/* 协议切换联动：SPICE 隐藏用户名行 */
	ProtoUi ui = { GTK_GRID(grid), user_row, GTK_TOGGLE_BUTTON(r_rdp) };
	g_signal_connect(r_rdp, "toggled", G_CALLBACK(on_proto_toggled), &ui);
	g_signal_connect(r_spice, "toggled", G_CALLBACK(on_proto_toggled), &ui);
	on_proto_toggled(NULL, &ui);

	gtk_widget_show_all(dlg);
	gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
	gboolean ok = (resp == GTK_RESPONSE_ACCEPT);

	if (ok)
	{
		const gchar* host = gtk_entry_get_text(GTK_ENTRY(e_host));
		if (!host || !*host)
		{
			ok = FALSE;
		}
		else
		{
			Conn* nc = is_new ? conn_new() : c;
			gboolean is_spice = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r_spice));
			gint def_port = is_spice ? 5900 : 3389;
			gint port = def_port;
			gchar* host_only = parse_host_port(host, &port, def_port);
			g_free(nc->name);
			nc->name = g_strdup(gtk_entry_get_text(GTK_ENTRY(e_name)));
			g_free(nc->host);
			nc->host = host_only;
			nc->port = port;
			nc->protocol = is_spice ? PROTO_SPICE : PROTO_RDP;
			g_free(nc->username);
			/* SPICE 无用户名；RDP 从输入框读取 */
			nc->username = is_spice
			                   ? NULL
			                   : g_strdup(gtk_entry_get_text(GTK_ENTRY(e_user)));
			g_free(nc->password);
			nc->password = g_strdup(gtk_entry_get_text(GTK_ENTRY(e_pass)));
			g_free(nc->group);
			nc->group = g_strdup(gtk_entry_get_text(GTK_ENTRY(e_group)));
			if (nc->group && !*nc->group)
			{
				g_free(nc->group);
				nc->group = NULL;
			}
			/* 宽度/高度保持默认（0=自适应），gfx/clipboard 保留原值 */
			nc->fullscreen = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c_full));
			nc->favorite = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c_fav));
			g_free(nc->extra);
			nc->extra = g_strdup(gtk_entry_get_text(GTK_ENTRY(e_extra)));

			if (is_new)
				g_ptr_array_add(self->config->conns, nc);
			config_save(self->config, NULL);
			filter_bar_rebuild(self);
			populate_grid(self);
		}
	}

	gtk_widget_destroy(dlg);
	return ok;
}

/* ---------------- 删除 / 收藏 ---------------- */

static void delete_conn(Launcher* self, Conn* c)
{
	if (!c)
		return;
	GtkWidget* dlg = gtk_message_dialog_new(
	    GTK_WINDOW(self->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
	    "删除连接 \"%s\"？", c->name && *c->name ? c->name : c->host);
	gint r = gtk_dialog_run(GTK_DIALOG(dlg));
	gtk_widget_destroy(dlg);
	if (r != GTK_RESPONSE_YES)
		return;

	g_ptr_array_remove(self->config->conns, c); /* 触发 conn_free */
	config_save(self->config, NULL);
	filter_bar_rebuild(self);
	populate_grid(self);
	log_append(self, "已删除连接\n");
}

static void toggle_favorite(Launcher* self, Conn* c)
{
	if (!c)
		return;
	c->favorite = !c->favorite;
	config_save(self->config, NULL);
	populate_grid(self);
}

/* ---------------- 卡片右键菜单 ---------------- */

static void menu_connect(GtkMenuItem* mi, gpointer d)
{
	(void)d;
	Launcher* self = g_object_get_data(G_OBJECT(mi), "self");
	Conn* c = g_object_get_data(G_OBJECT(mi), "conn");
	connect_conn(self, c);
}

static void menu_toggle_fav(GtkMenuItem* mi, gpointer d)
{
	(void)d;
	Launcher* self = g_object_get_data(G_OBJECT(mi), "self");
	Conn* c = g_object_get_data(G_OBJECT(mi), "conn");
	toggle_favorite(self, c);
}

static void menu_edit(GtkMenuItem* mi, gpointer d)
{
	(void)d;
	Launcher* self = g_object_get_data(G_OBJECT(mi), "self");
	Conn* c = g_object_get_data(G_OBJECT(mi), "conn");
	if (conn_dialog(self, c, GTK_WINDOW(self->window)))
		log_append(self, "已保存修改\n");
}

static void menu_delete(GtkMenuItem* mi, gpointer d)
{
	(void)d;
	Launcher* self = g_object_get_data(G_OBJECT(mi), "self");
	Conn* c = g_object_get_data(G_OBJECT(mi), "conn");
	delete_conn(self, c);
}

static void menu_info(GtkMenuItem* mi, gpointer d)
{
	(void)d;
	Launcher* self = g_object_get_data(G_OBJECT(mi), "self");
	Conn* c = g_object_get_data(G_OBJECT(mi), "conn");
	if (!c)
		return;
	gchar* size;
	if (c->fullscreen)
		size = g_strdup("全屏");
	else if (c->width > 0 && c->height > 0)
		size = g_strdup_printf("%d x %d", c->width, c->height);
	else
		size = g_strdup("默认");
	gchar* body = g_strdup_printf(
	    "名称：%s\n协议：%s\n主机：%s:%d\n用户名：%s\n窗口：%s\n"
	    "分组：%s\n收藏：%s\n额外参数：%s",
	    c->name && *c->name ? c->name : c->host,
	    c->protocol == PROTO_SPICE ? "SPICE" : "RDP",
	    c->host, c->port,
	    c->username && *c->username ? c->username : "（未设置）",
	    size,
	    c->group && *c->group ? c->group : "（未分组）",
	    c->favorite ? "是" : "否",
	    c->extra && *c->extra ? c->extra : "（无）");
	GtkWidget* dlg = gtk_message_dialog_new(GTK_WINDOW(self->window), GTK_DIALOG_MODAL,
	                                        GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", body);
	gtk_dialog_run(GTK_DIALOG(dlg));
	gtk_widget_destroy(dlg);
	g_free(body);
	g_free(size);
}

static gboolean on_card_button_press(GtkWidget* evbox, GdkEventButton* ev, gpointer data)
{
	Launcher* self = data;
	Conn* c = g_object_get_data(G_OBJECT(evbox), "conn");
	GtkFlowBoxChild* child = (GtkFlowBoxChild*)gtk_widget_get_parent(evbox);

	/* 左键：单击选中卡片，再次点击已选中的卡片才连接 */
	if (ev->button == 1)
	{
		GList* sel = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(self->flow));
		gboolean already_selected = FALSE;
		for (GList* it = sel; it; it = it->next)
		{
			if (it->data == (gpointer)child)
			{
				already_selected = TRUE;
				break;
			}
		}
		g_list_free(sel);

		if (already_selected && c)
		{
			connect_conn(self, c);
			return TRUE;
		}
		return FALSE; /* 未选中：交回 FlowBox 完成选中 */
	}

	if (ev->button != 3)
		return FALSE;
	if (child)
		gtk_flow_box_select_child(GTK_FLOW_BOX(self->flow), child);
	if (!c)
		return FALSE;

	GtkWidget* menu = gtk_menu_new();
#define ADD_MENU_ITEM(label, cb)                                  \
	do                                                            \
	{                                                             \
		GtkWidget* mi = gtk_menu_item_new_with_label(label);      \
		g_object_set_data(G_OBJECT(mi), "self", self);            \
		g_object_set_data(G_OBJECT(mi), "conn", c);               \
		g_signal_connect(mi, "activate", G_CALLBACK(cb), NULL);   \
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);          \
	} while (0)

	ADD_MENU_ITEM("连接", menu_connect);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	ADD_MENU_ITEM(c->favorite ? "取消收藏" : "收藏", menu_toggle_fav);
	ADD_MENU_ITEM("编辑…", menu_edit);
	ADD_MENU_ITEM("删除…", menu_delete);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	ADD_MENU_ITEM("详情", menu_info);

#undef ADD_MENU_ITEM

	gtk_widget_show_all(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)ev);
	return TRUE;
}

/* ---------------- 工具栏回调 ---------------- */

static void on_new(GtkButton* btn, gpointer data)
{
	(void)btn;
	Launcher* self = data;
	if (conn_dialog(self, NULL, GTK_WINDOW(self->window)))
		log_append(self, "已添加新连接\n");
}

static void on_edit(GtkButton* btn, gpointer data)
{
	(void)btn;
	Launcher* self = data;
	GList* sel = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(self->flow));
	Conn* c = NULL;
	if (sel)
	{
		c = g_object_get_data(G_OBJECT(sel->data), "conn");
		g_list_free(sel);
	}
	if (c && conn_dialog(self, c, GTK_WINDOW(self->window)))
		log_append(self, "已保存修改\n");
}

static void on_delete(GtkButton* btn, gpointer data)
{
	(void)btn;
	Launcher* self = data;
	GList* sel = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(self->flow));
	Conn* c = NULL;
	if (sel)
	{
		c = g_object_get_data(G_OBJECT(sel->data), "conn");
		g_list_free(sel);
	}
	delete_conn(self, c);
}

static void on_log_toggled(GtkToggleButton* tb, gpointer data)
{
	Launcher* self = data;
	gtk_revealer_set_reveal_child(GTK_REVEALER(self->log_revealer),
	                              gtk_toggle_button_get_active(tb));
}

/* ---------------- 主界面 ---------------- */

/* 将 GtkFlowBoxChild 的 CSS 节点名从 "flowboxchild" 改为 "card-cell"，
 * 使主题的 .flowboxchild:selected 整格蓝色高亮规则不再匹配，彻底去掉方形蓝块；
 * 选中态改由卡片自身的 .card-selected 类驱动（圆角 + 描边 + 光晕）。 */
static void rename_flowboxchild_css_node(void)
{
	GType t = GTK_TYPE_FLOW_BOX_CHILD;
	GtkWidgetClass* k = g_type_class_ref(t);
	gtk_widget_class_set_css_name(k, "card-cell");
	g_type_class_unref(k);
}

static void on_activate(GtkApplication* app, gpointer data)
{
	Launcher* self = data;
	rename_flowboxchild_css_node();
	load_css();

	GtkWidget* win = gtk_application_window_new(app);
	self->window = win;
	gtk_window_set_title(GTK_WINDOW(win), "remote");
	gtk_window_set_default_size(GTK_WINDOW(win), 1040, 680);

	/* ---------- 顶部标题栏 ---------- */
	GtkWidget* header = gtk_header_bar_new();
	gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
	gtk_header_bar_set_title(GTK_HEADER_BAR(header), "remote");
	gtk_window_set_titlebar(GTK_WINDOW(win), header);

	GtkWidget* b_new = btn_icon("新建", "list-add");
	GtkWidget* b_edit = btn_icon("编辑", "document-edit");
	GtkWidget* b_del = btn_icon("删除", "edit-delete");
	self->connect_btn = btn_icon("连接", "media-playback-start");
	self->disconnect_btn = btn_icon("断开", "process-stop");
	self->log_toggle = gtk_toggle_button_new_with_label("日志");
	gtk_button_set_image(GTK_BUTTON(self->log_toggle),
	                     gtk_image_new_from_icon_name("utilities-system-monitor", GTK_ICON_SIZE_BUTTON));
	gtk_button_set_always_show_image(GTK_BUTTON(self->log_toggle), TRUE);
	gtk_widget_set_sensitive(self->disconnect_btn, FALSE);

	/* 搜索：默认显示图标按钮，点击后展开为搜索框 */
	self->search = gtk_search_entry_new();
	gtk_widget_set_size_request(self->search, 240, -1);
	gtk_entry_set_placeholder_text(GTK_ENTRY(self->search), "搜索名称或主机…");

	self->search_btn = gtk_button_new_from_icon_name("system-search", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text(self->search_btn, "搜索");

	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), b_new);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), b_edit);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), b_del);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), self->disconnect_btn);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), self->connect_btn);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), self->search_btn);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), self->search);

	g_signal_connect(b_new, "clicked", G_CALLBACK(on_new), self);
	g_signal_connect(b_edit, "clicked", G_CALLBACK(on_edit), self);
	g_signal_connect(b_del, "clicked", G_CALLBACK(on_delete), self);
	g_signal_connect(self->connect_btn, "clicked", G_CALLBACK(on_connect), self);
	g_signal_connect(self->disconnect_btn, "clicked", G_CALLBACK(on_disconnect), self);
	g_signal_connect(self->log_toggle, "toggled", G_CALLBACK(on_log_toggled), self);
	g_signal_connect(self->search, "search-changed", G_CALLBACK(on_search_changed), self);
	g_signal_connect(self->search_btn, "clicked", G_CALLBACK(on_search_btn_clicked), self);
	g_signal_connect(self->search, "focus-out-event", G_CALLBACK(on_search_focus_out), self);
	g_signal_connect(self->search, "stop-search", G_CALLBACK(on_search_stop), self);

	/* 搜索框初始隐藏，仅显示图标按钮 */
	gtk_widget_set_no_show_all(self->search, TRUE);
	gtk_widget_hide(self->search);

	/* ---------- 主体 ---------- */
	GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(win), main_box);

	/* 筛选 chips：全部 / 收藏 / 分组 */
	self->filter_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(self->filter_bar), "filter-bar");
	gtk_widget_set_halign(self->filter_bar, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(main_box), self->filter_bar, FALSE, FALSE, 0);

	/* 卡片网格 */
	GtkWidget* grid_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(grid_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	self->flow = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->flow), GTK_SELECTION_SINGLE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(self->flow), TRUE);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->flow), 2);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->flow), 6);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->flow), 12);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->flow), 12);
	gtk_container_set_border_width(GTK_CONTAINER(self->flow), 16);
	/* 防止 FlowBox 在视口内被拉伸：单元格保持卡片自然高度，选中高亮不会延伸到整行底部 */
	gtk_widget_set_valign(self->flow, GTK_ALIGN_START);
	gtk_container_add(GTK_CONTAINER(grid_scroll), self->flow);

	/* 空状态 / 无匹配 提示 */
	GtkWidget* empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_valign(empty_box, GTK_ALIGN_CENTER);
	gtk_widget_set_halign(empty_box, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(empty_box),
	                   gtk_image_new_from_icon_name("computer", GTK_ICON_SIZE_DIALOG),
	                   FALSE, FALSE, 0);
	GtkWidget* empty_label = gtk_label_new("暂无连接\n点击左上角「新建」添加");
	gtk_label_set_justify(GTK_LABEL(empty_label), GTK_JUSTIFY_CENTER);
	gtk_style_context_add_class(gtk_widget_get_style_context(empty_label), "placeholder");
	gtk_box_pack_start(GTK_BOX(empty_box), empty_label, FALSE, FALSE, 0);

	GtkWidget* nomatch_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_valign(nomatch_box, GTK_ALIGN_CENTER);
	gtk_widget_set_halign(nomatch_box, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(nomatch_box),
	                   gtk_image_new_from_icon_name("edit-find", GTK_ICON_SIZE_DIALOG),
	                   FALSE, FALSE, 0);
	GtkWidget* nomatch_label = gtk_label_new("没有匹配的连接");
	gtk_style_context_add_class(gtk_widget_get_style_context(nomatch_label), "placeholder");
	gtk_box_pack_start(GTK_BOX(nomatch_box), nomatch_label, FALSE, FALSE, 0);

	self->left_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(self->left_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(self->left_stack), grid_scroll, "grid");
	gtk_stack_add_named(GTK_STACK(self->left_stack), empty_box, "empty");
	gtk_stack_add_named(GTK_STACK(self->left_stack), nomatch_box, "no-match");
	gtk_box_pack_start(GTK_BOX(main_box), self->left_stack, TRUE, TRUE, 0);

	/* 可折叠日志 */
	GtkWidget* log_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget* log_title = gtk_label_new("日志");
	gtk_widget_set_halign(log_title, GTK_ALIGN_START);
	gtk_style_context_add_class(gtk_widget_get_style_context(log_title), "section-title");
	gtk_box_pack_start(GTK_BOX(log_panel), log_title, FALSE, FALSE, 0);

	GtkWidget* log_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(log_scroll, -1, 200);
	self->log_view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(self->log_view), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(self->log_view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(self->log_view), 8);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(self->log_view), 8);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(self->log_view), 6);
	gtk_style_context_add_class(gtk_widget_get_style_context(self->log_view), "log-view");
	gtk_container_add(GTK_CONTAINER(log_scroll), self->log_view);
	gtk_box_pack_start(GTK_BOX(log_panel), log_scroll, TRUE, TRUE, 0);

	self->log_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(self->log_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_container_add(GTK_CONTAINER(self->log_revealer), log_panel);
	gtk_box_pack_start(GTK_BOX(main_box), self->log_revealer, FALSE, FALSE, 0);

	/* ---------- 底部状态栏 ---------- */
	GtkWidget* statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(statusbar), "statusbar");

	self->status = gtk_label_new("就绪");
	gtk_widget_set_halign(self->status, GTK_ALIGN_START);
	gtk_widget_set_hexpand(self->status, TRUE);
	gtk_box_pack_start(GTK_BOX(statusbar), self->status, TRUE, TRUE, 0);

	/* 日志开关移到状态栏右侧，缓解 HeaderBar 右侧按钮拥挤 */
	gtk_box_pack_end(GTK_BOX(statusbar), self->log_toggle, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(main_box), statusbar, FALSE, FALSE, 0);

	g_signal_connect(self->flow, "selected-children-changed", G_CALLBACK(on_card_selected), self);
	g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), self);

	/* 初次填充（自选激活回调需要这些字段就绪） */
	self->filter_mode = 0;
	self->filter_group = NULL;
	self->groups = NULL;
	filter_bar_rebuild(self);
	populate_grid(self);

	log_append(self, "欢迎使用远程桌面启动器\n");
	gchar* ver = g_strdup_printf("RDP 客户端：%s\nSPICE 客户端：%s\n",
	                             self->binary, self->spice_binary);
	log_append(self, ver);
	g_free(ver);

	gtk_widget_show_all(win);
}

int main(int argc, char** argv)
{
	g_set_application_name("remote");

	GtkApplication* app = gtk_application_new("org.example.xfreerdp-launcher", G_APPLICATION_DEFAULT_FLAGS);

	Launcher* self = g_new0(Launcher, 1);
	self->config = config_load();
	const gchar* env = g_getenv("XFREERDP_BIN");
	self->binary = g_strdup(env && *env ? env
	                     : (self->config->rdp_bin ? self->config->rdp_bin : "xfreerdp"));
	const gchar* rv = g_getenv("REMOTE_VIEWER_BIN");
	self->spice_binary = g_strdup(rv && *rv ? rv
	                      : (self->config->spice_bin ? self->config->spice_bin : "remote-viewer"));

	g_signal_connect(app, "activate", G_CALLBACK(on_activate), self);

	gint st = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	if (self->groups)
		g_ptr_array_unref(self->groups);
	g_free(self->binary);
	g_free(self->spice_binary);
	config_free(self->config);
	g_free(self);
	return st;
}
