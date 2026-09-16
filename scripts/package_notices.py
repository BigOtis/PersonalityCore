"""Preserve dependency notices and speech-library source alongside portable builds."""
import importlib.metadata
import json
from pathlib import Path
import shutil
import sys
import urllib.request

root = Path(sys.argv[1]) / 'ThirdParty'
root.mkdir(parents=True, exist_ok=True)
for package, version in [('phonemizer', '3.4.0'), ('espeakng-loader', '0.2.4')]:
    if importlib.metadata.version(package) != version:
        raise RuntimeError(f'Update the corresponding source archive for {package} before packaging this version.')
inventory = []
for dist in importlib.metadata.distributions():
    name = dist.metadata['Name']
    inventory.append({'name': name, 'version': dist.version, 'source': dist.metadata.get_all('Project-URL', [])})
    for entry in dist.files or []:
        if '.dist-info' in str(entry) and any(word in entry.name.lower() for word in ('license', 'copying', 'notice')):
            source = Path(dist.locate_file(entry))
            if source.is_file():
                target = root / 'Python' / name / entry.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
(root / 'build-environment.json').write_text(json.dumps(inventory, indent=2), encoding='utf-8')
archives = {
    'phonemizer-3.4.0.zip': 'https://codeload.github.com/bootphon/phonemizer/zip/refs/tags/v3.4.0',
    'espeakng-loader-0.2.4.zip': 'https://codeload.github.com/thewh1teagle/espeakng-loader/zip/0ddc87adf77e5850d7eeb542ac8a87d421b64daa',
    'espeak-ng-1.52.0.zip': 'https://codeload.github.com/espeak-ng/espeak-ng/zip/refs/tags/1.52.0',
}
for name, url in archives.items():
    with urllib.request.urlopen(url, timeout=90) as response, (root / name).open('wb') as target:
        shutil.copyfileobj(response, target)
(root / 'README.txt').write_text('Dependency license notices and speech-library source archives accompany this build.\n'
    'espeak-ng.dll version 1.52.0 is distributed through espeakng-loader 0.2.4.\n'
    'Phonemizer source is version 3.4.0. Source archives contain their licenses and build instructions.\n'
    'Model weights and external inference servers are not included.\n', encoding='utf-8')
