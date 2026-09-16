from pathlib import Path
from PyInstaller.utils.hooks import collect_all, copy_metadata

root = Path(SPECPATH).parent
datas = [(str(root / 'app' / 'dist'), 'ui')]
binaries = []
hiddenimports = ['uvicorn.logging', 'uvicorn.loops.auto', 'uvicorn.protocols.http.auto',
                 'uvicorn.protocols.websockets.auto', 'uvicorn.lifespan.on']
for package in ('kokoro_onnx', 'espeakng_loader', 'faster_whisper', 'ctranslate2', 'phonemizer'):
    d, b, h = collect_all(package)
    datas += d
    binaries += b
    hiddenimports += h
for distribution in ('kokoro-onnx', 'phonemizer', 'faster-whisper', 'localtalker'):
    datas += copy_metadata(distribution)

a = Analysis([str(root / 'scripts' / 'runtime_entry.py')], pathex=[str(root / 'src')],
             binaries=binaries, datas=datas, hiddenimports=hiddenimports,
             excludes=['pytest', 'tkinter', 'matplotlib', 'IPython', 'torch', 'tensorflow', 'transformers'])
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name='localtalker-runtime',
          console=True, upx=False)
coll = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name='localtalker-runtime')
