"""Write native PDF annotations, keeping original page content and source bytes."""

import io
import math
import re

import extract_attachment
from pypdf import PdfReader, PdfWriter
from pypdf.annotations import Highlight, Text
from pypdf.generic import ArrayObject, DictionaryObject, FloatObject, NameObject, TextStringObject


def numbers(values):
    return ArrayObject([FloatObject(value) for value in values])


def coordinates(page, point):
    x, y = point
    width, height = float(page.cropbox.width), float(page.cropbox.height)
    rotation = page.rotation % 360
    if rotation == 90:
        x, y = y * width, x * height
    elif rotation == 180:
        x, y = (1 - x) * width, y * height
    elif rotation == 270:
        x, y = (1 - y) * width, (1 - x) * height
    else:
        x, y = x * width, (1 - y) * height
    return float(page.cropbox.left) + x, float(page.cropbox.bottom) + y


def validate(mark):
    if mark.get('kind') not in {'ink', 'highlight', 'bookmark'}:
        raise ValueError('不支持的批注类型')
    if not re.fullmatch(r'#[0-9a-fA-F]{6}', mark.get('color', '#d8465b')):
        raise ValueError('批注颜色无效')
    points = mark.get('points', [])
    if not isinstance(points, list) or len(points) > 10000:
        raise ValueError('批注点数过多')
    for point in points:
        if not isinstance(point, list) or len(point) != 2 or not all(isinstance(value, (int, float)) and math.isfinite(value) and 0 <= value <= 1 for value in point):
            raise ValueError('批注坐标无效')
    for region in mark.get('regions', []):
        if not isinstance(region, list) or len(region) != 4 or not all(isinstance(value, (int, float)) and math.isfinite(value) and 0 <= value <= 1 for value in region):
            raise ValueError('高亮区域无效')
        if region[0] + region[2] > 1.000001 or region[1] + region[3] > 1.000001:
            raise ValueError('高亮区域超出原页')


def export(source, marks):
    if len(marks) > 10000:
        raise ValueError('批注数量超过本次导出上限')
    reader, writer = PdfReader(source), PdfWriter()
    if reader.is_encrypted:
        raise ValueError('加密 PDF 请先使用有权访问的解密副本')
    writer.append(reader)
    for mark in marks:
        validate(mark)
        number = mark.get('page', 0)
        if not isinstance(number, int) or not 1 <= number <= len(writer.pages):
            raise ValueError('批注页码超出文件范围')
        page = writer.pages[number - 1]
        color = mark.get('color', '#d8465b')[1:]
        if mark['kind'] == 'ink':
            points = [coordinates(page, point) for point in mark.get('points', [])]
            if not points:
                continue
            width = float(mark.get('width', .003))
            if not math.isfinite(width) or not .0001 <= width <= .1:
                raise ValueError('笔画宽度无效')
            xs, ys = [point[0] for point in points], [point[1] for point in points]
            annotation = DictionaryObject({NameObject('/Type'): NameObject('/Annot'), NameObject('/Subtype'): NameObject('/Ink'),
                NameObject('/Rect'): numbers([min(xs) - 2, min(ys) - 2, max(xs) + 2, max(ys) + 2]),
                NameObject('/InkList'): ArrayObject([numbers([value for point in points for value in point])]),
                NameObject('/C'): numbers([int(color[index:index + 2], 16) / 255 for index in (0, 2, 4)]),
                NameObject('/Contents'): TextStringObject(str(mark.get('text', ''))[:10000]),
                NameObject('/BS'): DictionaryObject({NameObject('/W'): FloatObject(width * min(float(page.cropbox.width), float(page.cropbox.height)))})})
            writer.add_annotation(number - 1, annotation)
        elif mark['kind'] == 'highlight':
            for x, y, width, height in mark.get('regions', []):
                points = [coordinates(page, point) for point in ((x, y), (x + width, y), (x, y + height), (x + width, y + height))]
                xs, ys = [point[0] for point in points], [point[1] for point in points]
                annotation = Highlight(rect=(min(xs), min(ys), max(xs), max(ys)), quad_points=numbers([value for point in points for value in point]), highlight_color=color, printing=True)
                annotation[NameObject('/Contents')] = TextStringObject(str(mark.get('text', ''))[:10000])
                writer.add_annotation(number - 1, annotation)
        elif mark['kind'] == 'bookmark':
            writer.add_outline_item(str(mark.get('text') or '第 ' + str(number) + ' 页')[:200], number - 1)
    output = io.BytesIO()
    writer.write(output)
    return output.getvalue()
