#!/usr/bin/env python3
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path


def fail(message: str) -> int:
    print(f"ERROR: {message}", file=sys.stderr)
    return 1


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def extract_function_body(source: str, signature: str) -> str:
    pattern = re.compile(rf"{re.escape(signature)}\s*\{{(?P<body>.*?)\n\}}", re.S)
    match = pattern.search(source)
    if not match:
        raise RuntimeError(f"Could not find function body for {signature}")
    return match.group("body")


def check_macos_layout_policy(gui_c_path: Path) -> int:
    source = read_text(gui_c_path)
    width_body = extract_function_body(source, "static int gui_layout_width(void)")
    height_body = extract_function_body(source, "static int gui_layout_height(void)")

    if "#if defined(__APPLE__)" not in width_body:
        return fail("gui_layout_width() is missing __APPLE__ guard")
    if "#if defined(__APPLE__)" not in height_body:
        return fail("gui_layout_height() is missing __APPLE__ guard")
    if "GetScreenWidth();" not in width_body:
        return fail("gui_layout_width() must use GetScreenWidth() on macOS")
    if "GetScreenHeight();" not in height_body:
        return fail("gui_layout_height() must use GetScreenHeight() on macOS")
    if "GetRenderWidth();" not in width_body:
        return fail("gui_layout_width() must use GetRenderWidth() for non-macOS")
    if "GetRenderHeight();" not in height_body:
        return fail("gui_layout_height() must use GetRenderHeight() for non-macOS")
    return 0


def extract_apprun_script(workflow_text: str) -> str:
    marker = "cat > AppDir/AppRun <<'EOF'"
    start = workflow_text.find(marker)
    if start < 0:
        raise RuntimeError("Could not find AppRun heredoc start marker in workflow")
    start = workflow_text.find("\n", start)
    if start < 0:
        raise RuntimeError("Malformed AppRun heredoc in workflow")
    start += 1
    end = workflow_text.find("\n          EOF", start)
    if end < 0:
        raise RuntimeError("Could not find AppRun heredoc end marker in workflow")
    return textwrap.dedent(workflow_text[start:end]).lstrip("\n")


def run_checked(command: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=True, capture_output=True, text=True, env=env)


def check_apprun_script(workflow_path: Path, icon_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    script = extract_apprun_script(workflow_text)

    with tempfile.TemporaryDirectory(prefix="misrc_ci_guard_") as temp_root:
        root = Path(temp_root)
        appdir = root / "AppDir"
        (appdir / "usr/bin").mkdir(parents=True, exist_ok=True)

        apprun_path = appdir / "AppRun"
        apprun_path.write_text(script, encoding="utf-8")
        apprun_path.chmod(apprun_path.stat().st_mode | stat.S_IXUSR)

        run_checked(["bash", "-n", str(apprun_path)])

        for exe in ("misrc_gui", "misrc_capture", "misrc_extract"):
            exe_path = appdir / "usr/bin" / exe
            exe_path.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            exe_path.chmod(exe_path.stat().st_mode | stat.S_IXUSR)

        if icon_path.exists():
            shutil.copy2(icon_path, appdir / "misrc.png")
        else:
            (appdir / "misrc.png").write_bytes(b"\x89PNG\r\n\x1a\n")

        appimage_path = root / "MISRC Test Build.AppImage"
        appimage_path.write_text("fake", encoding="utf-8")
        appimage_path.chmod(appimage_path.stat().st_mode | stat.S_IXUSR)

        home = root / "home"
        desktop_dir = home / "Desktop"
        desktop_dir.mkdir(parents=True, exist_ok=True)

        env = os.environ.copy()
        env["HOME"] = str(home)
        env["APPIMAGE"] = str(appimage_path)
        env.pop("XDG_DATA_HOME", None)

        run_checked(["bash", str(apprun_path), "--create-shortcut"], env=env)

        launcher_path = home / ".local/share/applications/misrc_gui.desktop"
        desktop_shortcut_path = home / "Desktop/MISRC GUI.desktop"
        icon_install_path = home / ".local/share/icons/hicolor/512x512/apps/misrc.png"

        if not launcher_path.exists():
            return fail("AppRun --create-shortcut did not create launcher file")
        if not desktop_shortcut_path.exists():
            return fail("AppRun --create-shortcut did not create Desktop shortcut")
        if not icon_install_path.exists():
            return fail("AppRun --create-shortcut did not install icon")

        launcher = read_text(launcher_path)
        expected_exec = f'Exec="{appimage_path}" %U'
        if expected_exec not in launcher:
            return fail(f"Launcher Exec entry mismatch. Expected: {expected_exec}")
        for required in ("Icon=misrc", "Terminal=false", "StartupWMClass=misrc_gui", "StartupNotify=true"):
            if required not in launcher:
                return fail(f"Launcher is missing required key: {required}")

        run_checked(["bash", str(apprun_path), "--smoke-test"], env=env)

    return 0


def check_windows_packaging_assertions(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "test \"$(find dist -maxdepth 1 -type f | wc -l)\" -eq 3",
        "test \"$(find dist -maxdepth 1 -name '*.exe' | wc -l)\" -eq 3",
        "objdump -p dist/misrc_gui.exe",
        "test \"$(objdump -p dist/misrc_gui.exe | awk '/^Subsystem[[:space:]]/ {print $2; exit}')\" = \"00000002\"",
        "assert_no_nonsystem_dlls()",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required Windows packaging assertion: {snippet}")
    return 0


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    workflow_path = repo_root / ".github/workflows/release-sanity-build.yml"
    gui_c_path = repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c"
    icon_path = repo_root / "assets/Icons/MISRC_Icon.png"

    checks = [
        ("macOS layout policy", lambda: check_macos_layout_policy(gui_c_path)),
        ("AppRun shortcut behavior", lambda: check_apprun_script(workflow_path, icon_path)),
        ("Windows packaging assertions", lambda: check_windows_packaging_assertions(workflow_path)),
    ]

    for name, check in checks:
        rc = check()
        if rc != 0:
            print(f"FAILED: {name}", file=sys.stderr)
            return rc
        print(f"PASS: {name}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
