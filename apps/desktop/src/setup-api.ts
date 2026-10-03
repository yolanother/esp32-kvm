// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Calls typed Tauri setup commands. A browser preview and missing native
// command surface report unavailable status rather than simulating hardware.
import { invoke } from "@tauri-apps/api/core";
import type { GuestProfile, SetupSnapshot } from "./setup-model";

/** Return a truthful disconnected snapshot when the native shell is absent. */
export function unavailableSnapshot(reason = "Native device service is unavailable."): SetupSnapshot {
  return { device: { kind: "unavailable", reason }, route: { kind: "failed", reason: "native_service" }, pairing: { kind: "closed" },
    bondTokens: [], readyTokens: [], profiles: [], pairingAvailable: false };
}

/** Reads current USB and pairing state without activating input routing. */
export async function setupSnapshot(): Promise<SetupSnapshot> {
  try { return await invoke<SetupSnapshot>("setup_snapshot"); }
  catch (error) { return unavailableSnapshot(String(error)); }
}

/** Requests a deliberate 60-second firmware pairing window. */
export function beginPairing(): Promise<void> { return invoke("setup_begin"); }
/** Cancels an open pairing window or numeric challenge. */
export function cancelPairing(): Promise<void> { return invoke("setup_cancel"); }
/** Replies to a protocol challenge ID; code digits never leave the status view. */
export function confirmPairing(challengeId: number, approved: boolean): Promise<void> {
  return invoke("setup_confirm", { challengeId, approved });
}
/** Saves a local label for a firmware-proven opaque bond token. */
export function saveGuestProfile(profile: GuestProfile): Promise<void> {
  return invoke("setup_save_profile", { profile });
}
/** Runs an explicit bounded HID test ending in an all-up report. */
export function testGuestControls(bondToken: string): Promise<void> {
  return invoke("setup_test_controls", { bondToken });
}

/** Requests safe local return; the dashboard waits for actor status before updating. */
export function returnToHost(): Promise<void> { return invoke("dashboard_return_local"); }
