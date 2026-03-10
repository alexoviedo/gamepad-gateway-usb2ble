#!/usr/bin/env python3
"""Generate browser-flashable firmware manifests and static-host feed metadata."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass
class Artifact:
    key: str
    source: Path
    filename: str
    offset: int
    label: str

    @property
    def sha256(self) -> str:
        hasher = hashlib.sha256()
        with self.source.open('rb') as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b''):
                hasher.update(chunk)
        return hasher.hexdigest()


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def write_json(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, indent=2) + '\n', encoding='utf-8')


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--channel', default='stable')
    parser.add_argument('--name', default='HOTAS Bridge')
    parser.add_argument('--chip-family', default='ESP32-S3')
    parser.add_argument('--build', default='')
    parser.add_argument('--release-url', default='')
    parser.add_argument('--published-at', default='')
    parser.add_argument('--release-notes', default='')
    parser.add_argument('--web-root', required=True, help='Destination root, e.g. webapp/firmware')
    parser.add_argument('--bootloader', required=True)
    parser.add_argument('--partition-table', required=True)
    parser.add_argument('--app-binary', required=True)
    return parser.parse_args()


def stage_artifacts(release_dir: Path, artifacts: Iterable[Artifact]) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for artifact in artifacts:
        ensure_dir(release_dir)
        dest = release_dir / artifact.filename
        shutil.copy2(artifact.source, dest)
        out[artifact.key] = {
            'label': artifact.label,
            'path': f'./{artifact.filename}',
            'offset': artifact.offset,
            'sha256': artifact.sha256,
        }
    return out


def main() -> int:
    args = parse_args()

    web_root = Path(args.web_root)
    release_dir = web_root / 'releases' / args.version
    latest_dir = web_root / args.channel
    ensure_dir(release_dir)
    ensure_dir(latest_dir)

    artifacts = [
        Artifact('bootloader', Path(args.bootloader), 'bootloader.bin', 0x0000, 'Bootloader'),
        Artifact('partitionTable', Path(args.partition_table), 'partition-table.bin', 0x8000, 'Partition table'),
        Artifact('app', Path(args.app_binary), 'hotas-bridge.bin', 0x10000, 'Application'),
    ]

    for artifact in artifacts:
        if not artifact.source.exists():
            raise SystemExit(f'Missing artifact: {artifact.source}')

    staged = stage_artifacts(release_dir, artifacts)

    esp_manifest = {
        'name': args.name,
        'version': args.version,
        'new_install_prompt_erase': True,
        'builds': [
            {
                'chipFamily': args.chip_family,
                'parts': [
                    {'path': staged['bootloader']['path'], 'offset': staged['bootloader']['offset']},
                    {'path': staged['partitionTable']['path'], 'offset': staged['partitionTable']['offset']},
                    {'path': staged['app']['path'], 'offset': staged['app']['offset']},
                ],
            }
        ],
    }
    write_json(release_dir / 'manifest.json', esp_manifest)

    checksums = {
        'bootloader': staged['bootloader']['sha256'],
        'partitionTable': staged['partitionTable']['sha256'],
        'app': staged['app']['sha256'],
    }
    write_json(release_dir / 'checksums.json', checksums)

    latest = {
        'channel': args.channel,
        'version': args.version,
        'build': args.build,
        'chipFamily': args.chip_family,
        'publishedAt': args.published_at,
        'releaseUrl': args.release_url,
        'releaseNotes': args.release_notes,
        'manifestPath': f'../releases/{args.version}/manifest.json',
        'artifacts': staged,
        'checksums': checksums,
    }
    write_json(latest_dir / 'latest.json', latest)

    sha_lines = [
        f"{staged['bootloader']['sha256']}  bootloader.bin",
        f"{staged['partitionTable']['sha256']}  partition-table.bin",
        f"{staged['app']['sha256']}  hotas-bridge.bin",
    ]
    (release_dir / 'SHA256SUMS.txt').write_text('\n'.join(sha_lines) + '\n', encoding='utf-8')

    print(f'Generated firmware release manifest under {release_dir}')
    print(f'Updated channel feed at {latest_dir / "latest.json"}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
