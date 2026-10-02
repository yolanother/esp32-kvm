# Desktop shell implementation

The Tauri webview renders the five destinations in [DESIGN.md](../../design/DESIGN.md): Systems, Screen layout, Key mappings, Shortcuts and Device. A shared 190-pixel sidebar, header, disconnected status region and footer follow the S02–S06 base designs. `apps/desktop/src/style.css` contains the semantic dark tokens; text uses relative sizes so Windows text scaling can enlarge it.

The shell defaults to local control and an unavailable device connection. It does not infer that an attached COM port is a healthy device. The **Show design examples** checkbox exposes sample guest cards, monitor geometry and mapping rows from `preview-fixtures.ts`. Every sample is labeled as a design example and cannot be selected, saved, or sent to hardware. The Device page reports unknown firmware and unmeasured latency. Proposed shortcuts and the emergency chord are explicitly marked inactive until the native capture and routing work is integrated.

Navigation uses native buttons, `aria-current="page"`, a skip link, visible focus, Arrow/Home/End sidebar movement, and heading focus after pointer/button selection. The connection message uses a live status region. The page title is also announced after navigation. A narrow layout stacks cards and permits horizontal table scrolling; reduced-motion preferences are respected.

## Checks

From `apps/desktop`:

```powershell
& 'C:/Program Files/nodejs/node.exe' --experimental-strip-types --test src/navigation.test.mjs
& 'C:/Program Files/nodejs/npm.cmd' run build
```

The navigation test was red before `navigation.ts` existed and covers the five required destinations and boundary-key behavior. TypeScript and Vite production build pass. A local Vite server started successfully, but this session exposed no browser/app surface for visual or screen-reader inspection. Visual QA at 1120×760, minimum 900×640, high text scaling, keyboard-only navigation and screen-reader announcements remains a shared integration gate.

## Next integrations

The pairing wizard, live guest selection, profile editor, screen portal editor, shortcut recorder, diagnostics and tray actions belong to their own tasks. Those features must replace preview fixtures with versioned native data and firmware-confirmed readiness. The webview must never own timing-critical physical input capture or decide that a guest is controlling before a routing acknowledgment.
