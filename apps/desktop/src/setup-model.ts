// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Defines setup status and fail-closed wizard gates. Device facts must come
// from the native serial owner; saved names are never proof of BLE readiness.

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

/** A local label keyed by an opaque 16-byte firmware bond identity. */
export interface GuestProfile {
  bondToken: string;
  name: string;
  os: "windows" | "macos" | "linux" | "other";
  profile: "unchanged" | "windows-to-mac";
}

/** Native snapshot of one setup session and remembered guest readiness. */
export interface SetupSnapshot {
  device: DeviceState;
  pairing: PairingState;
  bondTokens: string[];
  readyTokens: string[];
  profiles: GuestProfile[];
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

/** Selects only an identity first reported after the user's pairing request. */
export function newBondToken(previous: readonly string[], current: readonly string[], attempted: boolean): string | null {
  if (!attempted) return null;
  return current.find((token) => !previous.includes(token)) ?? null;
}

/** Validates a user label before it is stored alongside an opaque token. */
export function validGuestName(name: string): boolean {
  const length = name.trim().length;
  return length > 0 && length <= 64;
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
