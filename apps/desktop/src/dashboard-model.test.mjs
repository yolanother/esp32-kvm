// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Verifies route summaries, safe dashboard choices and switch announcements
// from authoritative actor snapshots, including degraded local recovery.
import assert from "node:assert/strict";
import test from "node:test";
import { describeRoute, overlayForTransition, systemChoices } from "./dashboard-model.ts";

const token = "abababababababababababababababab";
const base = {
  device: { kind: "verified", boardId: "esp32-kvm-s3", firmwareVersion: null, maxBonds: 8, maxConnections: 1 },
  pairing: { kind: "closed" }, bondTokens: [token], readyTokens: [token],
  profiles: [{ bondToken: token, name: "Work Mac", os: "macos", profile: "unchanged" }], pairingAvailable: true,
};

test("only actor-confirmed guest route is labelled controlling", () => {
  assert.equal(describeRoute({ ...base, route: { kind: "local" } }).title, "This computer");
  assert.equal(describeRoute({ ...base, route: { kind: "switching" } }).title, "Switch pending");
  const guest = describeRoute({ ...base, route: { kind: "guest", slot: 1, bondToken: token } });
  assert.equal(guest.title, "Work Mac");
  assert.match(guest.detail, /Ctrl\+Alt\+F10/);
  assert.equal(describeRoute({ ...base, route: { kind: "guest", slot: 1, bondToken: "unknown" } }).title, "Guest slot 1");
});

test("offline and failed sessions say local without claiming a connected guest", () => {
  const failed = describeRoute({ ...base, device: { kind: "unavailable", reason: "Disconnected" }, route: { kind: "failed", reason: "transport" }, readyTokens: [] });
  assert.equal(failed.title, "This computer");
  assert.match(failed.detail, /transport/i);
  const choices = systemChoices({ ...base, readyTokens: [], route: { kind: "local" } });
  assert.equal(choices[0].state, "Offline");
  assert.equal(choices[0].selectEnabled, false);
});

test("switch overlay appears only after a real route change and persists on loss", () => {
  const local = { ...base, route: { kind: "local" } };
  const guest = { ...base, route: { kind: "guest", slot: 1, bondToken: token } };
  assert.equal(overlayForTransition(null, local), null);
  assert.equal(overlayForTransition(local, local), null);
  assert.equal(overlayForTransition(local, guest)?.persistent, false);
  const failure = overlayForTransition(guest, { ...base, route: { kind: "failed", reason: "transport" } });
  assert.equal(failure?.persistent, true);
  assert.match(failure?.message ?? "", /local/i);
  const offline = overlayForTransition(guest, { ...base, route: { kind: "local" }, readyTokens: [] });
  assert.equal(offline?.persistent, true);
  assert.match(offline?.message ?? "", /offline/i);
});
