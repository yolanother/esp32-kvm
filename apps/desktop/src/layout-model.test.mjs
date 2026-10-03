// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Verifies manual layout placement, exposed-edge preview, portal drafts and
// capability gating before a native monitor snapshot can activate crossing.
import assert from "node:assert/strict";
import test from "node:test";
import { addGuestPlaceholders, defaultDraft, exposedSegments, moveDisplay, portalForSegment, restoreDraft, standardCapabilities, validDraft } from "./layout-model.ts";

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
