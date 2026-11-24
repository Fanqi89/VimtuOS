# HANDOVER —— v0.11.7 封存单（**已发版**）

- 日期：2026-11-24（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.7"`
- 封存对象：commit `26516540`（= tag `v0.11.7` = Release `456771997`）
- 基线（上一个已发布版本）：v0.11.5（tag `v0.11.5` → commit `2044d59d`，Release `456278688`）
- 中间被跳过的一版：v0.11.6（`65626737`，用户当时明确"暂缓发版"，其改动**已包含在 v0.11.7 里**）

## 1. 发布结果（本单是回读事实，不是计划）

| 项 | 值 |
|---|---|
| tag 对象 | `d740f4d0beb14698308c2dc3042c475add6c7c30`（annotated，指向 `26516540`） |
| Release id | `456771997`（`draft=false` / `prerelease=false`） |
| 远端 main | `26516540e8550c0e74133909a658cd2284045b6e`（= 本地 HEAD） |
| 附件 `vimtu64-64.iso` | 93,458,432 B — `sha256:14498c264922ae4c59256c3d468cf0ff982d5694f91cfed5a5916298a75009ca` |
| 附件 `vimtu64-64.img` | 42,684,416 B — `sha256:55cb40373bbf086a18fc77c291fa0a7593619a5a58fdd6eafedddb991c6ff948` |

## 2. 发布门禁（逐条实跑，全 PASS）

```
  [PASS] 远端 main == 本地 HEAD
  [PASS] 远端 tag 指向同一个 tag 对象  d740f4d0beb1 vs d740f4d0beb1
  [PASS] 附件 vimtu64-64.img size+digest  42,684,416 B
  [PASS] 附件 vimtu64-64.iso size+digest  93,458,432 B
  [PASS] 旧版 v0.11.5 vimtu64-64.img 一字未动  id=785570507 size=42661888 updated=2026-11-24T03:42:45Z
  [PASS] 旧版 v0.11.5 vimtu64-64.iso 一字未动  id=785569603 size=93435904 updated=2026-11-24T03:42:36Z
===== RESULT: PASS =====
```

- 发布前硬门禁：`tests/version64_test.py` **8/8 PASS**（内核内嵌 `VimtuOS 0.11.7`；上一已发布版本
  `v0.11.5` 的内嵌串在内核里 **0 次**；串口自报 `VimtuOS 0.11.7 x86_64`）。
- 旧 Release 回读口径：`id / size / digest / updated_at` 四项逐字节比对（快照存
  `$PI_SCRATCH_DIR\v0115_assets.json`），**旧 Release 一个字都没动**。
- 附件双向核对：本地 `sha256` == 远端 `asset.digest`，`asset.size` == 本地字节数。
- 发布脚本：`$PI_SCRATCH_DIR\rel_v0117.py`（沿用 v0.11.5 同一套：快照 → tag → Release → 上传 → 双向核对）。

## 3. 本版内容（详见 `HANDOVER_v0117_deskfolder.md`）

1. **桌面 = 真实文件夹**：真源 = 列 `<home>/Desktop`；拖到桌面 = `fs64_rename_to64` 真移动；
   删除 = 真进 `/Recycle`；`vfs64_mut_gen64()` 写类世代号驱动实时刷新（帧内零 I/O）。
2. **回收站合并成一套数据**：桌面回收站窗口的行 = 虚拟项 + `/Recycle` 真实条目。
3. **图标按扩展名分 5 档**，档位进串口 `glyph=`。
4. **MicroPython 贪吃蛇真跑** + 修掉方向表 `(dy,dx)` 写反的真 bug。

## 4. 验收现场（发版时的那一次构建）

- `kernel64_os.bin` **3,518,416 B**（闸门 15,728,640 B）；构建 `EXIT=0`
- `deskfolder64_test` **64/74**（两块能力 UNVERIFIED，见 §5）、`dragdrop64_test` **51/51**、
  `desktopops64_test` **72/72**、`version64_test` **8/8**

## 5. 本版**未**验证 / 已知边界（发版不等于全绿）

1. "桌面图标拖进回收站图标"这个**手势**没有被自动化证明（QEMU 相对鼠标在 1280×800 下点不中 48 px 图标）；
   底层 `trash64_put64` 链路在 v0.11.6 已端到端验过。
2. 窗口圆角**像素**判据（差分截图两帧几乎相同，判据不成立）；几何判据 `deco64_test` 绿。
3. `fileops64_test` / `micropython64_test` 本轮未复跑（改动未触及那两条路径）。

## 6. 下一轮（已开工）

**ring3 真隔离**：把"用户态进程与内核共享页表"这条现状补成真正的地址空间隔离
（内核不映射进用户 CR3 / 用户不可读写内核页 / ring3 服务崩溃可回收）。详见下一份交接单。

## 7. 不要重做

- 不要重跑 `rel_v0117.py` 去"修"这次发布：Release 已存在时脚本只会走"已存在"分支，附件不会重传；
  真要改内容必须发新版本号（**tag 只增不改**）。
- v0.11.6 不需要补发 tag/Release（其改动已在 v0.11.7 内）。
