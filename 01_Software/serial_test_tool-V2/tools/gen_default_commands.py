#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 serial_test_tool 的内置默认指令库 default_commands.json。

依据 00_Doc/serial_protocol_summary.md，按"命令字粒度"预置新/旧/旧扩展全部命令
（决策 D-02）。采用原始字符串模型（D-01）：
- 新协议(hex)：send_data 不含 BCC，enable_bcc=true，由软件自动追加。
  set 类命令的回复是确定的 "功能头 命令字 通道 00"（4 字节、无 BCC，已经真实设备验证）；
  read 类命令回复含实时数据，无法精确匹配，expected_reply 留空并在 remark 注明。
- 旧/旧扩展(ascii)：enable_bcc=false。

通道型命令统一用通道 1（新协议 0x01 / 旧协议 A）作为示例，用户可复制后改其它通道。

用法：python gen_default_commands.py > ../resources/default_commands.json
"""
import json
import sys

items = []


def add(cid, proto, group, name, sfmt, sdata, bcc, rfmt, reply, desc,
        remark="", enabled=True):
    items.append({
        "command_id": cid,
        "protocol_type": proto,
        "function_group": group,
        "command_name": name,
        "send_format": sfmt,
        "send_data": sdata,
        "enable_bcc": bcc,
        "expected_reply_format": rfmt,
        "expected_reply": reply,
        "match_rule": "exact",
        "timeout_mode": "auto",
        "timeout_ms": 100,
        "description": desc,
        "enabled": enabled,
        "builtin": True,
        "remark": remark,
    })


def new_set(cid, group, cmd, ch, data2, name, desc, remark="", enabled=True):
    """新协议单通道 set：CA cmd ch dataHi dataLo，回复 CA cmd ch 00。"""
    sdata = f"CA {cmd} {ch} {data2}"
    reply = f"CA {cmd} {ch} 00"
    add(cid, "new", group, name, "hex", sdata, True, "hex", reply, desc, remark, enabled)


def new_read(cid, group, cmd, ch, name, desc, remark=""):
    """新协议单通道 read：CA cmd ch 00 00，回复含实时数据，expected 留空。"""
    sdata = f"CA {cmd} {ch} 00 00"
    rk = (remark + "；" if remark else "") + "回复含实时数据，无法精确匹配，检测时请按实际填写或关闭检测"
    add(cid, "new", group, name, "hex", sdata, True, "hex", "", desc, rk)


# ============ 新协议 · 单通道 CA · 数字 ============
new_set("NEW_DIG_SET_BRIGHTNESS_CH1", "digital", "01", "01", "00 FF", "设置1通道亮度", "设置1通道亮度为255", "亮度范围 0~255 或 0~999（受亮度等级）")
new_set("NEW_DIG_SET_SWITCH_CH1", "digital", "02", "01", "00 01", "设置1通道开关", "打开1通道", "0关 1开")
new_set("NEW_DIG_SET_CONST_MODE", "digital", "03", "00", "00 01", "设置数字常亮/常灭", "1常亮 0常灭", "通道字段无效填00")
new_set("NEW_DIG_SET_CCT_CH1", "digital", "04", "01", "00 80", "设置1通道色温", "设置1通道色温", "0~255 预留/色温")
new_set("NEW_DIG_SET_BRIGHT_LEVEL", "digital", "05", "00", "00 01", "亮度等级切换", "0=上限255 1=上限999", "通道字段无效填00")
new_read("NEW_DIG_READ_BRIGHTNESS_CH1", "digital", "61", "01", "读取1通道亮度", "读取1通道当前亮度")
new_read("NEW_DIG_READ_SWITCH_CH1", "digital", "62", "01", "读取1通道开关", "读取1通道开/关状态")
new_read("NEW_DIG_READ_CONST_MODE", "digital", "63", "00", "读取常亮/常灭", "读取常亮还是常灭")
new_read("NEW_DIG_READ_CCT_CH1", "digital", "64", "01", "读取1通道色温", "读取1通道色温")
new_read("NEW_DIG_READ_BRIGHT_LEVEL", "digital", "65", "00", "读取亮度等级", "读取亮度上限模式")

# ============ 新协议 · 单通道 CA · 频闪 ============
new_set("NEW_STR_SET_PULSE_CH1", "strobe", "21", "01", "00 64", "设置1通道脉宽", "脉宽=100", "0~999")
new_set("NEW_STR_SET_LIGHTDELAY_CH1", "strobe", "22", "01", "00 0A", "设置1通道光源延时", "光源延时=10", "0~999")
new_set("NEW_STR_SET_CAMDELAY_CH1", "strobe", "23", "01", "00 0A", "设置1通道相机延时", "相机延时=10", "0~999")
new_set("NEW_STR_SET_INNERCYCLE", "strobe", "24", "00", "00 64", "设置内触发周期", "内触发周期=100", "0~999 通道无效填00")
new_set("NEW_STR_SET_TRIGSRC", "strobe", "25", "00", "00 01", "设置内外触发", "0外触发 1内触发", "代码最大值2")
new_set("NEW_STR_SET_CAMTRIGMODE", "strobe", "26", "00", "00 00", "设置相机触发模式", "0上升沿 1下降沿", "")
new_set("NEW_STR_SET_PULSEUNIT", "strobe", "27", "00", "00 00", "设置脉宽单位", "0=us 1=0.1us", "")
new_set("NEW_STR_SET_TRIGFILTER", "strobe", "28", "00", "00 0A", "设置触发滤波", "输入触发滤波抗干扰", "0~999")
new_read("NEW_STR_READ_PULSE_CH1", "strobe", "81", "01", "读取1通道脉宽", "读取灯亮多久")
new_read("NEW_STR_READ_LIGHTDELAY_CH1", "strobe", "82", "01", "读取1通道光源延时", "读取光源延时")
new_read("NEW_STR_READ_CAMDELAY_CH1", "strobe", "83", "01", "读取1通道相机延时", "读取相机延时")
new_read("NEW_STR_READ_INNERCYCLE", "strobe", "84", "00", "读取内触发周期", "读取内部触发周期")
new_read("NEW_STR_READ_TRIGSRC", "strobe", "85", "00", "读取内外触发", "读取内/外触发模式")
new_read("NEW_STR_READ_CAMTRIGMODE", "strobe", "86", "00", "读取相机触发模式", "读取相机触发沿")
new_read("NEW_STR_READ_PULSEUNIT", "strobe", "87", "00", "读取脉宽单位", "读取脉宽单位")
new_read("NEW_STR_READ_TRIGFILTER", "strobe", "88", "00", "读取触发滤波", "读取滤波参数")

# ============ 新协议 · 单通道 CA · 公共 ============
new_set("NEW_COM_SET_MODE", "common", "41", "00", "00 01", "模式切换", "0普通 1频闪 2可编程", "")
new_set("NEW_COM_SET_BAUD", "common", "42", "00", "00 07", "波特率设置", "0=4800…7=115200", "改后需按新波特率通讯")
new_set("NEW_COM_CLEAR_RECORD", "common", "43", "00", "00 00", "清空输入输出记录", "清零触发次数统计", "")
new_set("NEW_COM_RESERVED_44", "common", "44", "00", "00 00", "预留(0x44)", "暂未使用", "预留", enabled=False)
new_set("NEW_COM_SOFT_TRIGGER", "common", "45", "00", "00 00", "软件触发", "命令触发一次", "0~8 时间值")
new_set("NEW_COM_FACTORY_RESET", "common", "46", "00", "00 00", "恢复出厂设置", "参数恢复默认", "")
new_set("NEW_COM_SAVE_FLASH", "common", "47", "00", "00 00", "数据保存Flash", "保存当前参数", "")
new_set("NEW_COM_SET_TEMP_TH_CH1", "common", "48", "01", "00 50", "设置1通道温度阈值", "温度阈值", "Excel未完整列出")
new_set("NEW_COM_EXPLORE_SLAVE", "common", "49", "00", "00 00", "主机探索从机", "模块化查找从机", "")
new_set("NEW_COM_SET_SYNC_MODE", "common", "4A", "00", "00 01", "主机同步模式", "0非同步 1同步", "")
new_read("NEW_COM_READ_MODE", "common", "A1", "00", "读取模式", "当前工作模式")
new_read("NEW_COM_READ_BAUD", "common", "A2", "00", "读取波特率", "当前串口速度档位")
new_read("NEW_COM_READ_IN_TRIG_CH1", "common", "A3", "01", "读取1通道输入触发次数", "外部输入触发次数")
new_read("NEW_COM_READ_OUT_CH1", "common", "A4", "01", "读取1通道光源输出次数", "灯输出次数")
new_read("NEW_COM_READ_CHANNEL_NUM", "common", "A5", "00", "读取通道数", "控制器几路")
new_read("NEW_COM_READ_MODEL", "common", "A6", "00", "读取型号", "Digital/Strobe/模块化")
new_read("NEW_COM_READ_CAM_TRIG_CH1", "common", "A7", "01", "读取1通道相机触发次数", "相机输出次数")
new_read("NEW_COM_READ_TEMP_TH_CH1", "common", "A8", "01", "读取1通道温度阈值", "读取温度阈值")
new_read("NEW_COM_READ_SLAVE_NUM", "common", "A9", "00", "获取从机数量", "模块化主机用")
new_read("NEW_COM_READ_SYNC_MODE", "common", "AA", "00", "获取主机同步模式", "读取同步模式")

# ============ 新协议 · 多通道 CB（示例） ============
add("NEW_MULTI_SET_BRIGHTNESS", "new", "digital", "多通道设置亮度(示例)", "hex",
    "CB 01 02 01 00 FF 03 00 0A", True, "hex", "",
    "一次设置多个通道同一参数：CB 命令字 通道数 [通道 数据2]…", "本例：通道1=255 通道3=10；多通道仅支持写，不支持读")

# ============ 新协议 · 可编程 CC ============
add("NEW_PRG_TABLE_WRITE", "new", "program", "可编程表格写(示例)", "hex",
    "CC 01 01 01 00 01 00 01 01 00 00 64 00 FF", True, "hex", "",
    "写一行可编程表格", "格式：CC 01 配方 触发源 行号2 通道数2 相机输出1 保留1 脉宽2 [亮度/脉宽 2*通道]；本例行1/通道1")


def new_prog_param(cid, cmd, data2, name, desc, remark=""):
    sdata = f"CC {cmd} 01 01 {data2}"
    reply = f"CC {cmd} 01 01 00"  # 普通参数 set 回复（与单通道一致，末字节00表示成功）
    add(cid, "new", "program", name, "hex", sdata, True, "hex", reply, desc, remark)


def new_prog_read(cid, cmd, name, desc):
    sdata = f"CC {cmd} 01 01 00 00"
    add(cid, "new", "program", name, "hex", sdata, True, "hex", "", desc,
        "回复含实时数据，无法精确匹配")


new_prog_param("NEW_PRG_SET_TOTAL_STEPS", "02", "00 0A", "设置总步数", "0~64")
new_prog_param("NEW_PRG_SET_CUR_STEP", "03", "00 01", "设置当前步数", "0~64")
new_prog_param("NEW_PRG_SET_STEP_MODE", "04", "00 01", "单步/连续触发", "0单步 1连续")
new_prog_param("NEW_PRG_SET_CONT_INTERVAL", "05", "00 64", "连续触发间隔", "0~999")
new_prog_param("NEW_PRG_SET_CAM_DELAY", "06", "00 0A", "相机延时", "0~999")
new_prog_param("NEW_PRG_SET_LIGHT_DELAY", "07", "00 0A", "光源延时", "0~999")
new_prog_param("NEW_PRG_SET_SE_SWITCH", "08", "00 01", "起始/结束开关", "0~1")
new_prog_param("NEW_PRG_SET_START_STEP", "09", "00 01", "起始步骤", "0~64")
new_prog_param("NEW_PRG_SET_END_STEP", "0A", "00 0A", "结束步骤", "0~64")
new_prog_param("NEW_PRG_SET_CAM_OUT", "0B", "00 01", "相机输出", "0~1；普通参数接口可能未真正写入")
new_prog_param("NEW_PRG_SET_HW_RESET", "0C", "00 01", "硬件复位开关", "0~1")
new_prog_param("NEW_PRG_SET_AUTO_RESET", "0D", "00 01", "自动复位开关", "0~1")
new_prog_param("NEW_PRG_SET_AUTO_RESET_TIME", "0E", "00 64", "自动复位计时", "0~999")
new_prog_read("NEW_PRG_READ_TABLE", "31", "读取可编程表格", "读取表格行数据")
new_prog_read("NEW_PRG_READ_TOTAL_STEPS", "32", "读取总步数", "")
new_prog_read("NEW_PRG_READ_CUR_STEP", "33", "读取当前步数", "")
new_prog_read("NEW_PRG_READ_STEP_MODE", "34", "读取单步/连续", "")
new_prog_read("NEW_PRG_READ_CONT_INTERVAL", "35", "读取连续触发间隔", "")
new_prog_read("NEW_PRG_READ_CAM_DELAY", "36", "读取相机延时", "")
new_prog_read("NEW_PRG_READ_LIGHT_DELAY", "37", "读取光源延时", "")
new_prog_read("NEW_PRG_READ_SE_SWITCH", "38", "读取起始/结束开关", "")
new_prog_read("NEW_PRG_READ_START_STEP", "39", "读取起始步骤", "")
new_prog_read("NEW_PRG_READ_END_STEP", "3A", "读取结束步骤", "")
new_prog_read("NEW_PRG_READ_CAM_OUT", "3B", "读取相机输出", "")
new_prog_read("NEW_PRG_READ_HW_RESET", "3C", "读取硬件复位开关", "")
new_prog_read("NEW_PRG_READ_AUTO_RESET", "3D", "读取自动复位开关", "")
new_prog_read("NEW_PRG_READ_AUTO_RESET_TIME", "3E", "读取自动复位计时", "")
new_prog_param("NEW_PRG_RESET_STEP", "51", "00 00", "复位步数", "当前步数复位为1")
new_prog_param("NEW_PRG_ERASE_TABLE", "52", "00 00", "擦除表格数据", "只清表格不写Flash")
new_prog_param("NEW_PRG_SOFT_TRIGGER", "53", "00 00", "可编程软件触发", "调用可编程软件触发")


# ============ 旧协议 · ASCII ============
def old(cid, group, name, sdata, reply, desc, remark="", enabled=True):
    add(cid, "old", group, name, "ascii", sdata, False, "ascii", reply, desc, remark, enabled)


old("OLD_DIG_SET_BRIGHTNESS_A", "digital", "设置A通道亮度", "SA0255#", "a", "A通道亮度=255", "XXX 000~999")
old("OLD_DIG_READ_BRIGHTNESS_A", "digital", "读取A通道亮度", "SA#", "", "读取A通道亮度", "回复 a0XXX，含数据无法精确匹配")
old("OLD_DIG_SET_ZONE", "digital", "设置分区亮灭", "SW0001#", "a", "设置分区亮灭状态", "占位：代码未真正存入全局参数")
old("OLD_DIG_READ_ZONE", "digital", "读取分区亮灭", "SW#", "sw0000", "读取分区亮灭", "代码固定 temp=0")
old("OLD_DIG_SET_CONST_ON", "digital", "设置数字常亮", "TH#", "h", "设置数字常亮", "")
old("OLD_DIG_SET_CONST_OFF", "digital", "设置数字常灭", "TL#", "l", "设置数字常灭", "")
old("OLD_DIG_READ_CONST", "digital", "读取常亮/常灭", "T#", "", "读取常亮还是常灭", "回复 H 或 L")
old("OLD_STR_SET_PULSE_A", "strobe", "设置A通道脉宽", "SPA0100#", "pa", "A通道脉宽=100", "XXX 最大999")
old("OLD_STR_READ_PULSE_A", "strobe", "读取A通道脉宽", "SPA#", "", "读取A通道脉宽", "回复 pa0XXX")
old("OLD_STR_SET_PULSEUNIT_A", "strobe", "设置A通道脉宽单位", "SPUA0#", "pua", "脉宽单位", "代码疑点：存入ASCII'0'/'1'")
old("OLD_STR_READ_PULSEUNIT_A", "strobe", "读取A通道脉宽单位", "SPUA#", "", "读取脉宽单位", "回复 puaX")
old("OLD_STR_SET_INNERCYCLE", "strobe", "设置内触发周期", "ST0100#", "t", "内触发周期=100", "XXX 最大999")
old("OLD_STR_READ_INNERCYCLE", "strobe", "读取内触发周期", "ST#", "", "读取内触发周期", "回复 t0XXX")
old("OLD_STR_SET_TRIGMODE", "strobe", "设置内外触发/触发模式", "TR1#", "tr", "触发模式", "TR0/TR1/TR2")
old("OLD_STR_READ_TRIGMODE", "strobe", "读取触发模式", "TR#", "", "读取触发模式", "回复 trX")
old("OLD_STR_SET_LIGHTDELAY_A", "strobe", "设置A通道光源延时", "DLA0010#", "dla", "光源延时=10", "XXX 最大999")
old("OLD_STR_READ_LIGHTDELAY_A", "strobe", "读取A通道光源延时", "DLA#", "", "读取光源延时", "回复 dla0XXX")
old("OLD_STR_SET_CAMDELAY_A", "strobe", "设置A通道相机延时", "DCA0010#", "dca", "相机延时=10", "XXX 最大999")
old("OLD_STR_READ_CAMDELAY_A", "strobe", "读取A通道相机延时", "DCA#", "", "读取相机延时", "回复 dca0XXX")
old("OLD_STR_SET_ALLCAMDELAY", "strobe", "设置全部相机延时", "DC0010#", "#", "全部相机延时", "占位：代码未真正保存")
old("OLD_STR_READ_ALLCAMDELAY", "strobe", "读取全部相机延时", "DC#", "dc0000", "读取全部相机延时", "固定值")
old("OLD_STR_SET_CAMTRIG_EDGE", "strobe", "设置相机触发边沿", "CT0#", "ct", "0上升沿 1下降沿", "")
old("OLD_STR_READ_CAMTRIG_EDGE", "strobe", "读取相机触发边沿", "CT#", "", "读取相机触发边沿", "回复 ctX")
old("OLD_COM_SOFT_TRIGGER", "common", "软件触发", "SWTRIG#", "#", "软件触发", "代码只回复，未调用新协议软触发")
old("OLD_COM_SAVE", "common", "保存参数", "SAVE#", "#", "保存参数", "是否真写Flash取决于外部回调")
old("OLD_COM_HEARTBEAT", "common", "心跳测试CST", "CST", "CST", "心跳测试", "无需#结束；已真实设备验证")

# ============ 旧扩展协议 · $/@ ============
# 功能号: 名称, 小类, 是否可用
ext_funcs = [
    ("00", "软件版本", "common", False),
    ("01", "亮度", "digital", True),
    ("02", "脉宽", "strobe", True),
    ("03", "脉宽单位", "strobe", True),
    ("04", "工作模式", "common", True),
    ("05", "通道开关", "digital", True),
    ("06", "光源延时", "strobe", True),
    ("07", "相机延时", "strobe", True),
    ("08", "数字常亮/常灭", "digital", True),
    ("09", "内触发周期", "strobe", True),
    ("10", "外部触发模式", "strobe", False),
    ("11", "相机触发模式", "strobe", True),
    ("12", "通道数量", "common", False),
    ("13", "色温", "digital", True),
]
for fn, fname, grp, usable in ext_funcs:
    # 写：$ 功能号 A 0 3位数据 #，回复统一 $
    rk_w = "" if usable else "枚举有定义但实际不可用/未映射"
    add(f"EXT_SET_{fn}_A", "old_ext", grp, f"扩展写[{fn}]{fname}(A通道)", "ascii",
        f"$ {fn} A 0123 #".replace(" ", ""), False, "ascii", "$" if usable else "",
        f"扩展写 {fname}，A通道=123", rk_w, usable)
    # 读：@ 功能号 A #，回复含数据
    add(f"EXT_READ_{fn}_A", "old_ext", grp, f"扩展读[{fn}]{fname}(A通道)", "ascii",
        f"@ {fn} A #".replace(" ", ""), False, "ascii", "",
        f"扩展读 {fname}，A通道", ("回复 @+功能号+A+4位数据+#，含数据无法精确匹配"
                                  if usable else "枚举有定义但实际不可用/未映射"), usable)

import os
out_path = os.path.join(os.path.dirname(__file__), "..", "resources", "default_commands.json")
out_path = os.path.normpath(out_path)
with open(out_path, "w", encoding="utf-8") as f:
    json.dump(items, f, ensure_ascii=False, indent=2)
    f.write("\n")
print(f"wrote {len(items)} commands -> {out_path}")
