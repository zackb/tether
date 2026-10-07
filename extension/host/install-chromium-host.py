#!/usr/bin/env python3
"""Register an unpacked Chrome/Chromium extension with Tether (user scope)."""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('extension_id')
    parser.add_argument('--browser', choices=['chrome', 'chromium'], default='chrome')
    parser.add_argument('--host', default=shutil.which('tether-native-host'))
    args = parser.parse_args()
    if not re.fullmatch('[a-p]{32}', args.extension_id):
        parser.error('extension ID must contain 32 letters from a to p')
    if not args.host or not Path(args.host).is_absolute():
        parser.error('install tether-native-host or specify --host /absolute/path')
    host = Path(args.host)
    if not host.is_file() or not os.access(host, os.X_OK):
        parser.error('native host must be an executable file')
    browser = 'google-chrome' if args.browser == 'chrome' else 'chromium'
    target = Path.home() / '.config' / browser / 'NativeMessagingHosts' / 'com.tether.extension.json'
    origins = []
    if target.exists():
        previous = json.loads(target.read_text())
        origins = previous.get('allowed_origins', [])
        if not isinstance(origins, list) or not all(isinstance(x, str) for x in origins):
            parser.error('existing allowed_origins must be a list of strings')
    origin = f'chrome-extension://{args.extension_id}/'
    if origin not in origins:
        origins.append(origin)
    manifest = dict(name='com.tether.extension', description='Tether Native Messaging Host',
                    path=str(host), type='stdio', allowed_origins=origins)
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
        shutil.copy2(target, target.with_name(target.name + '.backup-' + stamp))
    temporary = target.with_name(target.name + '.tmp')
    temporary.write_text(json.dumps(manifest, indent=2) + '\n')
    temporary.replace(target)
    print(f'Registered {origin} in {target}')


if __name__ == '__main__':
    main()
