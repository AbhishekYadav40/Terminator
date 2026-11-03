# GUI tests (manual verification harness)

These scripts drive the real X11 window under Xvfb with synthetic key
events and assert on side-effect files. They were used to verify the
GUI behaviours that --batch cannot reach (hotkeys, tabs, Ctrl+C/Z,
Ctrl+R, completion, process-mode tab isolation).

Build the helpers and run:

    gcc -O2 -o keys-bin keys.c -lX11
    gcc -O2 -o shot-bin shot.c -lX11
    bash run_gui_tests.sh thread
    bash run_gui_tests.sh process useProcess=True
    bash mouse_test.sh thread
    bash mouse_test.sh process useProcess=True

(The scripts build keys-bin/shot-bin themselves when missing. `keys`
also accepts `click:<x>,<y>` for tab-bar mouse clicks,
`drag:<x1>,<y1>,<x2>,<y2>` for a Button1 drag (text selection), and
`wheel:up|down` for mouse-wheel scrolling. Dragging in the text area
selects the output text (inverted highlight); Ctrl+Shift+C copies the
selection, Ctrl+Shift+V pastes it. Typing goes through
`send_char`, which resolves each character against the server's
keymap instead of assuming Shift+keysym: on Xvfb's fallback keymap
`XKeysymToKeycode(XK_less)` + Shift arrives as `>`, so `<` would
never type correctly without the keymap scan.)

`shot-bin out.ppm` dumps the window as a PPM for visual inspection.
