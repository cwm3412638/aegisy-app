#!/usr/bin/env python3
import argparse
import json
from pathlib import Path

from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN
from pptx.util import Inches, Pt


TEAL = RGBColor(15, 118, 110)
DARK = RGBColor(16, 24, 40)
MUTED = RGBColor(71, 84, 103)
LIGHT = RGBColor(231, 245, 242)
WHITE = RGBColor(255, 255, 255)


def set_run(run, size, color, bold=False):
    run.font.name = "Arial"
    run.font.size = Pt(size)
    run.font.color.rgb = color
    run.font.bold = bold


def add_title(slide, title, number):
    accent = slide.shapes.add_shape(1, Inches(0), Inches(0), Inches(0.16), Inches(7.5))
    accent.fill.solid()
    accent.fill.fore_color.rgb = TEAL
    accent.line.fill.background()
    box = slide.shapes.add_textbox(Inches(0.65), Inches(0.48), Inches(11.5), Inches(0.65))
    paragraph = box.text_frame.paragraphs[0]
    paragraph.text = title
    set_run(paragraph.runs[0], 26, DARK, True)
    footer = slide.shapes.add_textbox(Inches(11.9), Inches(7.05), Inches(0.8), Inches(0.25))
    footer.text_frame.paragraphs[0].text = str(number)
    footer.text_frame.paragraphs[0].alignment = PP_ALIGN.RIGHT
    set_run(footer.text_frame.paragraphs[0].runs[0], 9, MUTED)


def add_bullets(slide, bullets):
    body = slide.shapes.add_textbox(Inches(0.85), Inches(1.45), Inches(11.45), Inches(5.1))
    frame = body.text_frame
    frame.word_wrap = True
    frame.clear()
    for index, value in enumerate(bullets[:7]):
        paragraph = frame.paragraphs[0] if index == 0 else frame.add_paragraph()
        paragraph.text = str(value)
        paragraph.level = 0
        paragraph.space_after = Pt(15)
        paragraph.line_spacing = 1.15
        set_run(paragraph.runs[0], 20, DARK)


def build(spec, output):
    presentation = Presentation()
    presentation.slide_width = Inches(13.333)
    presentation.slide_height = Inches(7.5)

    title = str(spec.get("title") or "演示文稿")
    subtitle = str(spec.get("subtitle") or "由 Aegisy Skills 生成")
    cover = presentation.slides.add_slide(presentation.slide_layouts[6])
    cover.background.fill.solid()
    cover.background.fill.fore_color.rgb = DARK
    mark = cover.shapes.add_shape(1, Inches(0.75), Inches(1.0), Inches(0.18), Inches(4.9))
    mark.fill.solid()
    mark.fill.fore_color.rgb = TEAL
    mark.line.fill.background()
    title_box = cover.shapes.add_textbox(Inches(1.25), Inches(1.55), Inches(10.8), Inches(1.6))
    title_box.text_frame.word_wrap = True
    title_box.text_frame.paragraphs[0].text = title
    set_run(title_box.text_frame.paragraphs[0].runs[0], 38, WHITE, True)
    subtitle_box = cover.shapes.add_textbox(Inches(1.28), Inches(3.45), Inches(9.5), Inches(0.7))
    subtitle_box.text_frame.paragraphs[0].text = subtitle
    set_run(subtitle_box.text_frame.paragraphs[0].runs[0], 18, LIGHT)

    slides = spec.get("slides") or []
    for index, item in enumerate(slides[:20], start=2):
        slide = presentation.slides.add_slide(presentation.slide_layouts[6])
        slide.background.fill.solid()
        slide.background.fill.fore_color.rgb = WHITE
        add_title(slide, str(item.get("title") or f"第 {index - 1} 部分"), index)
        bullets = item.get("bullets") or []
        add_bullets(slide, bullets if isinstance(bullets, list) else [str(bullets)])

    output_path = Path(output).expanduser().resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    presentation.save(output_path)
    print(json.dumps({"ok": True, "path": str(output_path)}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spec", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    with open(args.spec, "r", encoding="utf-8") as source:
        build(json.load(source), args.out)


if __name__ == "__main__":
    main()
