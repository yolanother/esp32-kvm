// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Mounts the desktop status shell. Input capture and routing belong to the native core, not this webview.
import React from "react";
import { createRoot } from "react-dom/client";
import "./style.css";

/** Shows the pre-connection shell without claiming hardware readiness. */
function App(): React.JSX.Element {
  return (
    <main>
      <h1>ESP32 KVM</h1>
      <p>Desktop setup is in progress.</p>
      <p className="status">Device status: not connected</p>
    </main>
  );
}

createRoot(document.getElementById("root")!).render(<App />);
