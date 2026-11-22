#!/usr/bin/env python3
"""Generates a Butterscotch input-playback JSON from a tiny script.

Script lines (one per line, '#' comments):
  wait N            advance N frames
  tap KEY [N]       press+release KEY (N times, 6 frames apart)
  hold KEY N        hold KEY for N frames
Keys: Z X C ENTER UP DOWN LEFT RIGHT

Used for automated smoke tests on the PC (headless CLI build) and on the
Dreamcast (the runner plays back /cd/INPUT.JSN when the disc contains one).
"""
import json, sys

KEYS = {"Z": 90, "X": 88, "C": 67, "ENTER": 13, "UP": 38, "DOWN": 40, "LEFT": 37, "RIGHT": 39}

def build(script):
    frames, t = {}, 0
    def ev(f, kind, k):
        frames.setdefault(str(f), {"keysPressed": [], "keysReleased": []})[kind].append(k)
    for raw in script.splitlines():
        line = raw.split("#")[0].split()
        if not line: continue
        op = line[0].lower()
        if op == "wait":
            t += int(line[1])
        elif op == "tap":
            k = KEYS[line[1].upper()]; n = int(line[2]) if len(line) > 2 else 1
            for _ in range(n):
                ev(t, "keysPressed", k); ev(t + 2, "keysReleased", k); t += 6
        elif op == "hold":
            k = KEYS[line[1].upper()]; n = int(line[2])
            ev(t, "keysPressed", k); ev(t + n, "keysReleased", k); t += n + 1
        else:
            raise SystemExit("unknown op: " + op)
    return frames

if __name__ == "__main__":
    src = open(sys.argv[1]).read()
    json.dump(build(src), open(sys.argv[2], "w"))
