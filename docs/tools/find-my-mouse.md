# CursorFocus / Find My Mouse Design (find-my-mouse.c)

- No CLI. Active state is process lifetime + event loop.
- **First launch**: own mutex → create event → create spotlight → loop.
- **Second launch**: signal toggle → exit. Running instance treats toggle as deactivate → destroy overlay → exit.
- **Spotlight = two layers**:
  1. **Dim layer** `g_dim` — full virtual-screen window, uniform black-dim via `SetLayeredWindowAttributes(hwnd, 0, DIM_ALPHA, LWA_ALPHA)` (constant 160 → ~37% screen). No per-pixel buffer. Window class has `BLACK_BRUSH` background.
  2. **Glow layer** `g_glow` — a small `MAX_GLOW_SIZE` (= 512) per-pixel-alpha window. A 32-bpp top-down DIB (`CreateDIBSection`) holds a flat unblended white disc (`SPOT_ALPHA`=160) presented via `UpdateLayeredWindow(AC_SRC_OVER|AC_SRC_ALPHA)`.
- **Physical Size Consistency Across Monitors**:
  - Base radius at 96 DPI: `BASE_SPOT_RADIUS = 75` (steady-state disc), `BASE_PULSE_RADIUS_MAX = 130` (focus-in/out max radius).
  - Actively queries the DPI of the monitor containing the cursor via `TinyDPI_GetDpiForPoint(cur)`.
  - Calculates radius dynamically as `MulDiv(BASE_SPOT_RADIUS, dpi, 96)` so that physical diameter on screen remains constant (~1.56 inches / ~40 mm) whether on a 150%/200% high-density laptop screen or a 100% desktop monitor.
  - Dynamically redraws when the cursor moves across displays of different DPIs.
- **Activation (focus-in)**: on first launch a large faint disc centered on cursor shrinks (`RenderFocusInFrame`, ease-in quadratic 180 ms) to `g_spotRadius` while opacity ramps to `SPOT_ALPHA`, then settles into steady disc.
- **Dismissal (focus-out)**: click/key or toggle-off plays animation in reverse (`RenderFocusOutFrame`, 210 ms — disc grows back to `g_pulseRadiusMax` while fading to 0), then process exits.
- **Positioning**: glow window is placed only by `MoveGlow` (tracks `g_glowX/g_glowY`); `PresentGlow` passes that position as `dst` to `UpdateLayeredWindow`.
- **Loop** (16 ms): pump messages; `GetCursorPos`; move glow center only when cursor position changes.
- **Dismissal**: mouse click or key press starts focus-out fade and process exits (detected via `GetAsyncKeyState` over VK 1–254 with 250 ms grace period `ACTIVATE_GRACE_MS`).
- `TinyDPI_EnablePerMonitorAwareness()` ensures overlay coordinates match physical pixels 1:1.

---

## Verified

- [x] IPC `Global\TinyCursorFocusMutex/Event`; 2nd launch toggles + exits 0 (2026-08-12)
- [x] Launch: mutex → event → dim+glow → loop; deactivate → destroy → exit
- [x] 16 ms loop; move glow only on cursor change
- [x] Layers: uniform-alpha dim (160) + per-pixel DIB spot (r 100, alpha 160)
- [x] Anim: focus-in 400 ms ease-in → steady; focus-out 350 ms ease-out → exit (2026-08-12)
- [x] Any click/key exits after 250 ms grace (`GetAsyncKeyState`, no hook)
- [x] GDI only; WS ≈ 7.3 MiB, private ≈ 1.6 MiB (2026-08-13)
- [x] DPI: radius `MulDiv(BASE_SPOT_RADIUS, dpi, 96)` ≈ 40 mm on any display; rescale at DPI change (2026-08-25)
- [x] `--help`: 9 lines, exit 0 (2026-10-01)
- [ ] CPU idle not re-measured (event-wait, expected < 0.2%)
- [ ] Visual click-through check in a real session
