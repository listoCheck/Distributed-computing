"""Create a source-only submission archive with the required pa4/ layout."""
from pathlib import Path
import tarfile

root = Path(__file__).resolve().parent
files = sorted((root / 'pa4').glob('*.c')) + sorted((root / 'pa4').glob('*.h'))
files.append(root / 'pa4' / 'Makefile')
with tarfile.open(root / 'pa4.tar.gz', 'w:gz') as archive:
    for path in files:
        archive.add(path, arcname='pa4/' + path.name)
print('Created pa4.tar.gz: %d source/header/build files' % len(files))
