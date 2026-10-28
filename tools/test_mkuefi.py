#!/usr/bin/env python3
"""Exercise the actual UEFI packaging script with a failing image builder."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILDER = r'''#!/usr/bin/env python3
import os
from pathlib import Path
import sys
args = sys.argv[1:]
out = Path(args[args.index('-o') + 1])
stage = Path(args[-1])
assert (stage / 'boot/nyx-kernel.bin').read_bytes() == b'kernel fixture'
cfg = (stage / 'boot/grub/grub.cfg').read_text()
assert cfg.count('module2 /boot/nyx-kernel.bin nyxkernel.bin') == 4
assert not list(stage.rglob('*.log'))
mode = os.environ['BUILDER_MODE']
if mode in ('partial', 'success'):
    out.write_bytes(b'new image fixture')
elif mode == 'missing':
    out.unlink()
print('builder diagnostic')
sys.exit(17 if mode in ('fail', 'partial') else 0)
'''


def main():
    with tempfile.TemporaryDirectory(prefix='nyx-uefi-contract-') as temp:
        root = Path(temp) / 'source with spaces'
        for subdir in ['tools', 'kernel', 'bin', 'efi', 'temporary']:
            (root / subdir).mkdir(parents=True)
        script = root / 'tools/mkuefi.sh'
        shutil.copyfile(ROOT / 'tools/mkuefi.sh', script)
        (root / 'kernel/nyx-kernel.bin').write_bytes(b'kernel fixture')
        (root / 'efi/modinfo.sh').touch()
        for name, content in [('grub-mkrescue', BUILDER),
                              ('xorriso', '#!/bin/sh\nexit 0\n'),
                              ('mformat', '#!/bin/sh\nexit 0\n')]:
            path = root / 'bin' / name
            path.write_text(content)
            path.chmod(0o755)
        env = dict(os.environ, PATH=str(root / 'bin') + os.pathsep + os.environ['PATH'],
                   NYX_GRUB_EFI_DIR=str(root / 'efi'), TMPDIR=str(root / 'temporary'))
        checks = 0
        for existed in [False, True]:
            for mode in ['fail', 'partial', 'empty', 'missing', 'success']:
                out = root / 'image with spaces.iso'
                out.unlink(missing_ok=True)
                if existed:
                    out.write_bytes(b'previous good image')
                result = subprocess.run(['bash', str(script), str(out)],
                                        env=dict(env, BUILDER_MODE=mode),
                                        text=True, capture_output=True)
                if mode == 'success':
                    assert result.returncode == 0, result.stderr
                    assert out.read_bytes() == b'new image fixture'
                    assert '[mkuefi] wrote ' in result.stdout
                else:
                    assert result.returncode != 0, (mode, result.stdout)
                    assert '[mkuefi] wrote ' not in result.stdout
                    if existed:
                        assert out.read_bytes() == b'previous good image'
                    else:
                        assert not out.exists()
                assert not list(root.glob('*.pending.*'))
                assert not list((root / 'temporary').iterdir())
                checks += 1
        result = subprocess.run(['bash', str(script), str(out)],
                                env=dict(env, NYX_GRUB_EFI_DIR=str(root / 'missing'),
                                         BUILDER_MODE='success'), text=True, capture_output=True)
        assert result.returncode != 0 and 'invalid NYX_GRUB_EFI_DIR' in result.stderr
        assert out.read_bytes() == b'new image fixture'
        checks += 1
        for destination in [root / 'efi', root / 'no-parent/image.iso']:
            result = subprocess.run(['bash', str(script), str(destination)],
                                    env=dict(env, BUILDER_MODE='success'),
                                    text=True, capture_output=True)
            assert result.returncode != 0
            assert '[mkuefi] wrote ' not in result.stdout
            assert not list((root / 'temporary').iterdir())
            assert not list(root.glob('*.pending.*'))
            checks += 1
        # Replacement can fail even after the builder succeeds.
        move = root / 'bin/mv'
        move.write_text('#!/bin/sh\nexit 19\n')
        move.chmod(0o755)
        out.write_bytes(b'previous good image')
        result = subprocess.run(['bash', str(script), str(out)],
                                env=dict(env, BUILDER_MODE='success'),
                                text=True, capture_output=True)
        assert result.returncode != 0 and '[mkuefi] wrote ' not in result.stdout
        assert out.read_bytes() == b'previous good image'
        assert not list(root.glob('*.pending.*'))
        assert not list((root / 'temporary').iterdir())
        print(f'UEFI packaging: {checks + 1} failure/success cases passed')


if __name__ == '__main__':
    main()
