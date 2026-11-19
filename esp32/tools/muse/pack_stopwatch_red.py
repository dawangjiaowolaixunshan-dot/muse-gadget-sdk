#!/usr/bin/env python3
"""Pack approved 466-square review videos into the StopWatch animation asset.

Uses OpenCV, NumPy and Pillow. The input directory is 正面红裙_v2 from
the avatar project. The output is a zlib block file and its C index.
"""
import argparse
from pathlib import Path
import zlib

import cv2
import numpy as np
from PIL import Image


W, H, GROUP = 120, 138, 12
LEFT, TOP, RIGHT, BOTTOM = 133, 85, 371, 357
CLIPS = {
    "WAKE_EYES": "01-垂眸抬眼.mp4",
    "WAKE_NOD": "唤醒-点头招呼.mp4",
    "IDLE_HAIR": "02-正面对视拢发.mp4",
    "IDLE_SHIFT": "待机-轻转肩换重心.mp4",
    "LISTEN_EAR": "倾听-侧耳靠近.mp4",
    "LISTEN_INTERRUPT": "03-黄键打断转倾听.mp4",
    "THINK_CHIN": "思考-托腮斟酌.mp4",
    "THINK_IDEA": "思考-低头再抬眼.mp4",
    "ANSWER_PALM": "回答-翻掌解释.mp4",
    "ANSWER_POINT": "回答-食指轻点重点.mp4",
    "TOUCH_SMILE": "轻触-挑眉侧笑.mp4",
    "TOUCH_KISS": "轻触-指尖飞吻.mp4",
    "BYE_SLOW": "告别-正面轻挥手.mp4",
    "BYE_QUICK": "告别-正面挥手再对视.mp4",
}
X, Y = np.meshgrid(np.arange(466), np.arange(466))
COLORS = 96


def mouth(rgb):
    patch = rgb[175:206, 216:254].astype(np.int16)
    red = patch[:, :, 0] - patch[:, :, 1]
    ys, xs = np.where((red > 62) & (patch[:, :, 0] > 69))
    if len(xs) < 18:
        raise ValueError("could not locate red lips")
    return (216 + np.median(xs), 175 + np.median(ys))


def clean_frame(rgb, neutral, moving_mouth, center):
    if moving_mouth:
        x, y = mouth(rgb)
        u, v = center
        dx, dy = x - u, y - v
        close = cv2.warpAffine(neutral, np.float32([[1, 0, dx], [0, 1, dy]]), (466, 466))
        distance = np.hypot((X - x) / 16, (Y - y) / 10)
        mask = np.clip((1.0 - distance) * 3.5, 0, 1)[..., None]
        rgb = np.uint8(rgb + mask * (close.astype(float) - rgb))
        xy = round((x - LEFT) * W / (RIGHT - LEFT)), round((y - TOP) * H / (BOTTOM - TOP))
    else:
        xy = (0, 0)
    avatar = rgb[TOP:BOTTOM, LEFT:RIGHT].copy()
    # Near the top the far-right sliver contains the speaker icon and the
    # "END CALL" hint. Their positions are supplied by LVGL at runtime.
    avatar[:88, 226:] = 0
    avatar[avatar.max(axis=2) < 5] = 0
    return cv2.resize(avatar, (W, H), interpolation=cv2.INTER_AREA), xy


def clips(source, neutral):
    center = mouth(neutral)
    for key, name in CLIPS.items():
        cap = cv2.VideoCapture(str(source / name))
        count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        if cap.get(cv2.CAP_PROP_FPS) != 24 or count < 80:
            raise ValueError(f"Invalid clip {name}")
        first = 34 if key == "LISTEN_INTERRUPT" else 0
        result = []
        for i in range(count):
            ok, bgr = cap.read()
            if not ok or bgr.shape != (466, 466, 3):
                raise ValueError(f"Invalid frame {i} in {name}")
            if i >= first and (i - first) % 2 == 0:
                result.append(clean_frame(bgr[:, :, ::-1], neutral, key.startswith("ANSWER_"), center))
        cap.release()
        yield key, result


def c_array(values, wrap=12):
    return "\n".join("    " + ", ".join(str(v) for v in values[i:i + wrap]) + ","
                     for i in range(0, len(values), wrap))


def pack(source, out):
    neutral = np.asarray(Image.open(source / "正面红丝绸定妆.png").convert("RGB"))
    frames = dict(clips(source, neutral))
    rest, _ = clean_frame(neutral, neutral, False, mouth(neutral))
    frames = {"REST": [(rest, (0, 0))], **frames}
    strip = Image.fromarray(np.vstack([im for video in frames.values() for im, xy in video[::7]]))
    palette = [0, 0, 0] + strip.quantize(colors=COLORS - 1).getpalette()[:(COLORS - 1) * 3]
    ink = Image.new("P", (1, 1))
    ink.putpalette(palette)
    rgb = np.array(palette, dtype=np.int16).reshape(-1, 3)
    red, green, blue = rgb.T.astype(np.uint16)
    rgb565 = (red >> 3 << 11) | (green >> 2 << 5) | (blue >> 3)
    blocks, meta, entries = [0], [], []
    with (out / "stopwatch_red.zlib").open("wb") as stream:
        for key, sequence in frames.items():
            entries.append((key, len(meta), len(blocks) - 1, len(sequence)))
            for base in range(0, len(sequence), GROUP):
                part = sequence[base:base + GROUP]
                raw = bytearray()
                previous = None
                for image, xy in part:
                    indexed = np.array(Image.fromarray(image).quantize(palette=ink, dither=0))
                    indexed[image.max(axis=2) < 5] = 0
                    if previous is None:
                        delta = indexed
                    else:
                        near = np.max(abs(rgb[previous] - image.astype(np.int16)), axis=2) <= 11
                        indexed[near] = previous[near]
                        delta = indexed ^ previous
                    raw.extend(delta.tobytes())
                    previous = indexed
                    meta.append(xy[0] | xy[1] << 8)
                data = zlib.compress(raw, 9)
                stream.write(data)
                blocks.append(stream.tell())
    with (out / "stopwatch_red_data.h").open("w", encoding="utf-8") as h:
        h.write("/* Generated by tools/muse/pack_stopwatch_red.py. */\n#pragma once\n\n")
        h.write(f"#define RED_W {W}\n#define RED_H {H}\n#define RED_GROUP {GROUP}\n")
        h.write(f"#define RED_X {LEFT - 73}\n#define RED_Y {TOP - 53}\n")
        h.write(f"#define RED_DRAW_W {RIGHT - LEFT}\n#define RED_DRAW_H {BOTTOM - TOP}\n\n")
        h.write("typedef struct { uint16_t frame, block, count; } red_clip_t;\n")
        h.write("enum {\n" + "".join(f"    RED_{name},\n" for name, *_ in entries) + "    RED_COUNT\n};\n")
        h.write("static const red_clip_t red_clips[RED_COUNT] = {\n")
        h.write("".join(f"    [RED_{n}] = {{{f}, {b}, {c}}},\n" for n, f, b, c in entries) + "};\n")
        for kind, name, values in [
            ("uint16_t", "red_palette", rgb565),
            ("uint32_t", "red_offsets", blocks),
            ("uint16_t", "red_mouth", meta),
        ]:
            h.write(f"static const {kind} {name}[] = {{\n{c_array(values)}\n}};\n")
    print(f"{len(CLIPS)} clips, {len(meta)} frames, {blocks[-1]} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()
    pack(args.source, args.output_directory)
