"""Fetch exact Ubuntu source publications and verify their DSC SHA-256 inventories.

BSD-3-Clause. Downloads stay in the project cache; system apt configuration is untouched.
"""

from __future__ import annotations

import concurrent.futures
import email.parser
import json
import urllib.parse
import urllib.request
from pathlib import Path

from blender_distribution import require, sha256


def control(text):
    if text.startswith("-----BEGIN PGP SIGNED MESSAGE-----"):
        text = text.split("\n\n", 1)[1].split("-----BEGIN PGP SIGNATURE-----", 1)[0]
        text = "\n".join(line.removeprefix("- ") for line in text.splitlines())
    return email.parser.Parser().parsestr(text)


def download(url, target):
    if not target.exists():
        temporary = target.with_suffix(target.suffix + ".partial")
        with (
            urllib.request.urlopen(url, timeout=180) as response,
            temporary.open("wb") as stream,
        ):
            import shutil

            shutil.copyfileobj(response, stream)
        temporary.replace(target)


def collect_one(name, version, cache):
    folder = cache / (name + "-" + version.replace(":", "_"))
    folder.mkdir(parents=True, exist_ok=True)
    receipt = folder / "receipt.json"
    if receipt.exists():
        info = json.loads(receipt.read_text())
        require(
            info["package"] == name and info["version"] == version,
            "Ubuntu receipt version mismatch",
        )
        for row in info["files"]:
            require(
                sha256(folder / row["file"]) == row["sha256"],
                "Ubuntu source cache changed: " + row["file"],
            )
        return info
    query = urllib.parse.urlencode(
        {
            "ws.op": "getPublishedSources",
            "source_name": name,
            "version": version,
            "exact_match": "true",
        }
    )
    api = "https://api.launchpad.net/1.0/ubuntu/+archive/primary?" + query
    with urllib.request.urlopen(api, timeout=90) as stream:
        result = json.load(stream)
    require(
        result["entries"], f"No official Ubuntu source publication: {name}={version}"
    )
    publication = result["entries"][0]
    require(
        publication["source_package_name"] == name
        and publication["source_package_version"] == version,
        "Launchpad returned a different source version",
    )
    with urllib.request.urlopen(
        publication["self_link"] + "?ws.op=sourceFileUrls", timeout=90
    ) as stream:
        urls = json.load(stream)
    dsc_url = next(url for url in urls if url.endswith(".dsc"))
    dsc = folder / Path(urllib.parse.urlparse(dsc_url).path).name
    download(dsc_url, dsc)
    metadata = control(dsc.read_text())
    require(
        metadata["Source"] == name and metadata["Version"] == version,
        "DSC source identity mismatch",
    )
    files = [
        {
            "file": dsc.name,
            "url": dsc_url,
            "sha256": sha256(dsc),
            "size": dsc.stat().st_size,
        }
    ]
    for line in metadata["Checksums-Sha256"].strip().splitlines():
        expected, size, filename = line.split()
        require(
            Path(filename).name == filename and filename not in (".", ".."),
            "Unsafe DSC source filename",
        )
        url = next(
            url
            for url in urls
            if Path(urllib.parse.urlparse(url).path).name == filename
        )
        target = folder / filename
        download(url, target)
        require(
            target.stat().st_size == int(size) and sha256(target) == expected,
            f"DSC source checksum mismatch: {filename}",
        )
        files.append(
            {"file": filename, "url": url, "sha256": expected, "size": int(size)}
        )
    info = {
        "package": name,
        "version": version,
        "publication": publication["self_link"],
        "folder": folder.name,
        "files": files,
    }
    receipt.write_text(json.dumps(info, indent=2) + "\n")
    print(f"Verified Ubuntu source {name}={version}", flush=True)
    return info


def collect(sources, cache):
    cache.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        pending = [
            pool.submit(collect_one, name, version, cache)
            for name, version in sorted(sources)
        ]
        return [future.result() for future in pending]


def verify_receipts(records, cache):
    for row in records:
        folder = cache / row["folder"]
        dsc = next(item for item in row["files"] if item["file"].endswith(".dsc"))
        metadata = control((folder / dsc["file"]).read_text())
        require(
            metadata["Source"] == row["package"]
            and metadata["Version"] == row["version"],
            "Wrong source version in cached DSC",
        )
        checksums = {
            line.split()[2]: line.split()[0]
            for line in metadata["Checksums-Sha256"].strip().splitlines()
        }
        for item in row["files"]:
            require(
                sha256(folder / item["file"]) == item["sha256"],
                "Ubuntu source bytes changed",
            )
            if item is not dsc:
                require(
                    checksums.get(item["file"]) == item["sha256"],
                    "Ubuntu receipt disagrees with DSC",
                )
