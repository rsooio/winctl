# winctl

命令行 Windows 应用自动化工具：定位窗口与界面元素、读取信息、点击与输入。

独立发布的 C 项目：单个 `winctl.exe`，无运行时依赖，部署即用。

## 构建

依赖 MinGW-w64 交叉编译器：

```sh
make
```

产物 `winctl.exe`。发布物为 `winctl.exe` 与本文档（含 LICENSE）。

## 用法

```
winctl <命令> [参数...]
winctl --help
```

### 命令

| 命令 | 说明 |
|---|---|
| `list [--all]` | 元素查询（集合语义）：无 locator = 顶层窗口（默认 `/Window`，快）；全量输出用 `//*`；locator 以 `/` 开头 = 全局查询（虚拟根）；hwnd 前缀 = 单窗口，无 xpath = 整树。默认排除大树进程（firefox.exe 等），`--all` 包含 |
| `prop <locator> [key] [value]` | 读写属性：无 value 参读、有 value 参写；key 缺省读全部（kv 行式） |
| `click <locator> [--mouse] [--button B] [--action A]` | 点击元素（取第一个匹配） |
| `focus <locator>` | 聚焦元素（取第一个匹配） |

`prop`/`click`/`focus` 的 locator 取第一个匹配；`list` 是唯一集合语义命令。

### locator

```
<hwnd>            根元素（16 进制，0x 前缀可选）；可为任意窗口/元素句柄，
                  list 输出列的 hwnd 可直接作为根
<hwnd>/<xpath>    定位其下元素，xpath 相对该根；首段必须匹配根元素类型，
                  深层元素用完整路径（含根类型段）或以元素 hwnd 为根
/<xpath>          list 全局查询：xpath 从虚拟根求值（所有顶层窗口），
                  首段 /Window 匹配顶层窗口，//Window 匹配任意深度
```

xpath 语法：`/` `//` `*` `[n]` 位置谓词（树内）、`[@Name='']` `[@Type='']` `[@Id='']` `[@Class='']` `[@Pid='']` 属性谓词（`@` 可选；`@Pid` 仅顶层窗口段）、`*=` 包含 `^=` 前缀 `$=` 后缀、`and`、`!=`。谓词值可带引号或裸值。

### 属性

| key | 读写 | 说明 |
|---|---|---|
| `value` | 可读可写 | ValuePattern 读写，失败回退聚焦 + 剪贴板输入 |
| `state` | 可读可写 | 窗口状态，`normal`/`maximized`/`minimized` |
| `pid` | 只读 | 元素窗口句柄所属进程 ID（十进制） |

### 示例

```sh
winctl list                                  # 顶层窗口列表（默认 /Window）
winctl list //*                             # 全量（所有可见窗口的元素树）
winctl list /Window[@Name*='记事本']         # 按标题查顶层窗口（快）
winctl list /Window[@Pid='1234']            # 按进程查顶层窗口（弹窗场景）
winctl list /Window[@Name*='记事本']/Pane//Button    # 顶层窗口内查询
winctl list 0x1a2b                           # 单窗口整树
winctl list "0x1a2b//*[@Name^='打开']"       # 单窗口内按名称前缀过滤
winctl prop 0x1a2b value                     # 读值
winctl prop 0x1a2b value 文本                # 写值
winctl prop 0x1a2b state maximized           # 最大化窗口
winctl prop 0x1a2b pid                       # 读进程 ID
winctl click 0x1a2b/Window/Pane/Button       # 点击
```

顶层窗口定位仅支持属性谓词（`[@Name]`/`[@Pid]`/`[@Class]`），不支持位置谓词 `[n]`（顶层窗口序号依 Z 序变化，不稳定）；树内 `[n]` 为兄弟序，结构稳定可用。

### 输出与退出码

- 默认人类可读：`list` 对齐列（元素行 `hwnd xpath name`；终端下 name 超宽省略号、xpath 永远完整）、`prop` kv 行式；`--json` 输出完整 JSON（元素字段 `hwnd/enabled/invokable/scrollable/name/value/rect/type/xpath`）
- hwnd 全链路 16 进制（`0x` 前缀），pid 十进制
- 错误走 stderr + 非零退出码：0 成功（含查询空集）、1 参数/locator 无效、2 窗口句柄无效、3 元素未找到、4 操作失败

### 一次进程一个操作

每次调用独立进程、无状态：定位、操作、退出。串行天然安全；重试/轮询由调用方脚本循环实现（例如等待弹窗出现时反复执行 `winctl list /Window[@Pid='...']`）。

## 契约

CLI 契约以 `winctl --help` 输出为准；修改命令语法时同步更新本文件。

## 许可证

GNU General Public License v3.0，见 LICENSE。
