# TODO

## 功能缺口（按优先级）

- [ ] 滚动支持：UIA ScrollPattern 操作（`scrollable` 字段已可查，缺操作命令）
- [ ] 组合键输入：Ctrl+S / Ctrl+V / Alt+F4 等（VK 表已删，原实现可复用）
- [ ] 元素状态读取：CheckBox 勾选态（TogglePattern）、选中项（SelectionPattern）
- [ ] 关闭窗口：WM_CLOSE（state 目前只有 normal/maximized/minimized）
- [ ] 谓词值大小写不敏感（或提供标志）

## 已知问题

- [ ] `list --all` 慢（Firefox 15664 节点 provider 成本，默认已排除）
- [ ] 无引号谓词值不能含空格/`]`
- [ ] `--json` hwnd 为字符串 `"0x..."`（机器解析需转换，与列输出一致的取舍）
- [ ] SDK 层（sdk/）仍引用旧 CLI（win/el 域），未适配当前契约

## 维护约定

- [ ] CLI 契约以 `winctl --help` 输出为准（help.txt 已删除）
- [ ] 修改命令语法时同步更新 README.md / README.zh.md 与 usage()
