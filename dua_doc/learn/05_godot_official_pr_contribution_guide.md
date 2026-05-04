# 给 Godot 官方提 PR 的完整流程与踩坑实录

> 第一次给 godotengine/godot 提 PR 的完整流程，附 4 次 CI 失败的实录与修复方法。
> 案例：[#119142 — Add Input.get_device_orientation()](https://github.com/godotengine/godot/pull/119142)
> 适用范围：所有"添加新 API / 新功能"类型的 PR。

---

## 一、动手前必须确认的硬性规则

### 1.1 必须先有 proposal

Godot 的官方政策：**任何添加新 API / 新功能的 PR，必须先在 [godot-proposals](https://github.com/godotengine/godot-proposals/issues) 提一个 proposal issue 立项**。

- 没 proposal 直接开 PR → maintainer 30 秒内 close，留言 "Please open a proposal first"
- proposal 不需要等 100% 通过 review，但要"已经被讨论且没有强烈反对"才能开 PR
- bug fix / 重构 / 文档修订 类的 PR 不需要 proposal

**操作**：去 `godot-proposals` 仓库 → New Issue → 选 "Feature Proposal" 模板 → 填 Problem / Proposed improvement → 等几天看反响。

### 1.2 一个 PR 只能做一件事

Godot maintainer 看到"两个独立功能塞在一个 PR"会让你拆开，没商量。

❌ 我的反例：原 commit 把"传感器采样率控制"和"设备姿态 API"塞在一起 → 必须重新拉分支只保留姿态 API

### 1.3 commit message 必须英文

- 中文 commit message → maintainer 直接打回
- 标题（subject）≤ 72 字符，imperative mood（"Add xxx" 不要"Added xxx"）
- subject 和 body 之间空一行
- body 解释 **why** 和 **what**，不解释 **how**（how 看代码就知道了）

---

## 二、Fork 与分支策略

### 2.1 一人一 fork

GitHub 规定：**同一账号对同一上游仓库只能 fork 一次**。如果你想"再 fork 一次"做新 PR，做不到——必须在现有 fork 里**新建分支**。

### 2.2 三个 remote 配置

```cmd
git remote -v
```

理想配置：

```
origin    https://github.com/<你的用户名>/godot.git  (你的 fork)
upstream  https://github.com/godotengine/godot.git   (官方上游)
```

如果没有 upstream，加一下：

```cmd
git remote add upstream https://github.com/godotengine/godot.git
```

### 2.3 分支命名

Godot 不强制，但社区习惯：

- `feature/xxx` —— 新功能
- `fix/xxx` —— bug 修复
- `docs/xxx` —— 文档
- 名字简短、用连字符

### 2.4 永远基于 upstream/master 拉分支

```cmd
git fetch upstream
git checkout -b feature/your-feature upstream/master
```

**不要基于自己的 master**，因为你的 master 上可能有别的 commit 污染。

---

## 三、把代码整理成干净 PR 的标准流程

适用场景：你在自己的 master 上已经做完了功能开发，commit 历史很乱（夹杂别的功能、有中文 commit message、有非 PR 内容如 dua_doc）。

### 3.1 拉新分支

```cmd
git fetch upstream
git checkout -b feature/your-feature upstream/master
```

### 3.2 把目标 commit cherry-pick 过来

```cmd
git cherry-pick <commit-hash-1> <commit-hash-2>
```

### 3.3 手工剔除不属于本 PR 的部分

把多塞进来的功能、中文文档、README 改动等都 revert 掉。逐个文件改回上游原样：

```cmd
git show upstream/master:path/to/file > 临时文件     # 看上游版本是什么
```

然后用 edit 工具把不属于本 PR 的代码改回原样。

### 3.4 把所有改动 squash 成 1 个 commit

cherry-pick + 修剪后会有 N 个零碎 commit，用 soft reset 合并：

```cmd
git add -u
git reset --soft upstream/master    # 把所有改动收回到暂存区
git commit -m "Subject line" -m "Body paragraph 1" -m "Body paragraph 2"
```

**注意**：cmd 里 `-m` 多次拼接会让段落之间多一个空行，GitHub 上看起来会散。如果在意，用 `git commit` 不带 `-m` 进编辑器写。

### 3.5 验证 diff 干净

```cmd
git diff upstream/master --stat
```

确认：
- 文件清单只包含本 PR 真正涉及的文件
- 行数和 proposal 里预估的差不多
- 没有 dua_doc / README.md / 中文文件夹 等污染

### 3.6 推送到 fork

```cmd
git push -u origin feature/your-feature
```

GitHub 会回显 PR 创建链接：

```
https://github.com/<用户名>/godot/pull/new/feature/your-feature
```

---

## 四、PR 描述模板

```markdown
Implements godotengine/godot-proposals#XXXX

## Summary
（一两句话总结这个 PR 做了什么、解决了什么问题）

## Implementation
（按平台/模块分点说明实现细节）
- **Module A**: ...
- **Module B**: ...

## API
（列出新增/修改的 API 和项目设置）
- New method: `Foo.bar()`
- New project setting: `module/feature/xxx`

## Testing
（你怎么测的，在什么设备/平台、什么场景）
- 设备/平台
- 测试结果
- ⚠️ 没测过的平台必须显式声明，例如：
  > I do not have access to an iOS device. Help testing on iOS is appreciated.
```

**关键点**：

1. 第一行 `Implements godotengine/godot-proposals#XXXX` —— GitHub 会自动建立 PR ↔ proposal 的关联
2. **没测的平台一定要主动说**，比被 reviewer 问到强 100 倍
3. 描述里不要写"This is my first PR, please be gentle" 之类的客套，直接进入正题

---

## 五、PR 开出来后的状态解读

刚开的 PR 会显示一堆 ❌ 警告，**这是正常的**，不是你做错了什么：

| 状态 | 是不是问题 | 解释 |
|------|----------|------|
| ❌ Review required | ✅ 正常 | 所有 PR 一开都是这状态，等 maintainer approve |
| 👤 N pending reviews | ✅ 正常 | 根据 .github/CODEOWNERS 自动 ping 模块负责人 |
| ⚠️ N workflow awaiting approval | ✅ 正常 | **首次贡献者**的 PR，CI 不会自动跑，等 maintainer 手动点 "Approve and run" |
| ❌ Merging is blocked | ✅ 正常 | 上面 review 没批，自然合不了 |

**别催别 ping**，maintainer 都是义务工作，催反招烦。

---

## 六、CI 失败踩坑实录（真实案例）

### 坑 1：Java 多行 `if` 条件被合并成一行

**报错信息**：
```diff
- if (sensorType == Sensor.TYPE_GAME_ROTATION_VECTOR
-         || sensorType == Sensor.TYPE_ROTATION_VECTOR) {
+ if (sensorType == Sensor.TYPE_GAME_ROTATION_VECTOR || sensorType == Sensor.TYPE_ROTATION_VECTOR) {
```

**规则**：Godot Java 风格要求，短逻辑表达式合并成一行（不超过 ~120 字符就别换行）。

### 坑 2：文件末尾少了换行符

**报错信息**：
```diff
-}
\ No newline at end of file
+}
+
```

**规则**：所有源文件最后一行必须以 `\n` 结尾（POSIX 标准）。

**Windows 下检查方法**：
```cmd
powershell -Command "$b=[IO.File]::ReadAllBytes('your/file.java'); 'last byte: ' + $b[$b.Length-1]"
```
若输出不是 `10`（LF），就要补换行符。

**修复**：
```cmd
powershell -Command "$f='your/file.java'; $b=[IO.File]::ReadAllBytes($f); [IO.File]::WriteAllBytes($f, $b + [byte[]](0x0A))"
```

### 坑 3：用多空格做"对齐美化"

**报错信息**：
```diff
- private static final float[] CORRECTION_0   = { ... };
- private static final float[] CORRECTION_90  = { ... };
- private static final float[] CORRECTION_180 = { ... };
+ private static final float[] CORRECTION_0 = { ... };
+ private static final float[] CORRECTION_90 = { ... };
+ private static final float[] CORRECTION_180 = { ... };
```

**规则**：变量名 / `=` / 注释前**不允许多空格对齐**。原因是重命名变量时这种对齐会让 git diff 变得超大、超难审。

### 坑 4：二元运算符两侧没空格

**报错信息**：
```diff
- a[0]*b[0] - a[1]*b[1]
+ a[0] * b[0] - a[1] * b[1]
```

**规则**：所有二元运算符（`+ - * / % == != && ||` 等）两侧都必须有空格。

### 坑 5：XML 文档的 method/member 没按字母排序

**最坑的一个**。改 `doc/classes/*.xml` 后，CI 会用 `make_rst.py` 检查每个 `<method>` 和 `<member>` 是否按 name 字母升序排列。

**报错表现**：CI 给出的 diff 看起来非常诡异，像是把"set_device_orientation 的描述改成 set_gravity 的描述"——其实是**整块代码挪位置**，diff 工具没能对齐而已。

**正确顺序例子**（方法名按字母升序）：

```
set_default_cursor_shape    ← d
set_device_orientation      ← d (devic-)
set_gravity                 ← g (gra-)
set_gyroscope               ← g (gyr-)
set_joy_light               ← j
```

✅ 字母排序对的位置；错插会被 CI 立刻发现。

**本地预检（强烈推荐）**：

```cmd
python doc/tools/make_rst.py --dry-run doc/classes
```

提交前跑一下，能省掉一轮 CI 等待。

---

## 七、终极建议：本地装 pre-commit hook

Godot 的所有代码风格检查都集中在 `.pre-commit-config.yaml` 里。本地装上后，每次 `git commit` 都会自动跑一遍，**提交前就知道有没有问题，不用被 CI 反复打回**。

```cmd
pip install pre-commit
cd <godot 仓库根>
pre-commit install
```

之后正常 `git commit` 即可，pre-commit 会自动运行 clang-format、clang-tidy、black、ruff、markdownlint、file_format.sh、make_rst.py 等所有检查。

如果检查失败，hook 会自动应用建议补丁，再 `git add -u && git commit` 一次就行。

> ⚠️ **强烈建议你提第一个 PR 之前就装好这个**。我没装，结果连续吃了 3 轮 CI 失败，每轮都要等几分钟才知道下一个错在哪。

---

## 八、修复 CI 失败的标准操作

每一轮 CI 失败时：

1. **点开红 ❌ 那个 check**，看 log（一般在 GitHub Actions tab）
2. **CI 通常会直接给出 diff 建议**，照着改
3. 本地修复后：
   ```cmd
   git add -u
   git commit -m "Fix xxx"
   git push origin feature/your-feature
   ```
4. **不要 force-push**！追加新 commit 即可。force-push 会让 reviewer 已经看过的代码评论变成"outdated"，他们重新看会很烦。
5. PR 会自动重新跑 CI，等 1~3 分钟看结果。

合并时由 maintainer 手动用 "Squash and merge" 一键合并成 1 个 commit，**你这边不要主动 squash**。

---

## 九、CODEOWNERS 自动 ping 的人

PR 开出来后，根据你改的文件路径，GitHub 会自动从 `.github/CODEOWNERS` 里 ping 对应模块的负责人。常见的：

| 模块 | CODEOWNER 团队 | 个人 |
|------|--------------|------|
| Input 模块 | @godotengine/input | @Sauermann |
| Android 平台 | @godotengine/android | @m4gr3d |
| iOS / Apple Embedded | @godotengine/ios | @bruvzg |
| 文档 | @godotengine/documentation | @Calinou |
| 渲染 | @godotengine/rendering | @clayjohn 等 |
| Editor / Inspector | @godotengine/editor | 多人 |

被 ping 的人不一定立刻 review，但他们是合并这个 PR 的"key person"，记住他们的名字以后回复 review 用得上。

---

## 十、常见时间线参考

| 阶段 | 预期时长 |
|------|---------|
| PR 开 → CI 首次跑 | **首次贡献者**：数小时～1 天（等 maintainer "Approve and run"）；老贡献者：自动 |
| CI 全绿 → 第一个 reviewer 看 | 3~14 天 |
| review 通过 → 合并 | 几小时～几天（要等 maintainer 决定 milestone） |
| 复杂 PR / 改动大 | 1~3 个月 |

**保持耐心**。如果超过 1 个月没回应，可以礼貌地在 PR 里 @ 一次相关 codeowner，但只能一次。

---

## 十一、应对 review 评论的态度

reviewer 的措辞经常很直接（"This is wrong" 就是字面意思，不带情绪）。处理原则：

- **同意**：改代码 + 回复 "Done in <commit hash>"
- **不同意**：有理有据地解释（比如解释 `TYPE_GAME_ROTATION_VECTOR` 为什么比 `TYPE_ROTATION_VECTOR` 更适合游戏）
- **不确定**：直接问 "What would you suggest?" 或 "Could you elaborate?"
- **千万别玻璃心**，也千万别在 PR 评论里和 reviewer 吵架

---

## 十二、本案例的完整 commit 历史

PR #119142 最终的 commit 历史，作为参考：

```
f5ea60f  Sort device_orientation entries alphabetically in XML docs
a3a8efb  Apply pre-commit Java formatting (operator spacing, no aligned spaces)
d3c66ec  Fix Java code style and trailing newline
a497701  Add Input.get_device_orientation() returning hardware-fused quaternion
```

第 1 个是主功能 commit；后 3 个都是 CI 失败后的修复 commit。最终合并时会被 squash 成 1 个 commit 进 master。

如果当初装了 pre-commit hook，**后 3 个 commit 完全可以避免**。

---

## 总结：下次提 PR 的 checklist

提 PR 前自检：

- [ ] 已经有对应的 godot-proposals issue，且没强烈反对
- [ ] 一个 PR 只做一件事，没有夹带其他功能
- [ ] 基于最新的 `upstream/master` 拉的分支
- [ ] commit message 全英文，subject ≤ 72 字符
- [ ] 没有把中文文档 / 个人笔记 / README 中文版 推上去
- [ ] 本地装了 pre-commit hook
- [ ] XML 文档里 method/member 按字母排序
- [ ] 改过的源文件末尾都有 `\n`
- [ ] 没有用多空格做"对齐美化"
- [ ] 二元运算符两侧有空格
- [ ] PR 描述里关联了 proposal、列了 API、说明了测试情况、声明了未测的平台

照着这个 checklist 走，能省掉至少一半的 CI 失败和 review 来回。
