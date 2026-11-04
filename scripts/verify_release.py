#!/usr/bin/env python3
"""Read-only integrity and compatibility inspection of a local iOS IPA.

Usage:
    python3 scripts/verify_release.py /path/to/app.ipa

No external dependencies. Does not sign, modify or install the application.
"""
import hashlib
import plistlib
import sys
import zipfile
from pathlib import Path

KNOWN_UNSIGNED_NAME = "DownhillRevived-iOS-0.7.9-build-70902-unsigned.ipa"
KNOWN_UNSIGNED_SHA256 = "4181572bd7b78ce87e956d52309e0879b3230f82a7511d325a6abd2b50adcc4c"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fd:
        for chunk in iter(lambda: fd.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    if len(sys.argv) != 2:
        print("Uso / Usage: python3 scripts/verify_release.py arquivo.ipa")
        return 2
    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"Arquivo nao encontrado / File not found: {path}")
        return 2
    digest = sha256(path)
    print(f"Arquivo / File: {path.name}")
    print(f"Tamanho / Size: {path.stat().st_size:,} bytes")
    print(f"SHA-256: {digest}")
    if path.name == KNOWN_UNSIGNED_NAME:
        print("Checksum oficial deste arquivo: " + ("CONFERE" if digest == KNOWN_UNSIGNED_SHA256 else "NAO CONFERE"))
        if digest != KNOWN_UNSIGNED_SHA256:
            return 1
    with zipfile.ZipFile(path) as ipa:
        files = ipa.namelist()
        plists = [name for name in files if name.startswith("Payload/") and name.endswith(".app/Info.plist")]
        if len(plists) != 1:
            print("Erro: nao foi possivel identificar um unico Info.plist do app no Payload.")
            return 1
        root = plists[0].removesuffix("Info.plist")
        info = plistlib.loads(ipa.read(plists[0]))
        print("Versao / Version:", info.get("CFBundleShortVersionString", "?"))
        print("Build:", info.get("CFBundleVersion", "?"))
        print("Minimo iOS / Minimum iOS:", info.get("MinimumOSVersion", "?"))
        print("Bundle ID:", info.get("CFBundleIdentifier", "?"))
        print("Capacidades / Capabilities:", ", ".join(info.get("UIRequiredDeviceCapabilities", [])))
        print("Familias / Device families:", info.get("UIDeviceFamily", []))
        profile = root + "embedded.mobileprovision"
        print("Perfil de provisionamento embutido:", "SIM" if profile in files else "NAO")
        sig_files = [n for n in files if n.startswith(root + "_CodeSignature/")]
        print("Arquivos CodeSignature:", "SIM" if sig_files else "NAO")
        if profile not in files and not sig_files:
            print("Aviso: nao ha indicios de assinatura/provisionamento no pacote. Nao instale como se estivesse assinado.")
        else:
            print("Aviso: a presenca de arquivos de assinatura NAO comprova validade ou direito de instalacao.")
            print("Confirme a assinatura no macOS com codesign e as ferramentas de desenvolvimento Apple.")
        if info.get("MinimumOSVersion") != "17.0":
            print("Observacao: versao minima diferente da versao analisada originalmente (17.0).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
