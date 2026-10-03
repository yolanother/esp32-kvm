// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Verifies manual layout placement, exposed-edge preview, portal drafts and
// capability gating before a native monitor snapshot can activate crossing.
import assert from "node:assert/strict";
import test from "node:test";
import { addGuestPlaceholders, defaultDraft, exposedSegments, moveDisplay, portalForSegment, restoreDraft, standardCapabilities, validDraft, withDetectedHosts, matchesDetectedHosts } from "./layout-model.ts";

test("detected mixed-DPI monitors replace draft hosts and invalidate obsolete portals", () => {
  const draft = defaultDraft();
  const hosts = [{ id: "\\\\.\\DISPLAY2", x: -1600, y: -100, width: 1600, height: 900, dpiX: 144, dpiY: 144, rotation: "deg90", primary: false },
    { id: "\\\\.\\DISPLAY1", x: 0, y: 0, width: 1920, height: 1080, dpiX: 96, dpiY: 96, rotation: "deg0", primary: true }];
  const next = withDetectedHosts(draft, hosts);
  assert.equal(next.hosts[0].source, "windows");
  assert.equal(next.hosts[0].x, -1600);
  assert.equal(next.hosts[0].dpiX, 144);
  assert.equal(matchesDetectedHosts(next, hosts), true);
  assert.equal(matchesDetectedHosts(moveDisplay(next, hosts[0].id, -1500, -100), hosts), false);
  assert.equal(next.portals.length, 0);
});

test("adjacent host displays hide their shared seam and retain outer edges", () => {
  const draft = defaultDraft();
  const second = { ...draft.hosts[0], id: "host-2", name: "Manual display 2", x: 1920, primary: false };
  const segments = exposedSegments([draft.hosts[0], second]);
  assert.equal(segments.some((segment) => segment.monitorId === "host-1" && segment.edge === "right"), false);
  assert.equal(segments.some((segment) => segment.monitorId === "host-2" && segment.edge === "left"), false);
  assert.equal(segments.some((segment) => segment.monitorId === "host-1" && segment.edge === "left"), true);
});

test("drag and numeric placement use the same draft coordinates", () => {
  const draft = defaultDraft();
  const moved = moveDisplay(draft, "host-1", -1920, 120);
  assert.equal(moved.hosts[0].x, -1920);
  assert.equal(moved.hosts[0].y, 120);
  assert.equal(validDraft({ ...moved, hosts: [{ ...moved.hosts[0], width: 0 }] }), false);
});

test("manual guest placeholders are profile-named and portals stay dry-run only", () => {
  const token = "ab".repeat(16);
  const draft = addGuestPlaceholders(defaultDraft(), [{ bondToken: token, name: "Work Mac" }]);
  assert.equal(draft.guests[0].name, "Work Mac");
  assert.equal(draft.guests[0].source, "manual");
  const segment = exposedSegments(draft.hosts).find((edge) => edge.edge === "right");
  const portal = portalForSegment(segment, token);
  assert.equal(portal.direction, "outward");
  assert.equal(portal.dwellMs, 200);
  assert.equal(standardCapabilities().canEnablePortals, false);
  assert.equal(standardCapabilities().canReturnFromGuestEdge, false);
});

test("versioned local drafts restore negative coordinates and reject damaged data", () => {
  const moved = moveDisplay(defaultDraft(), "host-1", -800, -200);
  assert.deepEqual(restoreDraft(JSON.stringify(moved)), moved);
  assert.deepEqual(restoreDraft('{damaged'), defaultDraft());
  assert.deepEqual(restoreDraft(JSON.stringify({ ...moved, version: 99 })), defaultDraft());
});
