// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Mounts the five-destination desktop shell; native workers alone own physical input timing.
import { createRoot } from "react-dom/client";
import App from "./App";
import "./style.css";

createRoot(document.getElementById("root")!).render(<App />);
