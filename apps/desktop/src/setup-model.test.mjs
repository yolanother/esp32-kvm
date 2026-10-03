// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Checks fail-closed setup gates, 60-second countdown, offline identity, and
// completion requirements before the pairing wizard is connected to hardware.
import assert from "node:assert/strict";
import test from "node:test";
import { canBeginPairing, canFinishSetup, countdownSeconds, liveGuestState, newBondToken, profileState, validDirectShortcut, validGuestName } from "./setup-model.ts";

const verified = { kind: "verified", boardId: "esp32-kvm-s3", firmwareVersion: "0.1.0-m1", maxBonds: 8, maxConnections: 1 };
const token = "00112233445566778899aabbccddeeff";

test("pairing requires verified firmware and free capacity", () => {
  assert.equal(canBeginPairing({ kind: "missing" }, 0), false);
  assert.equal(canBeginPairing({ kind: "candidate" }, 0), false);
  assert.equal(canBeginPairing({ kind: "incompatible", reason: "wrong_board" }, 0), false);
  assert.equal(canBeginPairing(verified, 7), true);
  assert.equal(canBeginPairing(verified, 8), false);
});

test("countdown expires at sixty seconds and never becomes negative", () => {
  assert.equal(countdownSeconds(1000, 61000), 60);
  assert.equal(countdownSeconds(60999, 61000), 1);
  assert.equal(countdownSeconds(61000, 61000), 0);
  assert.equal(countdownSeconds(90000, 61000), 0);
});

test("saved identity remains visible offline but cannot complete setup", () => {
  const saved = [{ bondToken: token, name: "Work Mac", os: "macos", profile: "unchanged" }];
  assert.equal(profileState(saved, token, []), "offline");
  assert.equal(profileState(saved, token, [token]), "ready");
  assert.equal(profileState(saved, "ffeeddccbbaa99887766554433221100", []), "unknown");
  assert.equal(canFinishSetup({ device: verified, bondToken: token, readyTokens: [], testPassed: true, saved: true }), false);
  assert.equal(canFinishSetup({ device: verified, bondToken: token, readyTokens: [token], testPassed: false, saved: true }), false);
  assert.equal(canFinishSetup({ device: verified, bondToken: token, readyTokens: [token], testPassed: true, saved: true }), true);
});

test("guest names are bounded and cannot be blank", () => {
  assert.equal(validGuestName("  "), false);
  assert.equal(validGuestName("Work Mac"), true);
  assert.equal(validGuestName("x".repeat(65)), false);
});

test("the wizard selects only a bond added after its explicit pairing attempt", () => {
  const previous = [token];
  const next = "ffeeddccbbaa99887766554433221100";
  assert.equal(newBondToken(previous, previous, false), null);
  assert.equal(newBondToken(previous, [token, next], false), null);
  assert.equal(newBondToken(previous, previous, true), null);
  assert.equal(newBondToken(previous, [token, next], true), next);
});

test("connection model keeps connected, ready and active separate", () => {
  const data = { device: verified, route: { kind: "local" }, connectedTokens: [], readyTokens: [] };
  assert.equal(liveGuestState(token, data), "offline");
  assert.equal(liveGuestState(token, { ...data, connectedTokens: [token] }), "connected");
  assert.equal(liveGuestState(token, { ...data, connectedTokens: [token], readyTokens: [token] }), "ready");
  assert.equal(liveGuestState(token, { ...data, connectedTokens: [token], readyTokens: [token], route: { kind: "guest", slot: 1, bondToken: token } }), "active");
  assert.equal(liveGuestState(token, { ...data, device: { kind: "missing" }, connectedTokens: [token], readyTokens: [token] }), "offline");
});

test("optional direct shortcut rejects unbounded or malformed values", () => {
  assert.equal(validDirectShortcut(""), true);
  assert.equal(validDirectShortcut("Ctrl+Alt+1"), true);
  assert.equal(validDirectShortcut("Ctrl Alt 1"), false);
  assert.equal(validDirectShortcut("x".repeat(65)), false);
});
