# 设计约束（务必遵守）

## 1. `nativePC\plugins\` 根目录只放主 DLL
- 插件的**一切输出**（日志 / ini / 场景缓存 / 临时文件）必须写在数据目录
  `nativePC\plugins\WeaponSoundEnhance\`，不要落在 `plugins\` 根。
- 原因：`plugins\` 是公共目录（别的 mod 也在这里），堆文件既难看又容易和别人的冲突。
- 现状（已修）：
  | 文件 | 位置 |
  |---|---|
  | `WeaponSoundEnhance.log` | 数据目录（原本就是）|
  | `WeaponSoundEnhance_wem.log` | **改到**数据目录（原来在 plugins\ 根）|
  | `SonarScene.txt` | **改到**数据目录（原来在 plugins\ 根）|
  | `sonar_gui.log` | 数据目录（GUI 自己目录）|
- 读取端都做了"数据目录优先 + 旧位置兼容"，老用户不会因为文件搬家而丢功能。

## 2. 任何日志文件都必须有上限
| 日志 | 上限 | 超限行为 |
|---|---|---|
| 插件 `WeaponSoundEnhance.log` | 128 KB | 清空重写 |
| wem 日志 `WeaponSoundEnhance_wem.log` | 512 KB | 删除重写（每 5 秒检查一次）|
| GUI `sonar_gui.log` | 64 KB | 清空重写 |

- 读取端（插件 / GUI）都实现了"文件变小 = 被截断"的检测：清空后**不会重放旧事件**。
- 新增任何日志前先想清楚上限，禁止"只追加"。

## 3. 发布包与部署
- 发布包从 `nativePC\` 开始打包，解压到游戏根目录即可。
- 游戏运行时 DLL 被占用 → 发布脚本改名替换（`.oldlocked`）；**下次成功部署后要清掉 `.oldlocked`**，
  不要让它在 `plugins\` 里长期残留。