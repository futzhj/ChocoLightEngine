#!/usr/bin/env python3
"""ChocoLight Mobile Pack Tool — Inject Lua scripts into pre-built APK / iOS .app

Usage:
    # Android: replace main.lua inside a template APK
    python lightpack_mobile.py apk template.apk my_game.lua -o my_game.apk

    # iOS: replace main.lua inside a template .app bundle
    python lightpack_mobile.py ios ChocoLight.app my_game.lua -o MyGame.app

    # Inject multiple Lua files
    python lightpack_mobile.py apk template.apk game/ -o my_game.apk

    # Also supports injecting additional asset files
    python lightpack_mobile.py apk template.apk main.lua --assets images/ -o my_game.apk

Flow (APK):
    1. Copy template APK
    2. Replace/add Lua scripts in assets/ directory inside the ZIP
    3. Zip-align and re-sign with debug keystore (or specified keystore)

Flow (iOS .app):
    1. Copy template .app bundle
    2. Replace/add Lua scripts in the bundle root (Resources)
    3. (Optional) Re-sign with codesign identity

Requirements:
    - APK: Java keytool + apksigner (from Android SDK build-tools), or jarsigner
    - iOS: codesign (macOS only, optional for unsigned dev builds)
"""

import sys, os, shutil, zipfile, subprocess, argparse, tempfile, glob


# ============================================================
# APK Packing
# ============================================================

def find_build_tools():
    """Try to locate Android SDK build-tools for zipalign/apksigner"""
    sdk = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if not sdk:
        # Common locations
        candidates = [
            os.path.expanduser('~/Android/Sdk'),
            os.path.expanduser('~/Library/Android/sdk'),
            r'C:\Users\%USERNAME%\AppData\Local\Android\Sdk',
            r'C:\Android\sdk',
        ]
        for c in candidates:
            c = os.path.expandvars(c)
            if os.path.isdir(c):
                sdk = c
                break

    if not sdk:
        return None, None

    bt_dir = os.path.join(sdk, 'build-tools')
    if not os.path.isdir(bt_dir):
        return None, None

    # Pick latest version
    versions = sorted(os.listdir(bt_dir), reverse=True)
    if not versions:
        return None, None

    bt = os.path.join(bt_dir, versions[0])
    zipalign = os.path.join(bt, 'zipalign' + ('.exe' if sys.platform == 'win32' else ''))
    apksigner = os.path.join(bt, 'apksigner' + ('.bat' if sys.platform == 'win32' else ''))

    return (zipalign if os.path.isfile(zipalign) else None,
            apksigner if os.path.isfile(apksigner) else None)


def create_debug_keystore(ks_path):
    """Create a debug keystore for signing"""
    print(f"[lightpack_mobile] Creating debug keystore: {ks_path}")
    subprocess.run([
        'keytool', '-genkeypair',
        '-keystore', ks_path,
        '-alias', 'chocolight',
        '-keyalg', 'RSA', '-keysize', '2048',
        '-validity', '10000',
        '-storepass', 'chocolight',
        '-keypass', 'chocolight',
        '-dname', 'CN=ChocoLight,O=ChocoLight,C=US',
    ], capture_output=True)


def collect_lua_files(source):
    """Collect Lua files from a file or directory, returns list of (src_path, assets_relative_path)"""
    files = []
    if os.path.isfile(source):
        files.append((source, os.path.basename(source)))
    elif os.path.isdir(source):
        for root, _, fnames in os.walk(source):
            for fn in fnames:
                if fn.endswith(('.lua', '.luac')):
                    full = os.path.join(root, fn)
                    rel = os.path.relpath(full, source)
                    files.append((full, rel))
    else:
        print(f"Error: source not found: {source}")
        sys.exit(1)
    return files


def pack_apk(template_apk, lua_source, output_apk, assets_dirs=None, keystore=None):
    """Inject Lua scripts into template APK"""
    if not os.path.isfile(template_apk):
        print(f"Error: template APK not found: {template_apk}")
        sys.exit(1)

    lua_files = collect_lua_files(lua_source)
    if not lua_files:
        print(f"Error: no Lua files found in: {lua_source}")
        sys.exit(1)

    # Collect additional asset files
    asset_files = []
    for ad in (assets_dirs or []):
        if os.path.isdir(ad):
            for root, _, fnames in os.walk(ad):
                for fn in fnames:
                    full = os.path.join(root, fn)
                    rel = os.path.relpath(full, ad)
                    asset_files.append((full, rel))

    # Work in temp directory
    with tempfile.TemporaryDirectory() as tmpdir:
        work_apk = os.path.join(tmpdir, 'work.apk')
        shutil.copy2(template_apk, work_apk)

        # APK is a ZIP — open and replace/add files
        # We need to recreate the ZIP since zipfile doesn't support in-place replacement
        final_apk = os.path.join(tmpdir, 'unsigned.apk')

        with zipfile.ZipFile(work_apk, 'r') as zin:
            with zipfile.ZipFile(final_apk, 'w', zipfile.ZIP_DEFLATED) as zout:
                # Copy existing entries, skipping files we'll replace
                replace_names = set()
                for src, rel in lua_files:
                    replace_names.add(f'assets/{rel}')
                for src, rel in asset_files:
                    replace_names.add(f'assets/{rel}')

                # Also skip META-INF (signatures) — we'll re-sign
                for item in zin.infolist():
                    if item.filename.startswith('META-INF/'):
                        continue
                    if item.filename in replace_names:
                        continue
                    data = zin.read(item.filename)
                    zout.writestr(item, data)

                # Add Lua files
                for src, rel in lua_files:
                    arcname = f'assets/{rel}'
                    with open(src, 'rb') as f:
                        zout.writestr(arcname, f.read())
                    print(f"  + {arcname}")

                # Add extra asset files
                for src, rel in asset_files:
                    arcname = f'assets/{rel}'
                    with open(src, 'rb') as f:
                        zout.writestr(arcname, f.read())
                    print(f"  + {arcname} (asset)")

        # Zipalign
        zipalign, apksigner = find_build_tools()
        aligned_apk = os.path.join(tmpdir, 'aligned.apk')

        if zipalign:
            print("[lightpack_mobile] Zip-aligning...")
            ret = subprocess.run([zipalign, '-f', '4', final_apk, aligned_apk],
                                 capture_output=True, text=True)
            if ret.returncode != 0:
                print(f"  Warning: zipalign failed ({ret.stderr}), skipping")
                shutil.copy2(final_apk, aligned_apk)
        else:
            print("[lightpack_mobile] zipalign not found, skipping alignment")
            shutil.copy2(final_apk, aligned_apk)

        # Sign
        signed_apk = os.path.join(tmpdir, 'signed.apk')
        ks = keystore
        if not ks:
            ks = os.path.join(tmpdir, 'debug.keystore')
            create_debug_keystore(ks)

        if apksigner:
            print("[lightpack_mobile] Signing with apksigner...")
            ret = subprocess.run([
                apksigner, 'sign',
                '--ks', ks,
                '--ks-pass', 'pass:chocolight',
                '--key-pass', 'pass:chocolight',
                '--ks-key-alias', 'chocolight',
                '--out', signed_apk,
                aligned_apk,
            ], capture_output=True, text=True)
            if ret.returncode != 0:
                print(f"  Warning: apksigner failed ({ret.stderr}), trying jarsigner")
                _jarsigner_sign(aligned_apk, signed_apk, ks)
        else:
            _jarsigner_sign(aligned_apk, signed_apk, ks)

        # Copy to output
        if os.path.isfile(signed_apk):
            shutil.copy2(signed_apk, output_apk)
        else:
            # Fallback: use unsigned
            print("[lightpack_mobile] Warning: signing failed, output is unsigned")
            shutil.copy2(aligned_apk, output_apk)

    _print_result(template_apk, output_apk, lua_files)


def _jarsigner_sign(input_apk, output_apk, keystore):
    """Fallback signing with jarsigner"""
    print("[lightpack_mobile] Signing with jarsigner...")
    shutil.copy2(input_apk, output_apk)
    ret = subprocess.run([
        'jarsigner',
        '-keystore', keystore,
        '-storepass', 'chocolight',
        '-keypass', 'chocolight',
        output_apk, 'chocolight',
    ], capture_output=True, text=True)
    if ret.returncode != 0:
        print(f"  Warning: jarsigner failed: {ret.stderr}")


# ============================================================
# iOS .app Packing
# ============================================================

def pack_ios(template_app, lua_source, output_app, sign_identity=None, assets_dirs=None):
    """Inject Lua scripts into template .app bundle"""
    if not os.path.isdir(template_app) and not os.path.isfile(template_app):
        # Maybe it's a .tar.gz or .zip?
        if template_app.endswith(('.zip', '.tar.gz', '.tgz')):
            return pack_ios_archive(template_app, lua_source, output_app, sign_identity, assets_dirs)
        print(f"Error: template .app not found: {template_app}")
        sys.exit(1)

    lua_files = collect_lua_files(lua_source)
    if not lua_files:
        print(f"Error: no Lua files found in: {lua_source}")
        sys.exit(1)

    # Copy template .app bundle
    if os.path.exists(output_app):
        shutil.rmtree(output_app)
    shutil.copytree(template_app, output_app)

    # Replace/add Lua files in bundle root
    for src, rel in lua_files:
        dst = os.path.join(output_app, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        print(f"  + {rel}")

    # Inject additional asset files into bundle
    for ad in (assets_dirs or []):
        if os.path.isdir(ad):
            for root, _, fnames in os.walk(ad):
                for fn in fnames:
                    full = os.path.join(root, fn)
                    rel = os.path.relpath(full, ad)
                    dst = os.path.join(output_app, rel)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(full, dst)
                    print(f"  + {rel} (asset)")

    # Re-sign if on macOS and identity provided
    if sign_identity and sys.platform == 'darwin':
        print(f"[lightpack_mobile] Re-signing with: {sign_identity}")
        ret = subprocess.run(['codesign', '--force', '--sign', sign_identity, output_app],
                             capture_output=True, text=True)
        if ret.returncode != 0:
            print(f"  Warning: codesign failed: {ret.stderr}")

    _print_result_ios(template_app, output_app, lua_files)


def pack_ios_archive(archive_path, lua_source, output_app, sign_identity=None, assets_dirs=None):
    """Extract .app from archive, inject, and repack"""
    with tempfile.TemporaryDirectory() as tmpdir:
        # Extract
        if archive_path.endswith('.zip'):
            import zipfile
            with zipfile.ZipFile(archive_path, 'r') as z:
                z.extractall(tmpdir)
        elif archive_path.endswith(('.tar.gz', '.tgz')):
            import tarfile
            with tarfile.open(archive_path, 'r:gz') as t:
                t.extractall(tmpdir)

        # Find .app bundle
        apps = glob.glob(os.path.join(tmpdir, '**', '*.app'), recursive=True)
        if not apps:
            print("Error: no .app bundle found in archive")
            sys.exit(1)

        template = apps[0]
        pack_ios(template, lua_source, output_app, sign_identity, assets_dirs)


# ============================================================
# IPA Packing (generates installable .ipa from .app)
# ============================================================

def pack_ipa(app_path, output_ipa):
    """Create IPA from .app bundle"""
    if not os.path.isdir(app_path):
        print(f"Error: .app not found: {app_path}")
        sys.exit(1)

    with tempfile.TemporaryDirectory() as tmpdir:
        payload = os.path.join(tmpdir, 'Payload')
        os.makedirs(payload)
        app_name = os.path.basename(app_path)
        shutil.copytree(app_path, os.path.join(payload, app_name))

        with zipfile.ZipFile(output_ipa, 'w', zipfile.ZIP_DEFLATED) as z:
            for root, dirs, files in os.walk(payload):
                for fn in files:
                    full = os.path.join(root, fn)
                    arcname = os.path.relpath(full, tmpdir)
                    z.write(full, arcname)

    print(f"[lightpack_mobile] IPA created: {output_ipa} ({os.path.getsize(output_ipa):,} bytes)")


# ============================================================
# Helpers
# ============================================================

def _print_result(template, output, lua_files):
    tsize = os.path.getsize(template)
    osize = os.path.getsize(output)
    print(f"\n[lightpack_mobile] ✅ Packed successfully!")
    print(f"  Template:    {template} ({tsize:,} bytes)")
    print(f"  Output:      {output} ({osize:,} bytes)")
    print(f"  Lua scripts: {len(lua_files)} file(s)")
    for _, rel in lua_files:
        print(f"    - {rel}")


def _print_result_ios(template, output, lua_files):
    print(f"\n[lightpack_mobile] ✅ iOS app packed successfully!")
    print(f"  Template:    {template}")
    print(f"  Output:      {output}")
    print(f"  Lua scripts: {len(lua_files)} file(s)")
    for _, rel in lua_files:
        print(f"    - {rel}")


# ============================================================
# CLI
# ============================================================

def main():
    parser = argparse.ArgumentParser(
        description='ChocoLight Mobile Pack Tool — Inject Lua into APK/IPA/.app without recompiling',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Replace main.lua in APK
  python lightpack_mobile.py apk template.apk my_game.lua -o my_game.apk

  # Inject a directory of Lua files
  python lightpack_mobile.py apk template.apk scripts/ -o my_game.apk

  # Inject into iOS .app
  python lightpack_mobile.py ios ChocoLight.app main.lua -o MyGame.app

  # Create IPA from .app
  python lightpack_mobile.py ipa MyGame.app -o MyGame.ipa

  # Inject + extra assets
  python lightpack_mobile.py apk template.apk main.lua --assets images/ sounds/ -o game.apk
""")

    sub = parser.add_subparsers(dest='platform', required=True)

    # APK subcommand
    p_apk = sub.add_parser('apk', help='Pack Lua into Android APK')
    p_apk.add_argument('template', help='Template APK path')
    p_apk.add_argument('scripts', help='Lua script file or directory')
    p_apk.add_argument('-o', '--output', required=True, help='Output APK path')
    p_apk.add_argument('--assets', nargs='*', help='Additional asset directories')
    p_apk.add_argument('--keystore', help='Custom keystore for signing')

    # iOS subcommand
    p_ios = sub.add_parser('ios', help='Pack Lua into iOS .app bundle')
    p_ios.add_argument('template', help='Template .app path (or .zip/.tar.gz)')
    p_ios.add_argument('scripts', help='Lua script file or directory')
    p_ios.add_argument('-o', '--output', required=True, help='Output .app path')
    p_ios.add_argument('--assets', nargs='*', help='Additional asset directories')
    p_ios.add_argument('--sign', help='Codesign identity (macOS only)')

    # IPA subcommand
    p_ipa = sub.add_parser('ipa', help='Create IPA from .app bundle')
    p_ipa.add_argument('app', help='.app bundle path')
    p_ipa.add_argument('-o', '--output', required=True, help='Output IPA path')

    args = parser.parse_args()

    if args.platform == 'apk':
        print(f"[lightpack_mobile] Packing APK: {args.template} + {args.scripts}")
        pack_apk(args.template, args.scripts, args.output, args.assets, args.keystore)

    elif args.platform == 'ios':
        print(f"[lightpack_mobile] Packing iOS: {args.template} + {args.scripts}")
        pack_ios(args.template, args.scripts, args.output, getattr(args, 'sign', None), args.assets)

    elif args.platform == 'ipa':
        print(f"[lightpack_mobile] Creating IPA from: {args.app}")
        pack_ipa(args.app, args.output)


if __name__ == '__main__':
    main()
