"""Apply the PersonalityCore icon and product metadata to the Windows shell."""
import hashlib
from pathlib import Path
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
EDITOR = ROOT / 'build/tools/rcedit-x64-v2.0.0.exe'
SHA256 = '3e7801db1a5edbec91b49a24a094aad776cb4515488ea5a4ca2289c400eade2a'
URL = 'https://github.com/electron/rcedit/releases/download/v2.0.0/rcedit-x64.exe'

def main():
    executable = Path(sys.argv[1]).resolve()
    if not executable.is_file():
        raise FileNotFoundError(executable)
    if not EDITOR.exists():
        EDITOR.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(URL, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise RuntimeError('Resource editor checksum differs from the pinned official build')
        EDITOR.write_bytes(data)
    if hashlib.sha256(EDITOR.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('Resource editor checksum verification failed')
    args = [str(EDITOR), str(executable), '--set-icon', str(ROOT / 'app/electron/personalitycore.ico'),
            '--set-file-version', '0.2.0.0', '--set-product-version', '0.2.0.0']
    for key, value in {'ProductName': 'PersonalityCore', 'FileDescription': 'PersonalityCore Character Studio',
                       'CompanyName': 'PersonalityCore contributors', 'InternalName': 'PersonalityCore',
                       'OriginalFilename': 'PersonalityCore.exe'}.items():
        args.extend(['--set-version-string', key, value])
    subprocess.run(args, check=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    print('PersonalityCore executable icon and Windows product metadata applied.')

if __name__ == '__main__':
    main()
