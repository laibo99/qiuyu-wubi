#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 data/networds.txt —— 网络用语 / APP 名称 / 常用词的自定义词条。

编码规则（与用户约定一致）：
  · 两字词：每个字取五笔码的**前两位**拼成 4 字母码
      微 tmg → tm，信 wy → wy  ⇒ 微信 = tmwy
      豆 gku → gk，包 qn → qn  ⇒ 豆包 = gkqn
      夸 dfn → df，克 dq → dq  ⇒ 夸克 = dfdq
  · 三/四字词：用拼音首字母（yyds、xhs、wxzf…），
    这样五笔模式与拼音简拼的结果一致，两种输入习惯都能打出来。
  · 单字：沿用码表里的二级简码。

用法：
    python tools/gen_networds.py            # 生成 data/networds.txt
    python tools/gen_networds.py --check    # 只检查撞码与缺字
"""
import os
import re
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BASE)
DATA = os.path.join(ROOT, 'data')
OUT = os.path.join(DATA, 'networds.txt')
WEIGHT = 2000000000  # 排到同码候选最前（现有单字权重多在 1e8~1.5e9 之间）

# ---- 词表：网络用语 / APP 名称 / 高频网络词 ----
WORDS = """
微信 钉钉 飞书 微博 淘宝 天猫 京东 苏宁 拼多多 抖音 快手 小红书 知乎 百度 夸克
豆包 闲鱼 得物 小米 华为 荣耀 联想 苹果 微软 特斯拉 比亚迪 蔚来 理想 小鹏 问界
元宝 文心一言 通义千问 智谱 网易 腾讯 阿里 字节 搜狐 新浪 央视 美团 饿了么 唯品会
哔哩哔哩 起点 晋江 番茄 QQ 微信支付 支付宝 云闪付 视频号 公众号 小程序 朋友圈
表情包 直播间 网盘 应用商店 二维码 邀请码 兑换码 优惠券 秒杀 拼团 砍价 助力
分享 转发 收藏 点赞 评论 关注 取关 拉黑 屏蔽 私信 群聊 撤回 引用 弹幕 鬼畜
打赏 涨粉 掉粉 限流 违规 举报 抄袭 蹭热度 带货 选品 挂车 测评 好物 开箱 种草
拔草 平替 官配 限定 联名 周边 二次元 预售 尾款 补款 现货 满减 包邮 退货 换货
运费险 客服 售后 快递 物流 订单 购物车 收藏夹 退款 投诉 维权 平台 商家 卖家
买家 外卖 跑腿 众筹 团购 裂变 私域 短视频 图文 探店 打卡 露营 骑行 滑板 滑雪
潜水 瑜伽 普拉提 增肌 减脂 代餐 轻食 加班 摸鱼 划水 内卷 摆烂 躺平 佛系 社恐
社牛 社死 破防 破大防 封神 逆天 绝了 绝绝子 蚌埠住了 泰裤辣 笑不活 鼠鼠 龟龟
显眼包 特种兵 搭子 饭搭子 健身搭子 旅游搭子 情绪价值 松弛感 氛围感 归属感 参与感
仪式感 性价比 心态 内耗 边界感 钝感力 配得感 稀缺感 安全感 幸福感 治愈 泪目 笑死
笑晕 想哭 无语 离谱 炸裂 神器 神兽 神回复 夺笋 爷青回 爷青结 回忆杀 名场面 破圈
出圈 翻红 顶流 网红 素人 避雷 踩雷 翻车 塌房 脱粉 回踩 认领 道歉 梗图 动漫 漫画
番剧 游戏 主机 端游 手游 电竞 战队 上分 排位 段位 皮肤 抽卡 保底 氪金 白嫖 肝
大招 连招 速通 通关 存档 读档 攻略 秘籍 彩蛋 剧透 追番 补番 弃番 番外 同人
太太 磕CP 尊嘟假嘟 栓Q 笑鼠 退退退
永远的神 一键三连 有一说一 不愧是你 笑死我了 我裂开了 冤种 啊这 懂得都懂 大无语
累了 麻了 笑纳 承蒙 抬举 荣幸 浅尝 细品 芜湖起飞 哦豁 咦 芜湖 妙哇 怼 撩 舔 咯
嗷呜 嗷 唔 耶 妙啊 夺笋啊 真的蚌埠住了 蚌埠住了 泰裤辣 笑不活 显摆 摆烂 躺平
"""


def load_char_codes():
    """单字 → 五笔码（取码表里该字的第一条，通常是最短/最高权重的那个）"""
    path = os.path.join(DATA, 'wubi86.dict.yaml')
    codes = {}
    started = False
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('...'):
                started = True
                continue
            if not started or not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) < 3 or len(parts[0]) != 1:
                continue
            if not re.match(r'^[a-y]{1,4}$', parts[1]):
                continue
            ch = parts[0]
            if ch not in codes:
                codes[ch] = parts[1]
    return codes


def load_pinyin():
    """单字 → 拼音音节（取首选读音）"""
    path = os.path.join(DATA, 'pinyin.txt')
    table = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            m = re.match(r'^U\+([0-9A-Fa-f]+):\s*([^#]+)#', line)
            if not m:
                continue
            cp = int(m.group(1), 16)
            if not (0x3400 <= cp <= 0x9FFF or cp == 0x3007):
                continue
            ch = chr(cp)
            if ch in table:
                continue
            r = m.group(2).split(',')[0].strip().lower()
            r = r.replace('ü', 'v').replace('ê', 'e')
            r = re.sub(r'[^a-z]', '', r)
            if r:
                table[ch] = r
    return table


# 手工指定编码：规则撞码或含特殊字时用
MANUAL_CODES = {
    '拉黑': 'lhei',   # 与“拼团”撞码（拉/拼同偏旁，黑/团同首码）
    '饿了么': 'elm',  # “么”在拼音表里没有读音
}


def code_for(word, char_codes, pinyin):
    """按约定生成编码；返回 (code, 说明) 或 (None, 原因)"""
    if word in MANUAL_CODES:
        return MANUAL_CODES[word], '手工指定'
    if len(word) == 1:
        c = char_codes.get(word)
        if not c:
            return None, '无五笔码'
        return c, '单字全码'
    if len(word) == 2:
        parts = []
        for ch in word:
            c = char_codes.get(ch)
            if not c:
                return None, f'“{ch}”无五笔码'
            parts.append(c[:2])
        return ''.join(parts), '两字各取前两位'
    initials = ''.join(pinyin.get(ch, '')[:1] for ch in word)
    if len(initials) != len(word) or not initials:
        return None, '缺拼音'
    return initials, '拼音首字母'


def main():
    words = [w for w in WORDS.split() if w]
    char_codes = load_char_codes()
    pinyin = load_pinyin()

    entries = []
    problems = []
    for w in words:
        code, why = code_for(w, char_codes, pinyin)
        if not code:
            problems.append((w, why))
            continue
        if len(code) > 4:
            problems.append((w, f'码过长 {code}'))
            continue
        entries.append((w, code, why))

    # 撞码检查：同码不同词要报出来
    seen = {}
    conflicts = []
    for w, code, why in entries:
        if code in seen and seen[code] != w:
            conflicts.append((code, seen[code], w))
        seen[code] = w

    print(f'词条 {len(entries)} 条，码冲突 {len(conflicts)} 组，问题 {len(problems)} 条')
    for code, a, b in conflicts:
        print(f'  [冲突] {code}: “{a}” 与 “{b}”')
    for w, why in problems:
        print(f'  [跳过] {w}: {why}')

    if '--check' in sys.argv:
        return 0

    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# 网络用语 / APP 名称 / 网络高频词（自定义词条）\n')
        f.write('# 格式：词<TAB>编码<TAB>权重<TAB>说明\n')
        f.write('# 编码：两字词=各字五笔码前两位；3~4 字=拼音首字母（与简拼一致）\n')
        f.write('# 由 tools/gen_networds.py 生成，可手工增删；改动后需重跑 build_dict.py\n')
        for w, code, why in entries:
            f.write(f'{w}\t{code}\t{WEIGHT}\t{why}\n')
    print(f'写出 {OUT}（{len(entries)} 条）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
