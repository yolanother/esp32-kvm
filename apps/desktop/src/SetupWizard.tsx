// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Renders keyboard-accessible USB setup, adoption of existing board bonds,
// deliberate BLE pairing, profile naming, and an explicit HID test.
import { useEffect, useRef, useState, type JSX } from "react";
import { beginPairing, cancelPairing, confirmPairing, saveGuestProfile, setupSnapshot, testGuestControls, unavailableSnapshot } from "./setup-api";
import { adoptableBondTokens, canBeginPairing, canFinishSetup, countdownSeconds, newBondToken, validGuestName, type GuestProfile, type SetupSnapshot } from "./setup-model";

const previewToken = "00112233445566778899aabbccddeeff";
const previewDevice: SetupSnapshot = {
  device: { kind: "verified", boardId: "esp32-kvm-s3", firmwareVersion: "Example firmware", maxBonds: 8, maxConnections: 1 },
  route: { kind: "local" },
  pairing: { kind: "closed" }, bondTokens: [], retainedBondTokens: null, connectedTokens: [], readyTokens: [], profiles: [], mappingPendingTokens: [], pairingAvailable: true,
};

/** Describe connection evidence without treating a COM name as verification. */
function deviceMessage(snapshot: SetupSnapshot): string {
  switch (snapshot.device.kind) {
    case "missing": return "No compatible USB device was found. Check the data cable and power, then scan again.";
    case "candidate": return "An ESP32 USB interface is present, but its firmware has not been verified yet.";
    case "handshaking": return "Checking board identity, firmware protocol and device session…";
    case "incompatible": return snapshot.device.reason === "wrong_board" ? "This is not the expected ESP32 KVM board." : snapshot.device.reason === "protocol" ? "Firmware protocol is incompatible. Review recovery instructions; setup will not flash automatically." : "The device did not answer the firmware handshake. Check the cable and retry.";
    case "verified": return `Verified ${snapshot.device.boardId}. Firmware ${snapshot.device.firmwareVersion ?? "version unavailable"}; up to ${snapshot.device.maxBonds} saved bonds.`;
    case "unavailable": return snapshot.device.reason;
  }
}

/** Setup wizard state is local until the native backend confirms each action. */
export default function SetupWizard({ preview, onClose }: { preview: boolean; onClose: () => void }): JSX.Element {
  const [step, setStep] = useState<"connect" | "pair" | "test">("connect");
  const [snapshot, setSnapshot] = useState<SetupSnapshot>(preview ? previewDevice : unavailableSnapshot());
  const [now, setNow] = useState(Date.now());
  const [knownTokens, setKnownTokens] = useState<string[]>([]);
  const [pairingAttempted, setPairingAttempted] = useState(false);
  const [bondToken, setBondToken] = useState<string | null>(null);
  const [name, setName] = useState("");
  const [os, setOs] = useState<GuestProfile["os"]>("macos");
  const [profile, setProfile] = useState<GuestProfile["profile"]>("unchanged");
  const [saved, setSaved] = useState(false);
  const [testPassed, setTestPassed] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const titleRef = useRef<HTMLHeadingElement>(null);

  useEffect(() => { titleRef.current?.focus(); }, [step]);
  useEffect(() => {
    if (preview) { setSnapshot(previewDevice); return; }
    let active = true;
    async function refresh(): Promise<void> {
      const next = await setupSnapshot();
      if (!active) return;
      setSnapshot(next);
      setNow(Date.now());
    }
    void refresh();
    const timer = window.setInterval(() => { void refresh(); }, 1000);
    return () => { active = false; window.clearInterval(timer); };
  }, [preview]);
  useEffect(() => {
    if (bondToken || step !== "pair") return;
    const added = newBondToken(knownTokens, snapshot.bondTokens, pairingAttempted);
    if (added) setBondToken(added);
  }, [snapshot.bondTokens, knownTokens, pairingAttempted, bondToken, step]);

  async function perform(action: () => Promise<void>): Promise<void> {
    setBusy(true); setError("");
    try { await action(); if (!preview) setSnapshot(await setupSnapshot()); }
    catch (failure) { setError(String(failure)); }
    finally { setBusy(false); }
  }

  function begin(): void {
    setKnownTokens(snapshot.bondTokens);
    setBondToken(null); setSaved(false); setTestPassed(false);
    if (preview) {
      setPairingAttempted(true);
      setSnapshot({ ...previewDevice, pairing: { kind: "challenge", challengeId: 412038, number: 381754, deadlineMs: Date.now() + 60000 } });
      return;
    }
    void perform(async () => { await beginPairing(); setPairingAttempted(true); });
  }

  function useExisting(token: string): void {
    if (!adoptableBondTokens(snapshot).includes(token)) return;
    setBondToken(token); setSaved(false); setTestPassed(false); setName(""); setError("");
  }

  function chooseAnother(): void {
    setBondToken(null); setPairingAttempted(false); setSaved(false); setTestPassed(false); setError("");
  }

  function answer(approved: boolean): void {
    if (snapshot.pairing.kind !== "challenge") return;
    if (preview) {
      setSnapshot({ ...previewDevice, pairing: { kind: "closed" }, bondTokens: approved ? [previewToken] : [], connectedTokens: approved ? [previewToken] : [], readyTokens: approved ? [previewToken] : [] });
      return;
    }
    void perform(() => confirmPairing(snapshot.pairing.kind === "challenge" ? snapshot.pairing.challengeId : 0, approved));
  }

  function cancel(): void {
    if (preview) { setPairingAttempted(false); setSnapshot(previewDevice); return; }
    void perform(async () => { await cancelPairing(); setPairingAttempted(false); });
  }

  function save(): void {
    if (!bondToken || !validGuestName(name)) return;
    const guest: GuestProfile = { bondToken, name: name.trim(), os, profile };
    if (preview) { setSaved(true); setSnapshot((current) => ({ ...current, profiles: [guest] })); setStep("test"); return; }
    void perform(async () => { await saveGuestProfile(guest); setSaved(true); setStep("test"); });
  }

  function runTest(): void {
    if (!bondToken) return;
    if (preview) { setTestPassed(true); return; }
    void perform(async () => { await testGuestControls(bondToken); setTestPassed(true); });
  }

  const pairing = snapshot.pairing;
  const capacityFull = snapshot.device.kind === "verified" && snapshot.bondTokens.length >= snapshot.device.maxBonds;
  const adoptable = adoptableBondTokens(snapshot);
  const canFinish = canFinishSetup({ device: snapshot.device, bondToken, readyTokens: snapshot.readyTokens, testPassed, saved });
  return <section className="setup card" aria-labelledby="setup-title">
    <div className="card-heading"><div><p className="eyebrow">DEVICE SETUP</p><h2 id="setup-title" ref={titleRef} tabIndex={-1}>Add a guest system</h2></div><button type="button" onClick={onClose} aria-label="Close setup">Close</button></div>
    {preview && <div className="notice notice--preview" role="note"><strong>Design preview</strong><span>This example does not pair a device or save a profile.</span></div>}
    <ol className="setup-steps" aria-label="Setup progress"><li aria-current={step === "connect" ? "step" : undefined}>1. Connect device</li><li aria-current={step === "pair" ? "step" : undefined}>2. Choose guest</li><li aria-current={step === "test" ? "step" : undefined}>3. Test controls</li></ol>
    {step === "connect" && <div className="setup-panel"><h3>Connect the input bridge</h3><p>Use a USB data cable. Setup checks the board and firmware before opening Bluetooth pairing; connecting never starts a firmware update.</p><div className="notice" role="status">{deviceMessage(snapshot)}</div>{snapshot.device.kind === "candidate" && <p className="muted">A port name alone cannot verify a device. Wait for the native session handshake.</p>}<div className="setup-actions"><button type="button" onClick={() => { if (!preview) void perform(async () => { setSnapshot(await setupSnapshot()); }); }} disabled={busy}>Scan again</button><button type="button" onClick={() => setStep("pair")} disabled={snapshot.device.kind !== "verified"}>Continue to guests</button></div></div>}
    {step === "pair" && <div className="setup-panel"><h3>Choose a guest</h3><p>If the guest has already paired with ESP32 KVM, use its board-reported connection below. For a new guest, open Bluetooth settings there and choose <strong>ESP32 KVM</strong>. The guest needs no app. Keep input local while pairing. On a board without touch, confirm the matching number here; the power button is not a pairing control.</p>
      {!bondToken && adoptable.length > 0 && <div className="setup-details"><h3>Already paired with the board</h3><p>Save a name and keyboard mapping for a guest already reported by firmware. No new pairing is needed.</p>{adoptable.map((token, index) => <div className="setup-actions" key={token}><span>Guest {index + 1} · {snapshot.readyTokens.includes(token) ? "Keyboard and mouse ready" : snapshot.connectedTokens.includes(token) ? "Connected, HID not ready" : "Paired, waiting for connection"}</span><button type="button" onClick={() => useExisting(token)} disabled={busy}>Use this guest</button></div>)}</div>}
      {capacityFull ? <div className="notice notice--warning" role="alert">All saved bond slots are full. You can still save a profile for an already paired guest. Forget a guest deliberately before pairing another.</div> : <div className="notice" role="status">{pairing.kind === "closed" ? "Pairing is closed." : pairing.kind === "waiting" ? `Waiting for the guest${pairing.deadlineMs === null ? "; countdown unavailable" : ` — ${countdownSeconds(now, pairing.deadlineMs)} seconds left`}.` : pairing.kind === "challenge" ? "Compare the number shown here with the guest. Approve only when both match." : pairing.kind === "expired" ? "The pairing window expired. Retry when ready." : pairing.kind === "full" ? "Bond storage is full; no guest was removed." : pairing.kind === "unsupported" ? `This guest cannot use the required protected pairing method: ${pairing.reason}` : `Pairing failed: ${pairing.reason}`}</div>}
      {pairing.kind === "challenge" && <div className="setup-challenge"><span>Numeric comparison</span><strong aria-label={`Pairing number ${String(pairing.number).padStart(6, "0").split("").join(" ")}`}>{String(pairing.number).padStart(6, "0")}</strong><div className="setup-actions"><button type="button" onClick={() => answer(false)} disabled={busy}>Numbers do not match</button><button type="button" onClick={() => answer(true)} disabled={busy}>Numbers match</button></div></div>}
      {!bondToken && <div className="setup-actions"><button type="button" onClick={() => setStep("connect")}>Back</button><button type="button" onClick={begin} disabled={busy || !snapshot.pairingAvailable || !canBeginPairing(snapshot.device, snapshot.bondTokens.length)}>Open 60-second pairing</button><button type="button" onClick={cancel} disabled={busy || pairing.kind === "closed"}>Cancel pairing</button></div>}
      {!snapshot.pairingAvailable && !preview && <p className="muted">Pairing controls are waiting for the verified native device session and firmware challenge events.</p>}
      {bondToken && <div className="setup-details"><h3>Name the paired guest</h3><p>The bond identity was reported by firmware. A friendly name and mapping preference stay on this computer.</p>{!snapshot.bondTokens.includes(bondToken) && <div className="notice notice--warning" role="status">This guest is no longer reported by the board. Reconnect it before saving.</div>}<label>Guest name<input value={name} maxLength={64} onChange={(event) => setName(event.target.value)} autoComplete="off" /></label><label>Guest OS<select value={os} onChange={(event) => setOs(event.target.value as GuestProfile["os"])}><option value="macos">macOS</option><option value="windows">Windows</option><option value="linux">Linux</option><option value="other">Other</option></select></label><label>Key profile preference<select value={profile} onChange={(event) => setProfile(event.target.value as GuestProfile["profile"])}><option value="unchanged">Unchanged keys</option><option value="cmd-to-ctrl">Cmd / Win to Ctrl</option><option value="windows-to-mac">Windows Ctrl to Mac Cmd</option></select></label><div className="setup-actions"><button type="button" onClick={chooseAnother} disabled={busy}>Choose another guest</button><button type="button" onClick={save} disabled={busy || !validGuestName(name) || !snapshot.bondTokens.includes(bondToken)}>Save guest and continue</button></div></div>}
    </div>}
    {step === "test" && <div className="setup-panel"><h3>Test, then return locally</h3><p>Start a short, explicit keyboard and pointer test. The native service must send all-up reports before this step succeeds. Do not infer readiness from a saved name.</p><div className="notice" role="status">{!bondToken || !snapshot.readyTokens.includes(bondToken) ? "Guest is offline or HID is not ready. Input stays local." : testPassed ? "Test completed and all input released." : "Guest is ready for an explicit test."}</div><div className="setup-actions"><button type="button" onClick={() => setStep("pair")}>Back</button><button type="button" onClick={runTest} disabled={busy || !bondToken || !snapshot.readyTokens.includes(bondToken)}>Test controls</button><button type="button" onClick={onClose} disabled={!canFinish}>Finish setup</button></div></div>}
    {error && <div className="notice notice--warning" role="alert">{error}</div>}
    <p className="small muted">No guest receives input during setup. A device or session loss keeps Windows in local control.</p>
  </section>;
}
