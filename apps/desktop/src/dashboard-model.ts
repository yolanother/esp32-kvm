// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Turns verified actor routing state into dashboard, tray and overlay labels.
// A ready BLE bond alone is never described as the active input recipient.
import type { SetupSnapshot, RouteState } from "./setup-model";

/** Text and tone for the only actor-confirmed input destination. */
export interface RouteDescription {
  title: string;
  detail: string;
  tone: "accent" | "warning" | "neutral";
}

/** A saved guest choice, including its non-actionable readiness. */
export interface SystemChoice {
  bondToken: string;
  name: string;
  state: "Controlling" | "Ready" | "Offline";
  selectEnabled: boolean;
}

/** A transient switch announcement or persistent local recovery alert. */
export interface SwitchAnnouncement {
  message: string;
  persistent: boolean;
}

/** Describes the active route without promoting a pending or ready guest. */
export function describeRoute(snapshot: SetupSnapshot): RouteDescription {
  switch (snapshot.route.kind) {
    case "guest": {
      const { bondToken, slot } = snapshot.route;
      const name = snapshot.profiles.find((guest) => guest.bondToken === bondToken)?.name ?? `Guest slot ${slot}`;
      return { title: name, detail: "Controlling this guest. Return to this computer with Ctrl+Alt+F10 when the native shortcut is available.", tone: "accent" };
    }
    case "switching": return { title: "Switch pending", detail: "Waiting for an exact device acknowledgement. Input is not yet assigned to the requested guest.", tone: "warning" };
    case "pairing": return { title: "This computer", detail: "Pairing is open. Input remains local.", tone: "neutral" };
    case "awaiting_status": return { title: "This computer", detail: "Firmware status is pending. Guest routing remains disarmed.", tone: "warning" };
    case "failed": return { title: "This computer", detail: `Local control restored after ${snapshot.route.reason.replaceAll("_", " ")} failure. Check the device connection.`, tone: "warning" };
    case "local": return { title: "This computer", detail: snapshot.device.kind === "verified" ? "Windows has local keyboard and mouse control." : "Input stays with this computer while the device is unverified.", tone: snapshot.device.kind === "verified" ? "accent" : "warning" };
  }
}

/** Lists saved guests while keeping selection gated by native capture support. */
export function systemChoices(snapshot: SetupSnapshot): SystemChoice[] {
  return snapshot.profiles.map((guest) => ({
    bondToken: guest.bondToken,
    name: guest.name,
    state: snapshot.route.kind === "guest" && snapshot.route.bondToken === guest.bondToken ? "Controlling" : snapshot.readyTokens.includes(guest.bondToken) ? "Ready" : "Offline",
    selectEnabled: false,
  }));
}

function routeKey(route: RouteState): string {
  return route.kind === "guest" ? `guest:${route.slot}:${route.bondToken}` : route.kind === "failed" ? `failed:${route.reason}` : route.kind;
}

/** Announces a confirmed change; failures persist until the user closes them. */
export function overlayForTransition(previous: SetupSnapshot | null, next: SetupSnapshot): SwitchAnnouncement | null {
  if (!previous || routeKey(previous.route) === routeKey(next.route)) return null;
  if (next.route.kind === "failed") return { message: describeRoute(next).detail, persistent: true };
  if (next.route.kind === "guest") return { message: `Now controlling ${describeRoute(next).title}. Return with Ctrl+Alt+F10 when available.`, persistent: false };
  if (next.route.kind === "local" && previous.route.kind === "guest") {
    if (!next.readyTokens.includes(previous.route.bondToken)) return { message: "Guest offline. Input returned to this computer; local control is retained.", persistent: true };
    return { message: "Returned to this computer. Input is local.", persistent: false };
  }
  return null;
}
