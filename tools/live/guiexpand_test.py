"""Live driver for stellaris-guiexpand: stages plugin folders, injects the host and the example plugins into a running game, drives the host's development
commands and takes screenshots. Author's tooling: it uses the bench scripts of the stellaris-perf repository (restart on a save, find the game
window) and the launcher's `stl inject`. Set the paths below (environment variables) for another machine.

    GUIEXPAND_BENCH_SCRIPTS  folder with game_session.py and benchlib.py   (default D:\\stellaris-perf\\bench\\scripts)
    GUIEXPAND_STL            stl.exe of the launcher                          (default D:\\stellaris-Launcher\\target\\release\\stl.exe)
    GUIEXPAND_RUN            scratch folder for the staged plugin folders     (default <repo>\\run)
    GUIEXPAND_SAVE           "<save name>,<folder>"                           (default fmbase,11_638438808)

    python tools/live/guiexpand_test.py stage                    copy build/plugin/stellaris-guiexpand and the examples into the run folder (dev_commands on)
    python tools/live/guiexpand_test.py load                     restart the game on the test save (mods are read at start)
    python tools/live/guiexpand_test.py bench "pause 0" "speed 3"   commands for the bench DLL, to get ticks going before the ImGui is up
    python tools/live/guiexpand_test.py inject host|imgui|badcfg|c|element   inject one of the staged DLLs (examples first, host last or first: both orders work)
    python tools/live/guiexpand_test.py cmd "tab 3" "post guiexpand_test_grant_energy"   lines for the host's logs\\stellaris_guiexpand.cmd
    python tools/live/guiexpand_test.py excmd imgui "fault"      a line for an example's consumer_<tag>.cmd
    python tools/live/guiexpand_test.py log [n] | exlog <tag> [n]   tail of the host's / an example's log
    python tools/live/guiexpand_test.py shot out.png            screenshot of the game window's client area
    python tools/live/guiexpand_test.py unload host|imgui|badcfg|c|element  development unload by event (host needs dev_unload=1)
"""
import ctypes
import ctypes.wintypes as w
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BENCH = os.environ.get("GUIEXPAND_BENCH_SCRIPTS", r"D:\stellaris-perf\bench\scripts")
STL = os.environ.get("GUIEXPAND_STL", r"D:\stellaris-Launcher\target\release\stl.exe")
RUN = os.environ.get("GUIEXPAND_RUN", os.path.join(REPO, "run"))
SAVE = os.environ.get("GUIEXPAND_SAVE", "fmbase,11_638438808").split(",")
BUILD = os.path.join(REPO, "build")
sys.path.insert(0, BENCH)

DLLS = {  # name -> (built dll, staged folder, event tag, unload event prefix)
    "host": ("stellaris_guiexpand.dll", "stellaris-guiexpand", None, "Local\\stellaris_guiexpand_unload_"),
    "imgui": ("example_imgui.dll", "ex_imgui", "imgui", "Local\\gui_consumer_imgui_unload_"),
    "badcfg": ("example_imgui_badcfg.dll", "ex_badcfg", "badcfg", "Local\\gui_consumer_badcfg_unload_"),
    "c": ("example_c.dll", "ex_c", "c", "Local\\gui_consumer_c_unload_"),
    "element": ("example_element.dll", "ex_element", "element", "Local\\gui_element_unload_"),
}

user32 = ctypes.WinDLL("user32", use_last_error=True)
try:
    ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
except OSError:
    user32.SetProcessDPIAware()


def bench_modules():
    import game_session as gs
    from benchlib import game_pids, module_loaded
    return gs, game_pids, module_loaded


def one_game():
    _, game_pids, _ = bench_modules()
    pids = game_pids()
    if len(pids) != 1:
        raise SystemExit(f"expected exactly one stellaris.exe, found {pids}")
    return pids[0]


def stage():
    for name, (dll, folder, _, _) in DLLS.items():
        d = os.path.join(RUN, folder)
        os.makedirs(os.path.join(d, "config"), exist_ok=True)
        os.makedirs(os.path.join(d, "logs"), exist_ok=True)
        built = os.path.join(BUILD, "Release", dll)
        if not os.path.exists(built):
            raise SystemExit(f"{built} is missing: build first (tools\\build.bat)")
        shutil.copyfile(built, os.path.join(d, dll))
    ini = os.path.join(RUN, "stellaris-guiexpand", "config", "stellaris_guiexpand.ini")
    with open(ini, "w", encoding="utf-8") as f:
        f.write("[guiexpand]\ndeck=1\ndeck_open=0\ntheme=0\nstars=1\ndev_commands=1\ndev_unload=1\n")
    print("staged in", RUN)


def inject(which):
    _, _, module_loaded = bench_modules()
    pid = one_game()
    dll, folder, _, _ = DLLS[which]
    path = os.path.join(RUN, folder, dll)
    if module_loaded(pid, dll):
        raise SystemExit(f"{dll} is already loaded")
    r = subprocess.run([STL, "inject", path, "--pid", str(pid)], capture_output=True, text=True)
    print(r.stdout.strip(), r.stderr.strip())
    time.sleep(1.5)
    print("loaded:", module_loaded(pid, dll))


def unload(which):
    _, _, module_loaded = bench_modules()
    pid = one_game()
    dll, _, _, prefix = DLLS[which]
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.OpenEventW.restype = ctypes.c_void_p
    k.OpenEventW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
    k.SetEvent.argtypes = [ctypes.c_void_p]
    ev = k.OpenEventW(0x2, False, f"{prefix}{pid}")
    if not ev:
        raise SystemExit("unload event not found (not loaded, or built without it)")
    k.SetEvent(ev)
    for _ in range(80):
        if not module_loaded(pid, dll):
            print("unloaded")
            return
        time.sleep(0.2)
    print("still loaded")


def write_cmd(path, lines):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)
    for _ in range(80):  # the DLL deletes the file once it has run it
        if not os.path.exists(path):
            return
        time.sleep(0.1)
    print("warning: the command file was not picked up (DLL not drawing frames? ImGui not running?)")


def tail(path, n):
    if os.path.exists(path):
        for line in open(path, encoding="utf-8", errors="replace").read().splitlines()[-n:]:
            print(line)
    else:
        print("(no log yet:", path, ")")


def shot(out):
    from PIL import ImageGrab
    gs, _, _ = bench_modules()
    pid = one_game()
    hwnd = gs.game_window(pid)
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 0x2, 0)
    user32.ShowWindow(hwnd, 9)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.5)
    pt = w.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    c = w.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(c))
    vx, vy = user32.GetSystemMetrics(76), user32.GetSystemMetrics(77)
    ImageGrab.grab(all_screens=True).crop((pt.x - vx, pt.y - vy, pt.x - vx + c.right, pt.y - vy + c.bottom)).save(out)
    print(f"saved {out} ({c.right}x{c.bottom})")


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
    elif a[0] == "stage":
        stage()
    elif a[0] == "load":
        gs, _, _ = bench_modules()
        gs.load(SAVE[0], SAVE[1], 420)
    elif a[0] == "bench":
        from benchlib import Bench
        b = Bench(tries=50)
        for line in a[1:]:
            print(line, "->", b.cmd(line))
        b.close()
    elif a[0] == "inject":
        inject(a[1])
    elif a[0] == "unload":
        unload(a[1])
    elif a[0] == "cmd":
        write_cmd(os.path.join(RUN, "stellaris-guiexpand", "logs", "stellaris_guiexpand.cmd"), a[1:])
    elif a[0] == "excmd":
        write_cmd(os.path.join(RUN, DLLS[a[1]][1], f"consumer_{DLLS[a[1]][2]}.cmd"), a[2:])
    elif a[0] == "log":
        tail(os.path.join(RUN, "stellaris-guiexpand", "logs", "stellaris_guiexpand.log"), int(a[1]) if len(a) > 1 else 40)
    elif a[0] == "exlog":
        tail(os.path.join(RUN, DLLS[a[1]][1], f"consumer_{DLLS[a[1]][2]}.log"), int(a[2]) if len(a) > 2 else 30)
    elif a[0] == "shot":
        shot(a[1])
    else:
        print(__doc__)


main()
