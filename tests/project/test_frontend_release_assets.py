"""Synthetic frontend release/cache contracts, no browser or network."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('release_asset_contract',ROOT/'scripts/project/frontend_release.py')
release=importlib.util.module_from_spec(spec);spec.loader.exec_module(release)


class ReleaseAssets(unittest.TestCase):
    def test_image_bytes_invalidate_immutable_urls_and_check_is_read_only(self):
        base=ROOT/'build-function3/frontend-release-assets';base.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            front=Path(temporary)
            (front/'index.html').write_text('<img src="/phase11/icon.png"><script src="./app.js"></script>',encoding='utf-8')
            (front/'app.js').write_text("export const img='./icon.png';",encoding='utf-8')
            (front/'icon.png').write_bytes(b'fixture-image-v1')
            with patch.object(release,'FRONT',front),patch.object(release,'ROOT',front),patch.object(sys,'argv',['release']),patch('builtins.print'):
                release.main()
                first=json.loads((front/'asset-manifest.json').read_text())
                self.assertEqual(first['binaryImages'],1)
                self.assertIn('icon.png?v='+first['version'],(front/'app.js').read_text())
                self.assertIn('icon.png?v='+first['version'],(front/'index.html').read_text())
                before={p.name:p.read_bytes() for p in front.iterdir()}
                with patch.object(sys,'argv',['release','--check']):release.main()
                self.assertEqual(before,{p.name:p.read_bytes() for p in front.iterdir()})
                (front/'icon.png').write_bytes(b'fixture-image-v2')
                with patch.object(sys,'argv',['release','--check']):
                    with self.assertRaises(SystemExit):release.main()
                release.main()
                second=json.loads((front/'asset-manifest.json').read_text())
                self.assertNotEqual(first['version'],second['version'])

    def test_avatar_is_bounded_and_retains_rgba_transparency(self):
        content=(ROOT/'frontend/phase11/shared/XiaoYan/XiaoYan.png').read_bytes()
        self.assertLess(len(content),32*1024)
        self.assertEqual(content[:8],b'\x89PNG\r\n\x1a\n')
        width,height,depth,color=struct.unpack('>IIBB',content[16:26])
        self.assertEqual((width,height,depth,color),(140,128,8,6))


if __name__=='__main__':unittest.main()
