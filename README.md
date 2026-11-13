# 秋雨五笔输入法

基于微软 TSF（Text Services Framework）文本服务框架实现的 Windows 平台五笔输入法，支持 Win7 / Win8 / Win10 / Win11，提供 32 位与 64 位双架构安装包。

## 功能

- **多方案输入**：五笔字型 86 版、全拼/简拼拼音、五笔拼音混输三种模式在同一输入法内切换，不占用多个输入法位置
- **临时拼音**：五笔状态下按 `z` 键可临时转入拼音输入，上屏后自动恢复五笔状态
- **手动调频**：按 `Ctrl+数字` 将常用候选直接提升至首位并永久记忆；配合词频调整形成个人习惯顺序
- **回车清屏**：编码输入后按回车清空当前组合，不输出任何字符
- **中英状态同步**：通过命名共享内存维护跨进程唯一的中英状态，新开窗口默认中文、切走再切回必定为中文
- **自绘状态条**：中/英 · 五/拼/混 · 全/半 · 图标输入 · 设置五个快捷分区，支持拖动、位置记忆、半透明悬浮；提供默认圆角阴影、经典直角描边、彩框胶囊三款皮肤
- **符号面板**：点选状态条笑脸图标弹出六个分类符号面板，插入即生效，不抢焦点可连续点选
- **系统手写**：勾选手写输入可调起 Windows 触摸键盘（TabTip），识别候选由系统给出
- **增量更新**：在线检查更新时仅下载变化文件，以版本号 + 构建时间戳双重门槛防止降级

## 依赖

- MinGW-w64（msvcrt 变体，winlibs 发行版，静态链接 CRT，Win7 无需额外 DLL）
- Python 3.x（词典编译、增量清单生成等辅助脚本）

构建脚本见 [`build.sh`](build.sh)（Linux Bash 环境）与 [`build_tests.sh`](build_tests.sh)。

## 构建

```bash
cd qywb
bash build.sh
```

输出至 `dist/`，含两个安装包：
- `秋雨五笔输入法_4.5_Setup.exe` — 64 位（Win7 x64 ~ Win11）
- `秋雨五笔输入法_4.5_x86_Setup.exe` — 32 位（Win7 x86 SP1 起）

### 参数说明

| 变量 | 默认值 | 说明 |
|---|---|---|
| `GCC64` | `../tools/mingw-dl/w64gcc/bin/g++.exe` | 64 位编译器路径 |
| `GCC32` | `../tools/mingw-dl/w32/mingw32/bin/g++.exe` | 32 位编译器路径 |
| `QY_BUILD_STAMP` | 自动生成 14 位时间戳 | 当前构建时间，写入清单供客户端防降级比对 |
| `QY_BASELINE` | `C:/Program Files/QiuYuWubi` | 增量包基线目录（未安装时可指定本地旧版本目录） |

复现某次构建：

```bash
QY_BUILD_STAMP=20261108120000 bash build.sh
```

## 目录结构

```
qywb/
├── src/
│   ├── ime/          # TSF 输入法组件入口（qy_dll.cpp / qy_ime.cpp）
│   ├── core/         # 引擎核心：字典、状态条、候选窗、符号面板、配置、更新
│   └── tools/        # 调试/测试工具：设置程序(qydemo)、安装程序(qysetup)、
│                      # 增量更新程序(qyupd)、各专项测试
├── data/             # 字典源文件（networds.txt / pinyin.txt / wubi86.dict.yaml）
│                       运行时加载 qywb.dat（二进制编译产物）
├── tools/            # Python 辅助脚本
│   ├── build_dict.py       从 YAML 字典编译 qywb.dat
│   ├── gen_networds.py     整理 networds.txt 生成词表
│   ├── make_inc.py         生成分发增量包 manifest
│   ├── check_dist_text.py  核验服务器文本编码 UTF-8 BOM
│   ├── fix_exports.py      改写 32 位 DLL stdcall 导出名
│   ├── seticon.py          向 PE 注入图标与版本资源节
│   └── make_copyright_pdf.py  生成软著鉴别材料 PDF
├── build.sh          # 一键构建（9 步流水线）
├── build_tests.sh    # 编译全部测试目标
├── app.rc            # 资源描述符（Windows SDK）
└── dist/             # 构建输出（排除在 git 外）
```

## 技术要点

- 源码规模约 1.85 万行（`src/` 下 57 cpp + 11 h），C++17 标准，无异常 / 无 RTTI
- TSF 实现 `ITfTextInputProcessor` / `ITfKeyEventSink` / `ITfThreadMgrEventSink` 接口
- 码表采用自定义二进制格式 `QYWBDIC1`，内存映射加载，单文件分发
- 状态条采用分层窗口（WS_EX_LAYERED + UpdateLayeredWindow）自绘，避开 Windows Ink 手写识别依赖
- 增量更新用 CRC32 校验文件完整性，同时比对版本号和 `build=` 构建戳双重判重

## 许可

个人自用项目，保留所有权利。如需商业使用请联系作者。
