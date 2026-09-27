# -*- coding: utf-8 -*-
"""把 NaniteCutHLODShots.py 的出图合成对照图，并算像素判据。系统 Python（PIL + numpy），不进编辑器。

    python NaniteCutHLODCompose.py [出图目录]      # 默认 <项目>/Saved/NaniteCutHLODShots

每个机位一张 compare_<机位>.png（2×2）：
    原始模型群 | 截面 HLOD
    差异热图（原始 vs HLOD） | 噪声底（原始 vs 原始重拍）
热图是 |A-B| 的通道最大值，0 → 64/255 映射黑→红→黄→白，差异 ≤4 的地方垫一层原图灰度。
噪声底那格回答"这点差异是不是本来就有"：TSR 抖动与曝光残差在同一套设置下重拍一次也会有。
线框机位另出 wire_<机位>.png；compare_overview.png 把所有机位拼成一页。

再拆两刀看差异落在哪：轮廓（覆盖掩码的边界像素）vs 内部，背光（原图亮度 < DARK_Y）vs 受光。
截面几何的误差只会出现在轮廓上；内部、尤其背光面的差异来自渲染路径（天光、AO、阴影），不是几何。
hlod2 在的话同样算"HLOD vs HLOD 重拍"，作 HLOD 那条路径自己的噪声底。

判据只在"模型群覆盖到的像素"上统计。覆盖 = 原始或 HLOD 与 empty（两者都藏）相差 > MASK_DELTA，
再用 15×15 窗口的密度滤掉稀疏点 —— 远处地板的棋盘格在 TSR 下会闪，零星像素也能超过阈值，
不滤的话裁剪框被撑到整个画面。阴影算在覆盖里：阴影形状同样是 HLOD 要复刻的东西。
  diff>16   覆盖内通道差 > 16/255 的像素占比（同样的量对"原始 vs 重拍"也算一遍，作噪声底）
  IoU       原始与 HLOD 各自覆盖的交并比（轮廓 + 阴影是否对得上）
远处的小画面按整数倍最近邻放大，像素误差按原样可见。
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

MASK_DELTA = 10
DARK_Y = 80
DENSITY_RADIUS = 7
DENSITY_MIN = 0.25
DIFF_DELTA = 16
HEAT_RANGE = 64.0
PAD = 20
PANEL_MAX_W = 1000
OVERVIEW_PANEL_H = 300


def project_dir():
    here = os.path.dirname(os.path.abspath(__file__))           # <项目>/Plugins/PCGPlugins/Scripts
    return os.path.normpath(os.path.join(here, "..", "..", ".."))


def font(size, bold=False):
    for name in (("msyhbd.ttc" if bold else "msyh.ttc"), "simhei.ttf", "consola.ttf"):
        path = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", name)
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def load(dir_, name):
    path = os.path.join(dir_, name)
    return np.asarray(Image.open(path).convert("RGB")).astype(np.int16) if os.path.exists(path) else None


def to_image(a):
    return Image.fromarray(a.astype(np.uint8))


def box_mean(mask, r):
    """mask 在 (2r+1)² 窗口里的均值（积分图，边界外按 0 算）。"""
    h, w = mask.shape
    k = 2 * r + 1
    c = np.pad(mask.astype(np.float32), ((r + 1, r), (r + 1, r))).cumsum(0).cumsum(1)
    s = c[k:k + h, k:k + w] - c[:h, k:k + w] - c[k:k + h, :w] + c[:h, :w]
    return s / float(k * k)


def coverage(img, empty):
    raw = np.abs(img - empty).max(axis=2) > MASK_DELTA
    return raw & (box_mean(raw, DENSITY_RADIUS) >= DENSITY_MIN)


def bbox(mask, w, h):
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return 0, 0, w, h
    x0, x1 = max(int(xs.min()) - PAD, 0), min(int(xs.max()) + PAD + 1, w)
    y0, y1 = max(int(ys.min()) - PAD, 0), min(int(ys.max()) + PAD + 1, h)
    return x0, y0, x1, y1


def channel_diff(a, b):
    return np.abs(a - b).max(axis=2)


def heat(base, diff):
    t = np.clip(diff.astype(np.float32) / HEAT_RANGE, 0.0, 1.0)
    rgb = np.stack([np.clip(3 * t, 0, 1), np.clip(3 * t - 1, 0, 1), np.clip(3 * t - 2, 0, 1)], axis=2) * 255.0
    gray = (base.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)) * 0.3
    out = np.where((diff > 4)[:, :, None], rgb, np.repeat(gray[:, :, None], 3, axis=2))
    return to_image(out)


def stats_in(diff, mask):
    n = max(int(mask.sum()), 1)
    return {"diff16_pct": round(100.0 * float(((diff > DIFF_DELTA) & mask).sum()) / n, 2),
            "diff48_pct": round(100.0 * float(((diff > 48) & mask).sum()) / n, 2),
            "mean_diff": round(float(diff[mask].mean()) if mask.any() else 0.0, 2)}


def pct16(diff, mask):
    return round(100.0 * float(((diff > DIFF_DELTA) & mask).sum()) / max(int(mask.sum()), 1), 2)


def regions(orig, mo, mh, union):
    """覆盖内的四个子集：轮廓 / 内部（任一掩码 3×3 邻域不全在内的算轮廓），背光 / 受光（按原图亮度）。"""
    edge = union & ((mo & (box_mean(mo, 1) < 0.999)) | (mh & (box_mean(mh, 1) < 0.999)))
    luma = orig.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)
    return {"edge": edge, "interior": union & ~edge, "dark": union & (luma < DARK_Y), "lit": union & (luma >= DARK_Y)}


def breakdown(diff, reg):
    return {k: pct16(diff, m) for k, m in reg.items()}


def panel_scale(crop_w):
    if crop_w * 3 <= PANEL_MAX_W:
        return 3
    if crop_w * 2 <= PANEL_MAX_W:
        return 2
    return 1 if crop_w <= PANEL_MAX_W else PANEL_MAX_W / float(crop_w)


def scale_img(img, k):
    size = (max(1, int(round(img.width * k))), max(1, int(round(img.height * k))))
    return img.resize(size, Image.NEAREST if k >= 1 else Image.LANCZOS)


def grid(panels, captions, title_lines, cols=2):
    f_title, f_cap = font(24, True), font(20)
    title_h = 14 + 32 * len(title_lines)
    cap_h = 32
    gap = 8
    cw = max(p.width for p in panels)
    ch = max(p.height for p in panels)
    rows = (len(panels) + cols - 1) // cols
    w = cols * cw + (cols - 1) * gap
    h = title_h + rows * (cap_h + ch) + (rows - 1) * gap
    canvas = Image.new("RGB", (w, h), (24, 24, 28))
    d = ImageDraw.Draw(canvas)
    for i, line in enumerate(title_lines):
        d.text((10, 8 + 32 * i), line, fill=(235, 235, 235), font=f_title if i == 0 else f_cap)
    for i, (p, c) in enumerate(zip(panels, captions)):
        x = (i % cols) * (cw + gap)
        y = title_h + (i // cols) * (cap_h + ch + gap)
        d.text((x + 8, y + 3), c, fill=(200, 210, 255), font=f_cap)
        canvas.paste(p, (x, y + cap_h))
    return canvas


def main():
    dir_ = sys.argv[1] if len(sys.argv) > 1 else os.path.join(project_dir(), "Saved", "NaniteCutHLODShots")
    with open(os.path.join(dir_, "shots.json"), encoding="utf-8") as f:
        shots = json.load(f)
    build = shots["build"]
    full_res = shots["full_res_source_triangles"]
    cull = build.get("cull") or {}
    final = cull["triangles_after"] if cull.get("applied") else build["triangles"]
    headline = "%d 个源 · 全精度 %s 三角 → 截面 %s → 剔除看不见的之后 %s 三角（%.1f%%）· CutError %.2f cm（%.0f m 处 %.1f 像素）· 排除 %d 个组件" % (
        shots["num_source_actors"], format(full_res, ","), format(build["triangles"], ","), format(final, ","),
        100.0 * final / max(full_res, 1), build["world_cut_error"], shots["switch_distance"] / 100.0, shots["pixel_error"], build["excluded"])
    region_names = {"house_back_room": "后屋（封闭）", "cave_middle": "隧道中段", "house_front_room": "前屋（有门窗）"}
    region_counts = build.get("regions") or {}
    parts = ["%s %d → %d" % (region_names[k], v["cull_off"], v["cull_on"]) for k, v in region_counts.items() if k in region_names]
    # 逐物体的包围盒按组加总：外面绝对看不见的两组应当是 → 0，看得见的两组不该是 0。
    for prefix, label in (("item:CaveDeep", "隧道深处 3 物体"), ("item:HouseBack", "后屋 3 物体"),
                          ("item:CaveMouth", "洞口 2 物体"), ("item:HouseFront", "前屋 2 物体")):
        items = [v for k, v in region_counts.items() if k.startswith(prefix)]
        if items:
            parts.append("%s %d → %d" % (label, sum(v["cull_off"] for v in items), sum(v["cull_on"] for v in items)))
    region_line = ("盒子里剩的 HLOD 三角（剔除关 → 开）：" + " · ".join(parts)) if parts else ""
    print(headline)
    if region_line:
        print(region_line)

    rows, report = [], []
    for a in shots["angles"]:
        name = a["name"]
        orig, hlod, empty, orig2, hlod2 = (load(dir_, "%s_%s.png" % (name, v)) for v in ("orig", "baked", "empty", "orig2", "baked2"))
        if orig is None or hlod is None or empty is None:
            print("缺图：", name)
            continue
        h, w = orig.shape[:2]
        mo, mh = coverage(orig, empty), coverage(hlod, empty)
        union = mo | mh
        x0, y0, x1, y1 = bbox(union, w, h)
        d_hlod = channel_diff(orig, hlod)
        s_hlod = stats_in(d_hlod, union)
        reg = regions(orig, mo, mh, union)
        stats = {"name": name, "coverage_px": int(union.sum()), "iou": round(float((mo & mh).sum()) / max(int(union.sum()), 1), 4),
                 "hlod": s_hlod, "hlod_regions": breakdown(d_hlod, reg),
                 "region_px": {k: int(m.sum()) for k, m in reg.items()}, "crop": [x0, y0, x1, y1]}
        panels = [to_image(orig), to_image(hlod), heat(orig, d_hlod)]
        captions = ["原始模型群（Nanite，源材质）", "烘焙 HLOD（Nanite SM + 2048 贴图）", "差异：原始 vs 烘焙 HLOD"]
        if orig2 is not None:
            d_noise = channel_diff(orig, orig2)
            stats["noise"] = stats_in(d_noise, union)
            stats["noise_regions"] = breakdown(d_noise, reg)
            panels.append(heat(orig, d_noise))
            captions.append("噪声底：原始 vs 原始重拍")
        if hlod2 is not None:
            d_hnoise = channel_diff(hlod, hlod2)
            stats["hlod_noise"] = stats_in(d_hnoise, union)
            stats["hlod_noise_regions"] = breakdown(d_hnoise, reg)
        report.append(stats)

        k = panel_scale(x1 - x0)
        crops = [p.crop((x0, y0, x1, y1)) for p in panels]
        noise_txt = ("   噪声底 >16：%.2f%%  均差 %.2f" % (stats["noise"]["diff16_pct"], stats["noise"]["mean_diff"])) if "noise" in stats else ""
        r = stats["hlod_regions"]
        region_txt = "差异>16 按区域：轮廓 %.1f%% · 内部 %.1f%% · 背光 %.1f%% · 受光 %.1f%%" % (r["edge"], r["interior"], r["dark"], r["lit"])
        if "noise_regions" in stats:
            n = stats["noise_regions"]
            region_txt += "   （原始重拍：轮廓 %.1f%% · 背光 %.1f%% · 受光 %.1f%%" % (n["edge"], n["dark"], n["lit"])
            if "hlod_noise_regions" in stats:
                hn = stats["hlod_noise_regions"]
                region_txt += "；HLOD 重拍：轮廓 %.1f%% · 背光 %.1f%% · 受光 %.1f%%" % (hn["edge"], hn["dark"], hn["lit"])
            region_txt += "）"
        title = ["%s  —  距模型群中心 %.0f m，最近物体 %.0f m，FOV %.0f°，截面误差 ≈ %.2f 像素（最近处）" % (
                     name, a["distance"] / 100.0, a["nearest_cm"] / 100.0, a["fov"], a["cut_error_px_at_nearest"]),
                 "HLOD 差异 >16/255：%.2f%%  >48/255：%.2f%%  均差 %.2f   覆盖 IoU %.3f%s   （裁剪 %dx%d，显示 %s）" % (
                     s_hlod["diff16_pct"], s_hlod["diff48_pct"], s_hlod["mean_diff"], stats["iou"], noise_txt,
                     x1 - x0, y1 - y0, ("%dx" % k) if k >= 1 else ("%.2fx" % k)),
                 region_txt]
        grid([scale_img(c, k) for c in crops], captions, title).save(os.path.join(dir_, "compare_%s.png" % name))
        rows.append((name, crops[:3], stats, a))

        if a.get("wire"):
            wo, wh = load(dir_, name + "_wire_nocull.png"), load(dir_, name + "_wire_cull.png")
            if wo is not None and wh is not None:
                wk = panel_scale(x1 - x0)
                wpanels = [scale_img(to_image(img).crop((x0, y0, x1, y1)), wk) for img in (wo, wh)]
                grid(wpanels, ["HLOD 剔除关（同一份截面）", "HLOD 剔除开：外面看不见的三角已删"],
                     ["%s  X 光线框（线框是透视的，被挡住的边也画）—— 距目标 %.0f m" % (name, a["distance"] / 100.0)]
                     + ([region_line] if region_line and name in ("house", "cave") else [])).save(
                    os.path.join(dir_, "wire_%s.png" % name))

    # 总览：每个机位一行（原始 | HLOD | 差异），统一行高
    ov_rows = []
    for name, crops, stats, a in rows:
        k = OVERVIEW_PANEL_H / float(crops[0].height)
        noise = (" / 噪声底 %.2f%%" % stats["noise"]["diff16_pct"]) if "noise" in stats else ""
        ov_rows.append(grid([scale_img(c, k) for c in crops], ["原始", "HLOD", "差异"],
                            ["%s  %.0f m  误差≈%.2f px  差异>16 %.2f%%%s  IoU %.3f" % (
                                name, a["distance"] / 100.0, a["cut_error_px_at_nearest"], stats["hlod"]["diff16_pct"], noise, stats["iou"])],
                            cols=3))
    if ov_rows:
        head_h = 92 if region_line else 56
        w = max(r.width for r in ov_rows)
        h = head_h + sum(r.height for r in ov_rows) + 10 * len(ov_rows)
        ov = Image.new("RGB", (w, h), (16, 16, 20))
        ImageDraw.Draw(ov).text((10, 12), headline, fill=(255, 230, 160), font=font(22, True))
        if region_line:
            ImageDraw.Draw(ov).text((10, 50), region_line, fill=(160, 230, 255), font=font(20))
        y = head_h
        for r in ov_rows:
            ov.paste(r, (0, y))
            y += r.height + 10
        ov.save(os.path.join(dir_, "compare_overview.png"))

    with open(os.path.join(dir_, "compare_stats.json"), "w", encoding="utf-8") as f:
        json.dump({"headline": headline, "regions": region_line, "angles": report}, f, ensure_ascii=False, indent=2)
    for s in report:
        print(json.dumps(s, ensure_ascii=False))


if __name__ == "__main__":
    main()
