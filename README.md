# CYD Notion To‑Do List Display (ESP32‑2432S028)

Turn the **Cheap Yellow Display (CYD, ESP32‑2432S028, 2.8" ILI9341 + XPT2046)** into a clean, touch‑friendly **Notion to‑do monitor**. It fetches your tasks from a Notion database and shows them in a paginated list with on‑screen buttons and local checkboxes.

> ✅ 5‑point touch calibration (least‑squares affine) stored in NVS
>
> ✅ Fast Notion fetch via HTTPS (Notion API 2022‑06‑28)
>
> ✅ Compact UI with paging + refresh
>
> ✅ Local checkbox toggles (no writes to Notion — by design)



---

## Table of Contents

* [Features](#features)
* [Hardware](#hardware)
* [Screens & Controls](#screens--controls)
* [Notion Setup](#notion-setup)
* [Build & Flash](#build--flash)

  * [Arduino IDE](#arduino-ide)
  * [PlatformIO](#platformio)
* [Configure Credentials](#configure-credentials)
* [How It Works](#how-it-works)
* [Security Notes](#security-notes)
* [Troubleshooting](#troubleshooting)
* [Customization](#customization)
* [Roadmap](#roadmap)
* [Contributing](#contributing)
* [License](#license)

---

## Features

* **Touch calibration**: 5 targets → **least‑squares affine fit** `(a,b,c,d,e,f)` → saved to **NVS/Preferences** so it survives reboots.
* **UI**: simple list with **UP | REFRESH | DOWN** buttons, row dividers, checkbox glyphs.
* **Pagination**: auto‑computes visible rows from screen geometry; page up/down step by exactly one screenful.
* **Notion fetch**: pulls pages in chunks (`page_size=25`, safety‑capped) and **maps `Status` → checkbox** using a case‑insensitive match for `done / complete / finished`.
* **Schema assumptions** (configurable in code):

  * **Title property**: `Task` *(type: title)*
  * **Status property**: `Status` *(type: status)*
* **Storage**: tasks are kept in RAM (max **50**), reversed for a more recent‑first feel.

> ⚠️ Checkboxes toggle **locally** (visual only). This project is a **monitor**; it does not write back to Notion.

---

## Hardware & Enclosure

* **Electronics**: **ESP32‑2432S028 (CYD)** — the only required board.
* **Enclosure**: **3D‑printed part from “Aura – Smart Weather Forecast Display”** (front shell/stand).
* **Cable/connector (recommended)**: **Any 90° angled USB‑C** (or Micro‑USB, depending on your CYD revision) adapter to route the cable cleanly and avoid strain. Choose the orientation (left/right, up/down) that matches your enclosure cutout.

> No extra sensors, breakouts, or shields are required.

### Touch wiring used by this sketch (XPT2046 over VSPI)

| Signal         | Pin                   |
| -------------- | --------------------- |
| `XPT2046_IRQ`  | **36**                |
| `XPT2046_MOSI` | **32**                |
| `XPT2046_MISO` | **39** *(input‑only)* |
| `XPT2046_CLK`  | **25**                |
| `XPT2046_CS`   | **33**                |

> The TFT panel is driven by **TFT\_eSPI**. Ensure your **TFT\_eSPI** setup matches your CYD’s display driver (e.g., ILI9341).

---

## Bill of Materials

* **ESP32‑2432S028 (CYD) display board** ×1
* **3D‑printed shell/stand**: *Aura – Smart Weather Forecast Display*  ×1
* **90° angled USB‑C or Micro‑USB adapter**, orientation to suit the enclosure cutout ×1

---

## Screens & Controls

```
┌───────────────────────────────┐
│  To‑Do List                   │  Title bar
│  (status line)                │
├───────────────────────────────┤
│ □ Task A [In Progress]        │
│ □ Task B [Todo]               │  Paginated list area (tap a row’s
│ ■ Task C [Done]               │  checkbox to toggle locally)
│ …                             │
├───────────────────────────────┤
│   UP     |  REFRESH  |  DOWN  │  Bottom action bar
└───────────────────────────────┘
```

* **UP/DOWN**: page through your tasks.
* **REFRESH**: re‑query Notion.
* **Checkbox tap**: toggles **visual** state only (no API write).
* **Force re‑calibration**: on boot, **touch and hold** the screen within \~700 ms to enter the 5‑point calibration.

---

## Notion Setup

1. Create a **Notion integration** → copy the **Internal Integration Token**.
2. Create a **database** (or pick an existing one) with **two properties**:

   * `Task` → *Title*
   * `Status` → *Status* (with options like *Todo*, *In Progress*, *Done*)
3. **Share** that database with your integration (top‑right ••• → *Add connections*).
4. Copy the **Database ID** (32‑char hex or dashed form from the URL).

> Mapping rule: a task counts as **checked** if its `Status` name contains `done`, `complete`, or `finished` (case‑insensitive).

---

## Build & Flash

### Arduino IDE

* **Board**: `ESP32 Dev Module` (ESP32 Arduino core v2.0.x or newer)
* **Libraries (Library Manager)**:

  * `TFT_eSPI`
  * `XPT2046_Touchscreen` (Paul Stoffregen)
  * `ArduinoJson` (v6)
  * (built‑in) `WiFi`, `WiFiClientSecure`, `HTTPClient`, `Preferences`
* Open the sketch and edit the **Config** constants (see below). Compile & upload at 115200.

---

## Configure Credentials

Edit these at the top of the sketch (or move to a `secrets.h` you don’t commit):

```cpp
const char* WIFI_SSID = "your-ssid";
const char* WIFI_PSK  = "your-pass";

const char* NOTION_SECRET = "secret_...";   // Internal Integration Token
const char* DATABASE_ID   = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"; // or dashed form
const char* NOTION_VERSION = "2022-06-28";  // API version used

const bool USE_INSECURE_TLS = true;          // see Security Notes
```

**Pro tip**: For production, store secrets in **NVS (Preferences)** or prompt once at runtime and persist; avoid hard‑coding tokens in firmware.

---

## How It Works

* **Touch**

  * `readRawAvg(...)` samples XPT2046 multiple times while pressed to reduce noise.
  * `fitAffineLS(...)` solves two 3×3 systems (x and y) to get the affine transform.
  * Coefficients are saved under the `"touch"` namespace in **Preferences**.
* **UI**

  * Fixed portrait layout **240×320**; rows are `30 px` high; checkboxes are `22 px` squares.
  * Visible rows are computed at runtime; long titles are ellipsized.
* **Notion**

  * Queries `POST /v1/databases/{id}/query` in pages (`page_size=25`) up to `MAX_TASKS=50`.
  * Title pulled from the first `Task.title[0].plain_text` (fallback `(untitled)`).
  * Status pulled from `Status.status.name`.
  * Results are reversed client‑side.

---

## Security Notes

* `USE_INSECURE_TLS = true` disables certificate validation on `api.notion.com` for convenience. **Don’t ship this**.
* Prefer loading the **ISRG Root X1** (or the current Notion root) into `WiFiClientSecure` and call `setTrustAnchors(...)`.
* Treat your **Notion token** like a password. Consider scoping a dedicated database for this device.

---

## Troubleshooting

* **“Wi‑Fi failed”**: Check SSID/PSK and signal. CYD antennas are small; try closer to the AP.
* **“HTTP begin() failed / JSON parse error / Notion error”**: Verify token, database sharing, database ID, and API version.
* **Touch offset or drift**: Force recalibration by **touch‑and‑hold** during boot; ensure you tap the center of crosshairs.
* **Garbled text / missing rows**: Ensure **TFT\_eSPI** is configured for **ILI9341** (or your panel) and matching pins.
* **Out of memory**: Reduce `DynamicJsonDocument` from `32768` or fetch fewer pages.
* **Rate limits**: Avoid hammering REFRESH; Notion enforces rate limits.

---

## Customization

* **Colors & fonts**: tweak `COLOR_*` and `FONT_*` constants.
* **Row behavior**: enable row‑tap toggling (not just checkbox) by uncommenting the noted line in `handleTouchUI()`.
* **Task mapping**: change the `done` keywords in the Notion mapping logic.
* **Rotation**: set `tft.setRotation(...)` / `ts.setRotation(...)` if you prefer landscape.
* **Persistence**: extend the code to cache tasks in NVS so a list shows before network comes up.
* **Write‑back**: add a `PATCH /v1/pages/{id}` call to mirror toggles into Notion (requires storing page IDs).

---

## Roadmap

* **Notion write‑back (checkbox sync)**
  When a task is toggled on the ESP32, mirror the change in Notion: store each task’s `page_id`, call **`PATCH /v1/pages/{page_id}`** to update the `Status` (mapping configurable), debounce taps, queue retries with exponential backoff, and persist an offline queue in NVS.
  **Acceptance:** toggling on CYD reliably updates Notion within a few seconds and safely buffers when offline.
* **Templates, fonts, graphics & animations**
  Add switchable **UI templates** (Minimal, Card, Compact), support TFT\_eSPI **FreeFonts / .vlw** smooth fonts with built‑in fallbacks, small bitmap icons, and lightweight **scroll/page animations** (sprite‑based) with a low‑RAM fallback.
  **Acceptance:** user can change look & feel at runtime; animations remain smooth on CYD.

## Contributing

PRs and issues welcome! Please:

1. Describe the behavior/change clearly.
2. Include board, library versions, and logs if it’s a bug.
3. Keep code style consistent.

---

## License

## License
This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file in the repo root for details.

---

## Acknowledgements

* CYD / ESP32 community projects and examples.
* Libraries: **TFT\_eSPI**, **XPT2046\_Touchscreen**, **ArduinoJson**.
* **Enclosure**: *Aura – Smart Weather Forecast Display* 3D‑printed model.

> If you build one, please share a photo/GIF and your Notion board setup!
