# xfreerdp-launcher 实现文档

一个基于 GTK3 的轻量级远程桌面启动器，用卡片式界面管理 RDP 与 SPICE 连接，
分别调用 `xfreerdp` 和 `remote-viewer` 建立会话。

## 1. 项目定位

参照微软 Windows「远程桌面」Connection Center 的卡片式布局，提供：

- 卡片网格主页：展示连接名称、主机:端口、协议（RDP / SPICE）、分组、收藏状态
- 新建 / 编辑 / 删除连接
- 收藏、分组筛选、搜索
- 可折叠的运行日志（带密码脱敏）
- 单击选中、再次点击连接的交互

## 2. 技术栈与依赖

| 类别 | 内容 |
| --- | --- |
| 语言 | C（`-std=gnu11`） |
| GUI 框架 | GTK3（`gtk+-3.0`，实测 3.24.38） |
| 基础库 | GLib / GIO / GObject（随 GTK3 引入） |
| 配置持久化 | GKeyFile（INI 格式） |
| 密码加密 | OpenSSL（AES-256-GCM 认证加密） |
| 进程管理 | `g_spawn_async_with_pipes` + GIOChannel 监听子进程输出 |
| 构建 | GNU Make + `pkg-config` |
| 外部客户端 | `xfreerdp`（RDP，2.11.x）、`remote-viewer`（SPICE） |

### 编译依赖（Debian/Ubuntu）

```bash
sudo apt install build-essential pkg-config libgtk-3-dev libssl-dev freerdp2-x11 virt-viewer
```

- `virt-viewer` 提供 `remote-viewer` 命令；`freerdp2-x11` 提供 `xfreerdp` 命令
- `libssl-dev` 提供密码加密所需的 OpenSSL 开发库
- 编译运行时需 GTK3 与 OpenSSL 开发头文件

## 3. 构建与运行

```bash
cd xfreerdp-launcher
make            # 编译生成 ./xfreerdp-launcher
make install    # 安装到 /usr/local/bin/xfreerdp-launcher
make clean      # 清理产物

./xfreerdp-launcher
```

### 环境变量覆盖客户端路径

| 变量 | 默认值 | 作用 |
| --- | --- | --- |
| `XFREERDP_BIN` | `xfreerdp` | RDP 客户端可执行文件 |
| `REMOTE_VIEWER_BIN` | `remote-viewer` | SPICE 客户端可执行文件 |

## 4. 目录结构

```
xfreerdp-launcher/
├── Makefile             # 构建脚本
├── .gitignore
└── src/
    ├── launcher.h       # 数据结构与函数声明
    ├── config.c         # 配置读写、连接模型、命令行构造
    └── main.c           # GTK 界面、交互逻辑、进程管理、样式
```

## 5. 核心数据结构

连接协议枚举与连接模型定义于 `src/launcher.h`：

```c
typedef enum
{
    PROTO_RDP,   /* 远程桌面，用 xfreerdp */
    PROTO_SPICE  /* SPICE，用 remote-viewer */
} ConnProtocol;

typedef struct Conn
{
    gchar* name;       /* 显示名 */
    gchar* host;       /* 主机/IP */
    gint   port;       /* RDP 默认 3389，SPICE 默认 5900 */
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
```

`Config` 使用 `GPtrArray` 保存 `Conn*` 列表，析构时逐个 `conn_free`。

## 6. 配置持久化

- 配置文件路径：`~/.config/xfreerdp-launcher/config.ini`
- 目录以 `0700` 权限创建
- 使用 GKeyFile 读写（INI 格式）

### Section 键设计

为避免同名连接互相覆盖，section 键采用 `conn:host|port|protocol` 形式：

```ini
[conn:10.52.17.60|3389|rdp]
name=数据库服务器
host=10.52.17.60
port=3389
protocol=rdp
username=admin
password=enc:v1:AbCdEfGh...   # AES-256-GCM 密文（base64）
width=0
height=0
fullscreen=true
gfx=false
clipboard=false
favorite=true
group=生产
extra=
```

说明：

- `password` 字段存储 AES-256-GCM 密文，前缀 `enc:v1:` + base64(iv|tag|ciphertext)
- 加密密钥独立存放于 `~/.config/xfreerdp-launcher/secret.key`（权限 0600）
- 无 `enc:v1:` 前缀时视为历史明文，加载时解密后自动在下次保存时转加密
- `name` 作为独立字段存储（早期版本的 section 键为 `conn:名称`，加载时通过
  是否含 `name` 字段做向后兼容）

## 7. 命令行构造（`conn_build_argv`）

### RDP（xfreerdp）

```
xfreerdp /v:host:port[/u:user] [/p:pass] [/f | /size:WxH] [/gfx] [+clipboard]
         /dynamic-resolution /log-level:INFO /cert-ignore [extra...]
```

要点：

- `/cert-ignore` 忽略证书校验，避免证书错误拦截连接；不使用 `-v`（那是版本输出，
  不是连接参数）
- 未配密码时**不**传 `/p:`，改用 stdin 重定向方案（见下文）
- `extra` 用 `g_shell_parse_argv` 解析为独立参数

### SPICE（remote-viewer）

```
remote-viewer spice://host:port[?password=xxx] [-f] [extra...]
```

要点：

- 无用户名概念；密码可为空（为空则不拼接 `?password=`
- `-f` 表示全屏

## 8. 关键设计决策

### 8.1 单实例应用

使用 `GtkApplication` 且应用 ID 固定为 `org.example.xfreerdp-launcher`。
这意味着旧进程未退出时，再次启动只会激活旧实例。**这是开发调试时
「改动没生效」的常见根因**，测试前需确保旧进程已完全退出。

### 8.2 无密码不卡死（stdin 重定向）

`xfreerdp` 未配置密码时会从 stdin 交互式提示输入，导致阻塞。
解决方案：在子进程内通过 `child_setup_stdin_devnull` 回调，把 stdin 显式
`dup2` 到 `/dev/null`，使 xfreerdp 读到 EOF 后快速失败退出，而非挂起。

### 8.3 空凭据补录弹窗

RDP 连接缺少密码（或用户名）时，先弹「输入凭据」对话框补录，**不写回配置**；
取消则中止。SPICE 密码可为空，因此不弹窗。

### 8.4 日志密码脱敏

`mask_cmdline` 在写日志前把命令行中的 `/p:密码` 与 `password=密码` 掩码为
`***`，避免明文密码进日志。

### 8.5 单击选中、再次点击连接

左键单击选中卡片，再次点击已选中的卡片才建立连接；通过
`on_card_button_press` 判断 `child` 是否已在选中集合中实现。

### 8.6 卡片选中态样式

通过 `rename_flowboxchild_css_node` 将 `GtkFlowBoxChild` 的 CSS 节点名改为
`card-cell`，使主题默认的 `.flowboxchild:selected` 方形蓝块不再匹配，改由
卡片自身的 `.conn-card.card-selected` 呈现圆角描边高亮。

### 8.7 日志大小限制

`log_append` 中当缓冲字符数超过 `LOG_MAX_CHARS`（200KB）时，从头裁剪最早内容，
防止长时间运行内存无限增长。

### 8.8 密码加密存储

密码字段不再明文落盘，采用 OpenSSL 的 **AES-256-GCM** 认证加密：

- 加密结果形如 `enc:v1:<base64(iv|tag|ciphertext)>`
- 密钥为 32 字节随机数，存放于 `~/.config/xfreerdp-launcher/secret.key`（权限 0600）
- 每次加密随机生成 12 字节 IV、密文末尾带 16 字节 GCM 认证 tag，防篡改
- 空密码仍存空串
- 向后兼容：无 `enc:v1:` 前缀的历史明文直接读取，下次保存自动转加密
- GCM 认证解密失败（密钥丢失或密文被篡改）时视为无密码，避免返回错误数据

## 9. 界面结构（`on_activate`）

```
GtkApplicationWindow (title: remote, 1040x680)
└── GtkHeaderBar
    ├── 左侧: 新建 | 编辑 | 删除
    └── 右侧: 搜索 | 日志 | 连接 | 断开
└── GtkBox (vertical)
    ├── 筛选 chips (全部 / 收藏 / 分组)
    ├── GtkStack
    │   ├── grid      (GtkScrolledWindow → GtkFlowBox 卡片网格)
    │   ├── empty     (空状态提示)
    │   └── no-match  (无匹配提示)
    ├── GtkRevealer (可折叠日志)
    └── 底部状态栏
```

## 10. 样式（CSS_DATA）

通过 `GtkCssProvider` 以应用优先级加载。主要类：

- `.conn-card` / `.conn-card:hover` / `.conn-card.card-selected`：卡片本体与选中态
- `.conn-name` / `.conn-sub`：名称与主机行
- `.proto-pill` + `.proto-rdp` / `.proto-spice`：协议徽标（RDP 蓝色、SPICE 橙色）
- `.group-pill`：分组胶囊
- `.star` / `.star-empty`：收藏星标
- `.chip` / `.chip:hover` / `.chip:checked`：筛选 chip
- `.statusbar` / `.log-view` / `.conn-dialog` / `.placeholder`

协议徽标颜色、分组胶囊等均为胶囊化处理，卡片带 `cursor: pointer` 提示可点击。

## 11. 进程与状态管理

`Launcher` 结构体保存：

- 子进程 `GPid`、运行标志、stdout/stderr 的 `GIOChannel` 与 watch 句柄
- 连接状态通过 `child_watch` 回调 `on_child_exit` 感知退出
- 退出时用 `WIFEXITED/WEXITSTATUS` 区分「正常退出」与「异常结束」
- 断开按钮向子进程发送 `SIGTERM`

## 12. 已实现的功能一览

- 新建/编辑/删除连接（主机:端口合并解析，默认端口按协议区分）
- RDP / SPICE 协议选择与命令行构造
- 收藏、分组筛选、搜索（匹配名称/主机/协议/用户名/分组）
- 单击选中、再次点击连接
- 空凭据补录弹窗（仅 RDP）
- 日志脱敏、日志大小限制、连接状态栏反馈
- 配置持久化与旧格式向后兼容
- 重名连接不覆盖（host|port|protocol 唯一键）

## 13. 已知可优化点

- 日志脱敏在密码含空格时存在边界情况；SPICE 密码未做 URL 编码
- `parse_host_port` 暂不支持 IPv6
- 连接运行中仍可编辑/删除当前连接
- 分组与卡片按保存顺序排列，未排序