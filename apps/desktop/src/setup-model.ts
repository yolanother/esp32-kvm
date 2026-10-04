// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Defines setup status and fail-closed wizard gates. Device facts must come
// from the native serial owner; retained bonds and per-guest physical key rules
// remain separate from live slots and observed input.

/** Device discovery and firmware-verification state supplied by native code. */
export type DeviceState =
  | { kind: "missing" }
  | { kind: "candidate"; port?: string }
  | { kind: "handshaking" }
  | { kind: "incompatible"; reason: "wrong_board" | "protocol" | "unresponsive" }
  | { kind: "verified"; boardId: string; firmwareVersion: string | null; maxBonds: number; maxConnections: number }
  | { kind: "unavailable"; reason: string };

/** Transient pairing state; codes never enter diagnostics or saved profiles. */
export type PairingState =
  | { kind: "closed" }
  | { kind: "waiting"; deadlineMs: number | null }
  | { kind: "challenge"; challengeId: number; number: number; deadlineMs: number | null }
  | { kind: "full" }
  | { kind: "expired" }
  | { kind: "unsupported"; reason: string }
  | { kind: "failed"; reason: string };

/** Actor-confirmed routing state, separate from USB and BLE readiness. */
export type RouteState =
  | { kind: "local" }
  | { kind: "awaiting_status" }
  | { kind: "switching" }
  | { kind: "pairing" }
  | { kind: "guest"; slot: number; bondToken: string }
  | { kind: "failed"; reason: string };

/** A local label keyed by an opaque 16-byte firmware bond identity. */
export interface ModifierBinding { sourceUsage: number; targetUsage: number }

/** One physical HID keyboard key and its side when it is a modifier. */
export interface KeySource { usage: number; side: "unspecified" | "left" | "right" }

/** One exact source chord and emitted guest chord, evaluated once. */
export interface KeyRule { source: KeySource[]; target: KeySource[]; priority: number; enabled: boolean }

/** A local label keyed by an opaque 16-byte firmware bond identity. */
export interface GuestProfile {
  bondToken: string;
  name: string;
  os: "windows" | "macos" | "linux" | "other";
  profile: "unchanged" | "cmd-to-ctrl" | "windows-to-mac" | "custom";
  customBasePreset?: "unchanged" | "cmd-to-ctrl" | "windows-to-mac" | null;
  modifierBindings?: ModifierBinding[];
  keyRules?: KeyRule[];
  directShortcut?: string | null;
  mappingProfileId?: string | null;
  layoutLinkId?: string | null;
}

/** Native snapshot of one setup session and remembered guest readiness. */
export interface SetupSnapshot {
  device: DeviceState;
  route: RouteState;
  pairing: PairingState;
  bondTokens: string[];
  retainedBondTokens: string[] | null;
  connectedTokens: string[];
  readyTokens: string[];
  profiles: GuestProfile[];
  mappingPendingTokens: string[];
  pairingAvailable: boolean;
}

/** Returns true only after a verified firmware handshake with bond capacity. */
export function canBeginPairing(device: DeviceState, bondCount: number): boolean {
  return device.kind === "verified" && bondCount < device.maxBonds;
}

/** Returns a nonnegative whole-second countdown from a monotonic deadline. */
export function countdownSeconds(nowMs: number, deadlineMs: number): number {
  return Math.max(0, Math.ceil((deadlineMs - nowMs) / 1000));
}

/** Distinguishes a saved, offline label from live firmware readiness. */
export function profileState(profiles: readonly GuestProfile[], token: string, readyTokens: readonly string[]): "unknown" | "offline" | "ready" {
  if (!profiles.some((profile) => profile.bondToken === token)) return "unknown";
  return readyTokens.includes(token) ? "ready" : "offline";
}

/** Derives one guest's live state from verified STATUS and acknowledged route. */
export function liveGuestState(token: string, snapshot: Pick<SetupSnapshot, "device" | "route" | "connectedTokens" | "readyTokens">): "offline" | "connected" | "ready" | "active" {
  if (snapshot.device.kind !== "verified") return "offline";
  if (snapshot.route.kind === "guest" && snapshot.route.bondToken === token) return "active";
  if (snapshot.readyTokens.includes(token)) return "ready";
  if (snapshot.connectedTokens.includes(token)) return "connected";
  return "offline";
}

/** Allows bond removal only with authoritative retained inventory and a verified local route. */
export function canForgetRetainedGuest(snapshot: Pick<SetupSnapshot, "device" | "route" | "retainedBondTokens">, token: string): boolean {
  return snapshot.device.kind === "verified" && snapshot.route.kind === "local" && snapshot.retainedBondTokens?.includes(token) === true;
}

/** Selects only an identity first reported after the user's pairing request. */
export function newBondToken(previous: readonly string[], current: readonly string[], attempted: boolean): string | null {
  if (!attempted) return null;
  return current.find((token) => !previous.includes(token)) ?? null;
}

/** Lists live firmware-reported bonds that have no host profile yet. */
export function adoptableBondTokens(snapshot: Pick<SetupSnapshot, "device" | "route" | "bondTokens" | "profiles">): string[] {
  if (snapshot.device.kind !== "verified" || snapshot.route.kind !== "local") return [];
  return snapshot.bondTokens.filter((token) => !snapshot.profiles.some((profile) => profile.bondToken === token));
}

/** Validates a user label before it is stored alongside an opaque token. */
export function validGuestName(name: string): boolean {
  const length = name.trim().length;
  return length > 0 && length <= 64;
}

/** Accepts an optional direct-select chord label in the native store's format. */
export function validDirectShortcut(value: string): boolean {
  return value.length === 0 || (value.length <= 64 && /^[A-Za-z0-9+-]+$/.test(value));
}

/** Finishing requires a saved identity, explicit test, and live HID readiness. */
export function canFinishSetup(value: {
  device: DeviceState;
  bondToken: string | null;
  readyTokens: readonly string[];
  testPassed: boolean;
  saved: boolean;
}): boolean {
  return value.device.kind === "verified" && value.bondToken !== null &&
    value.readyTokens.includes(value.bondToken) && value.testPassed && value.saved;
}
