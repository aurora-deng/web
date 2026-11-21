"""Render with the existing Poppler/Pillow runtime; never modify originals."""

import base64
import binascii
import hashlib
import io
import json
import math
import os
import re
import stat
import subprocess
import tempfile
import threading
from collections import OrderedDict
from functools import lru_cache
from pathlib import Path
from weakref import WeakValueDictionary

import extract_attachment  # Adds the already bundled document/image libraries.
from PIL import Image, ImageDraw, ImageFont, ImageOps
from pypdf import PdfReader, PdfWriter

LOCK = threading.Lock()
_METADATA_LIMIT = 128
_METADATA = OrderedDict()
_WORK_LOCKS = WeakValueDictionary()
_PREVIEW_FILE_LIMIT = 60
_PREVIEW_BYTE_LIMIT = 256 * 1024**2
_RENDER_SLOTS = threading.BoundedSemaphore(2)
RENDERER = Path.home() / '.cache/codex-runtimes/codex-primary-runtime/dependencies/native/poppler/Library/bin/pdftoppm.exe'
_WINDOWS_FONTS = Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Fonts'
_TEXT_FONT_PATHS = (
    _WINDOWS_FONTS / 'msyh.ttc',
    _WINDOWS_FONTS / 'NotoSansSC-VF.ttf',
    _WINDOWS_FONTS / 'simhei.ttf',
    _WINDOWS_FONTS / 'simsun.ttc',
)


@lru_cache(maxsize=32)
def _text_font(size):
    for path in _TEXT_FONT_PATHS:
        try:
            return ImageFont.truetype(str(path), size)
        except OSError:
            continue
    raise ValueError('已有中文字体不可用，无法导出文字批注；未安装新字体')


def _draw_text(draw, stroke, width, height, color):
    # Lay out in original-image space, before the shared crop/rotation step.
    size = max(1, round(stroke.get('fontSize', .025) * width))
    font = _text_font(size)
    left, top = stroke['points'][0][0] * width, stroke['points'][0][1] * height
    available = max(size, width - left)
    row = 0
    for paragraph in stroke['text'].replace('\r\n', '\n').replace('\r', '\n').split('\n'):
        line = ''
        for character in paragraph:
            if line and draw.textlength(line + character, font=font) > available:
                draw.text((left, top + row * size * 1.3), line, font=font, fill=color, anchor='lt')
                row += 1
                if top + row * size * 1.3 >= height:
                    return
                line = ''
            line += character
        draw.text((left, top + row * size * 1.3), line, font=font, fill=color, anchor='lt')
        row += 1
        if top + row * size * 1.3 >= height:
            break


def _math_raster(stroke):
    raster = stroke.get('mathRaster')
    if raster is None:
        return None
    if (not isinstance(raster, dict) or raster.get('version') != 1
            or raster.get('source') != stroke['text'] or raster.get('color') != stroke['color']
            or raster.get('fontSize') != stroke.get('fontSize', .025)):
        raise ValueError('公式批注与原文不一致，请重新编辑后保存')
    for key, maximum in (('width', 128), ('height', 512)):
        value = raster.get(key)
        if type(value) not in (int, float) or not .01 <= value <= maximum:
            raise ValueError('公式批注尺寸无效')
    png = raster.get('png')
    prefix = 'data:image/png;base64,'
    if not isinstance(png, str) or not png.startswith(prefix) or len(png) > 1400000:
        raise ValueError('公式批注图像无效或过大')
    try:
        raw = base64.b64decode(png[len(prefix):], validate=True)
        with Image.open(io.BytesIO(raw)) as image:
            if (image.format != 'PNG' or not 1 <= image.width <= 2048
                    or not 1 <= image.height <= 4096 or image.width * image.height > 4000000):
                raise ValueError('公式批注图像尺寸无效')
            image.verify()
        return raw
    except (binascii.Error, OSError, Image.DecompressionBombError) as error:
        raise ValueError('公式批注图像损坏') from error


def is_pdf(info):
    return info['type'] == 'application/pdf' or info['name'].lower().endswith('.pdf')


def _work_lock(key):
    # Waiters keep a strong reference; idle locks do not accumulate with files.
    with LOCK:
        lock = _WORK_LOCKS.get(key)
        if lock is None:
            lock = threading.Lock()
            _WORK_LOCKS[key] = lock
        return lock


def _source_version(source):
    try:
        status = source.stat()
    except (FileNotFoundError, NotADirectoryError):
        raise ValueError('原附件不存在') from None
    if not stat.S_ISREG(status.st_mode):
        raise ValueError('原附件不存在')
    return (source, status.st_dev, status.st_ino, status.st_size,
            status.st_mtime_ns, status.st_ctime_ns)


def _check_source(source, version):
    if _source_version(source) != version:
        raise ValueError('原附件已更改，请重新打开预览')


def _cached_metadata(key):
    with LOCK:
        metadata = _METADATA.get(key)
        if metadata is not None:
            _METADATA.move_to_end(key)
        return metadata


def _preview_metadata(source, identity, pdf, version):
    key = (identity, pdf, version)
    metadata = _cached_metadata(key)
    if metadata is not None:
        return metadata
    token = hashlib.sha256(repr(key).encode()).hexdigest()
    if not pdf:
        return 1, token
    with _work_lock(('metadata', key)):
        _check_source(source, version)
        metadata = _cached_metadata(key)
        if metadata is not None:
            return metadata
        with source.open('rb') as stream:
            count = len(PdfReader(stream).pages)
        _check_source(source, version)
        metadata = count, token
        with LOCK:
            _METADATA[key] = metadata
            _METADATA.move_to_end(key)
            while len(_METADATA) > _METADATA_LIMIT:
                _METADATA.popitem(last=False)
        return metadata


def _prune_previews(folder, target):
    # Publication and pruning share a per-directory lock, never the render lock.
    cached = []
    for path in folder.glob('*.png'):
        try:
            status = path.stat()
        except FileNotFoundError:
            continue
        cached.append((status.st_mtime_ns, path, status.st_size))
    cached.sort(key=lambda entry: entry[0])
    size = sum(entry[2] for entry in cached)
    count = len(cached)
    for _, path, length in cached:
        if path != target and (count > _PREVIEW_FILE_LIMIT or size > _PREVIEW_BYTE_LIMIT):
            path.unlink(missing_ok=True)
            size -= length
            count -= 1


def render_page(data, info, page=1):
    data=Path(data)
    from library_sources import resolve_file
    source=resolve_file(data,info['id'])
    version=_source_version(source)
    page=int(page)
    pdf=is_pdf(info)
    count,token=_preview_metadata(source,info['id'],pdf,version)
    if page < 1 or page > count:
        raise ValueError('页码超出附件范围')
    folder=data/'previews';folder.mkdir(exist_ok=True)
    key=hashlib.sha256(f'{token}:{page}'.encode()).hexdigest()[:32]
    target=folder/(key+'.png')
    directory_key=('previews',folder.resolve())
    with _work_lock(('page',target.resolve())):
        with _work_lock(directory_key):
            _check_source(source,version)
            if target.is_file():
                return target,count
        with tempfile.TemporaryDirectory(prefix='render-',dir=folder) as staging:
            prefix=Path(staging)/'page'
            # Allow independent pages to progress without an unbounded RAM spike.
            with _RENDER_SLOTS:
                if pdf:
                    if not RENDERER.is_file():
                        raise ValueError('已有 PDF 渲染组件不可用，仍可下载原件；未安装新组件')
                    result=subprocess.run([str(RENDERER),'-f',str(page),'-l',str(page),'-scale-to','2200','-png','-singlefile',str(source),str(prefix)],capture_output=True,timeout=45,creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                    if result.returncode or not prefix.with_suffix('.png').is_file():
                        raise ValueError('PDF 页面无法渲染，请检查附件是否损坏或加密')
                else:
                    with Image.open(source) as image:
                        normalized=ImageOps.exif_transpose(image).convert('RGB')
                        normalized.thumbnail((2200,2200))
                        normalized.save(prefix.with_suffix('.png'))
            with _work_lock(directory_key):
                _check_source(source,version)
                prefix.with_suffix('.png').replace(target)
                _prune_previews(folder,target)
    return target,count


def validate_edit(edit):
    if not isinstance(edit,dict):raise ValueError('批注格式无效')
    crop=edit.get('crop',[0,0,1,1]);rotation=edit.get('rotation',0);strokes=edit.get('strokes',[])
    if rotation not in (0,90,180,270) or not isinstance(crop,list) or len(crop)!=4:
        raise ValueError('裁剪或旋转参数无效')
    if not all(type(value) in (int,float) and 0<=value<=1 for value in crop) or min(crop[2:])<.01 or crop[0]+crop[2]>1.00001 or crop[1]+crop[3]>1.00001:
        raise ValueError('裁剪区域无效')
    if not isinstance(strokes,list) or len(strokes)>1000:raise ValueError('批注过多，请分批保存')
    for stroke in strokes:
        if not isinstance(stroke,dict):raise ValueError('批注格式无效')
        if stroke.get('tool') not in ('pen','highlight','circle','text') or not isinstance(stroke.get('color'),str) or not re.fullmatch(r'#[0-9a-fA-F]{6}',stroke['color']):
            raise ValueError('批注工具或颜色无效')
        width=stroke.get('width',.003)
        if type(width) not in (int,float) or not .0001<=width<=.1:raise ValueError('笔画宽度无效')
        points=stroke.get('points',[])
        if not isinstance(points,list) or not 1<=len(points)<=10000:raise ValueError('笔画点数无效')
        for point in points:
            if not isinstance(point,list) or len(point)!=2 or not all(type(value) in (int,float) and 0<=value<=1 for value in point):raise ValueError('笔画坐标无效')
        if stroke['tool']=='text':
            text=stroke.get('text');size=stroke.get('fontSize',.025)
            if not isinstance(text,str) or not text.strip() or len(text)>2000 or any(0xD800<=ord(character)<=0xDFFF for character in text):
                raise ValueError('文字批注内容无效或超过 2000 字')
            if len(points)!=1:raise ValueError('文字批注需要一个定位点')
            if type(size) not in (int,float) or not .008<=size<=.12:
                raise ValueError('文字批注字号无效')
            _math_raster(stroke)
    return {'crop':crop,'rotation':rotation,'strokes':strokes}


def annotated_image(data,info,edit,page=1,marks=None):
    edit=validate_edit(edit)
    rendered,_=render_page(data,info,page)
    with Image.open(rendered) as raw:image=raw.convert('RGBA')
    width,height=image.size
    from reading_export import validate as validate_mark
    for mark in marks or []:
        if mark.get('page') != page or mark.get('kind') == 'bookmark':
            continue
        validate_mark(mark)
        layer=Image.new('RGBA',image.size,(0,0,0,0));draw=ImageDraw.Draw(layer)
        color_text=mark.get('color','#d8465b')
        color=tuple(int(color_text[i:i+2],16) for i in (1,3,5))+(64 if mark['kind']=='highlight' else 255,)
        if mark['kind']=='highlight':
            for x,y,w,h in mark.get('regions',[]):
                draw.rectangle((round(x*width),round(y*height),round((x+w)*width),round((y+h)*height)),fill=color)
        else:
            thickness=mark.get('width',.003)
            if not isinstance(thickness,(int,float)) or not math.isfinite(thickness) or not .0001<=thickness<=.1:
                raise ValueError('阅读笔画宽度无效')
            points=[(round(x*width),round(y*height)) for x,y in mark.get('points',[])]
            line_width=max(1,round(thickness*min(width,height)))
            if len(points)>1:draw.line(points,fill=color,width=line_width,joint='curve')
            elif points:
                x,y=points[0];radius=line_width/2;draw.ellipse((x-radius,y-radius,x+radius,y+radius),fill=color)
        image=Image.alpha_composite(image,layer)
    for stroke in edit['strokes']:
        layer=Image.new('RGBA',image.size,(0,0,0,0));draw=ImageDraw.Draw(layer)
        color=tuple(int(stroke['color'][i:i+2],16) for i in (1,3,5))+(72 if stroke['tool']=='highlight' else 255,)
        coordinates=[(int(point[0]*width),int(point[1]*height)) for point in stroke['points']]
        line_width=max(1,round(stroke.get('width',.003)*min(width,height)))
        if stroke['tool']=='text':
            raster = stroke.get('mathRaster')
            if raster:
                with Image.open(io.BytesIO(_math_raster(stroke))) as source:
                    size = stroke.get('fontSize', .025) * width
                    dimensions = (max(1, round(raster['width'] * size)), max(1, round(raster['height'] * size)))
                    # Clip before resampling so a tiny PNG cannot request a huge allocation.
                    left, top = coordinates[0]
                    visible = (min(dimensions[0], width - left), min(dimensions[1], height - top))
                    if min(visible) > 0:
                        box = (0, 0, source.width * visible[0] / dimensions[0], source.height * visible[1] / dimensions[1])
                        annotation = source.convert('RGBA').resize(visible, Image.Resampling.LANCZOS, box=box)
                        layer.alpha_composite(annotation, (left, top))
            else:
                _draw_text(draw,stroke,width,height,color)
        elif stroke['tool']=='circle' and len(coordinates)>1:
            x1,y1=coordinates[0];x2,y2=coordinates[-1]
            draw.ellipse((min(x1,x2),min(y1,y2),max(x1,x2),max(y1,y2)),outline=color,width=line_width)
        elif len(coordinates)>1:draw.line(coordinates,fill=color,width=line_width,joint='curve')
        else:
            x,y=coordinates[0];r=line_width/2;draw.ellipse((x-r,y-r,x+r,y+r),fill=color)
        image=Image.alpha_composite(image,layer)
    x,y,w,h=edit['crop']
    image=image.crop((round(x*width),round(y*height),round((x+w)*width),round((y+h)*height)))
    if edit['rotation']:image=image.rotate(-edit['rotation'],expand=True)
    return image.convert('RGB')


def export(data,info,edits,page=1,pdf=False,marks=None):
    marks=marks or []
    if len(marks)>10000:raise ValueError('批注数量超过本次导出上限')
    output=io.BytesIO()
    if pdf and is_pdf(info):
        from library_sources import resolve_file
        source=resolve_file(data,info['id'])
        if marks:
            from reading_export import export as export_reading
            source=io.BytesIO(export_reading(source,marks))
        original=PdfReader(source);writer=PdfWriter()
        for index,original_page in enumerate(original.pages):
            edit=edits.get(f"{info['id']}:{index+1}")
            if not edit:writer.add_page(original_page);continue
            rendered,_=render_page(data,info,index+1)
            with Image.open(rendered) as raw:raw_width=raw.width
            display_width=float(original_page.mediabox.height if original_page.rotation%180 else original_page.mediabox.width)
            resolution=max(20,raw_width/display_width*72)
            converted=io.BytesIO();annotated_image(data,info,edit,index+1,marks).save(converted,'PDF',resolution=resolution);converted.seek(0)
            writer.add_page(PdfReader(converted).pages[0])
            comments=[str(mark.get('text','')) for mark in marks if mark.get('page')==index+1 and mark.get('text') and mark.get('kind')!='bookmark']
            if comments:
                from pypdf.annotations import Text
                writer.add_annotation(index,Text(rect=(8,8,28,28),text='\n\n'.join(comments)[:10000]))
        for mark in marks:
            if mark.get('kind')=='bookmark':writer.add_outline_item(str(mark.get('text') or '书签')[:200],mark['page']-1)
        writer.write(output)
    else:
        annotated_image(data,info,edits.get(f"{info['id']}:{page}",{}),page,marks).save(output,'PNG')
    return output.getvalue()
