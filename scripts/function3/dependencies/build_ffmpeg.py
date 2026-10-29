"""Build the approved FFmpeg 8.1.3 source already present on Linux. No downloads."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tarfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--archive', type=Path, required=True)
a = p.parse_args()
assert hashlib.sha256(a.archive.read_bytes()).hexdigest() == 'a516d39ac7a92ff7c6f2c44b0338aaecf708c42f3d4febc8cb9203de6b1e2603', 'unapproved source archive'
root = Path('/home/pikachu/phase11-deps/ffmpeg/8.1.3')
source = root/'source'
source.mkdir(parents=True, exist_ok=True)
with tarfile.open(a.archive) as archive:
    for member in archive.getmembers():
        parts = Path(member.name).parts[1:]
        if not parts: continue
        assert '..' not in parts and not Path(*parts).is_absolute()
        output = source.joinpath(*parts)
        if member.isdir(): output.mkdir(parents=True, exist_ok=True)
        else:
            assert member.isfile(), 'source links not accepted'
            data = archive.extractfile(member).read()
            if not output.exists() or output.read_bytes() != data:
                output.parent.mkdir(parents=True, exist_ok=True); output.write_bytes(data)
                output.chmod(member.mode & 0o755)
# image_*_pipe is the configure component name; png_pipe is just its display name.
args = ['sh', './configure', '--prefix='+str(root/'install'), '--disable-autodetect',
        '--disable-gpl', '--disable-nonfree', '--disable-network', '--disable-everything',
        '--disable-doc', '--disable-debug', '--disable-x86asm', '--enable-zlib',
        '--enable-ffmpeg', '--enable-ffprobe', '--enable-avcodec', '--enable-avformat',
        '--enable-avfilter', '--enable-swresample', '--enable-swscale',
        '--enable-protocol=file,pipe',
        '--enable-demuxer=wav,ogg,matroska,mp3,image2,image_png_pipe,image_jpeg_pipe,gif,image_webp_pipe',
        '--enable-decoder=pcm_s16le,pcm_s24le,pcm_f32le,mp3,mp3float,vorbis,opus,flac,png,mjpeg,gif,webp',
        '--enable-parser=png,mjpeg,webp,mpegaudio,opus,vorbis', '--enable-encoder=pcm_s16le,png',
        '--enable-muxer=wav,image2,null', '--enable-filter=aresample,aformat,format,scale,anull,null']
for command, name in ((args, 'configure.log'), (['make', '-j', '2'], 'build.log'), (['make', 'install'], 'install.log')):
    with (root/name).open('w') as log:
        subprocess.run(command, cwd=source, stdout=log, stderr=subprocess.STDOUT, check=True)
print('FFmpeg built from verified source, local files only; no global environment changed.')
