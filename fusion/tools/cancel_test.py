"""Test Cancel on the import progress dialog the way a user would: start an import job, wait, then press the
dialog's Cancel button (found on screen), and report the job's reply and the documents left open.

  python fusion/tools/cancel_test.py <file.ncx.step> [seconds before cancelling, default 20] [out.png]

Needs pywin32 and Pillow (developer tool). The dialog is found by its title "Import NCX: <file>"; the Cancel button
is pressed with Escape first (the dialog's cancel key), then, if the import is still running, by a click on the
button (bottom right of the dialog).
"""
import json
import os
import sys
import time
import uuid

import win32api
import win32con
import win32gui

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fjob  # noqa: E402


def find_dialog():
    found = []

    def cb(h, _):
        t = win32gui.GetWindowText(h)
        if t.startswith("Import NCX") and win32gui.IsWindowVisible(h):
            found.append(h)
    win32gui.EnumWindows(cb, None)
    return found[0] if found else None


def main(argv):
    f = os.path.abspath(argv[0])
    wait = float(argv[1]) if len(argv) > 1 else 20
    png = argv[2] if len(argv) > 2 else None
    os.makedirs(fjob.JOBS, exist_ok=True)
    name = time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:6]
    tmp = os.path.join(fjob.JOBS, name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump({"cmd": "import", "file": f, "close": True}, fh)
    os.replace(tmp, os.path.join(fjob.JOBS, name + ".job.json"))
    res = os.path.join(fjob.JOBS, name + ".result.json")
    t0 = time.time()
    time.sleep(wait)
    h = find_dialog()
    log = {"dialogFound": bool(h), "pressedAt": round(time.time() - t0, 1)}
    if h:
        log["title"] = win32gui.GetWindowText(h)
        rect = win32gui.GetWindowRect(h)
        log["rect"] = rect
        if png:
            from PIL import ImageGrab
            ImageGrab.grab(bbox=rect).save(png)
        try:
            win32gui.SetForegroundWindow(h)
        except Exception as e:  # noqa: BLE001
            log["foreground"] = str(e)
        win32api.keybd_event(win32con.VK_ESCAPE, 0, 0, 0)
        win32api.keybd_event(win32con.VK_ESCAPE, 0, win32con.KEYEVENTF_KEYUP, 0)
        time.sleep(3)
        if not os.path.exists(res) and find_dialog():
            # click the Cancel button: bottom right of the dialog
            x, y = rect[2] - 60, rect[3] - 25
            win32api.SetCursorPos((x, y))
            win32api.mouse_event(win32con.MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
            win32api.mouse_event(win32con.MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
            log["clicked"] = (x, y)
    while not os.path.exists(res) and time.time() - t0 < 1200:
        time.sleep(0.5)
    time.sleep(0.2)
    with open(res, encoding="utf-8") as fh:
        reply = json.load(fh)
    os.remove(res)
    log["replyStatus"] = reply.get("status")
    log["cancelledAt"] = reply.get("cancelledAt")
    log["seconds"] = reply.get("seconds")
    log["reportItem"] = reply.get("report", [{}])[0] if reply.get("status") == "cancelled" else None
    log["documentsAfter"] = fjob.run({"cmd": "docs"}).get("documents")
    print(json.dumps(log, indent=1))


if __name__ == "__main__":
    main(sys.argv[1:])
