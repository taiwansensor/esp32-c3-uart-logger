# ESP32-C3 無線 UART Logger

使用 ESP32-C3 製作的獨立、RX-only 無線 UART 診斷記錄器。它能在沒有電腦長時間接在設備旁的情況下，持續收集 ESP32、ESP8266、Arduino 或其他 3.3 V UART 裝置的啟動訊息、一般日誌、Watchdog 與 Panic Backtrace。

目前版本：`0.1.0-alpha.3`

> 本專案仍屬 Alpha。已完成 ESP32-C3 4 MB 實板、弱 Wi-Fi、Web OTA、長時間 UART 收集與循環檔案測試；正式用於關鍵設備前，請先依自己的板型、GPIO 與電壓完成驗證。

## 為什麼需要它？

很多 ESP32 裝置只在偶發重啟時，從 UART 印出真正的 Panic 原因。一般 USB 線若沒有一直連著電腦，事件發生後只能看到「裝置重新上線」，卻拿不到重啟前的 Backtrace。

本 Logger 放在被監測設備旁邊，使用自己的 USB 電源與 Wi-Fi，單向接收目標板 UART TX。即使電腦暫時離線，C3 仍會將資料寫入 LittleFS；電腦重新連線後也能從 Web 下載。

```text
目標設備 UART TX
        │ 3.3 V、RX-only
        ▼
ESP32-C3 UART driver（16 KiB）
        ▼
高優先接收佇列（32 KiB）
        ├── RAM 尾端畫面（24 KiB）
        ├── LittleFS 循環日誌
        ├── Panic 事件快照
        ├── Web 即時查看／下載
        └── TCP 2323 唯讀串流
```

## 主要功能

- UART RX-only；預設 GPIO4、`115200 8N1`，可從 Web 修改。
- 16 KiB UART 驅動緩衝與 32 KiB 高優先接收佇列。
- 24 KiB RAM 尾端緩衝，Web 可即時查看。
- 兩個 384 KiB 一般日誌循環輪替。
- 三個 128 KiB Panic 快照循環輪替。
- 兩個 64 KiB Wi-Fi 事件日誌循環輪替，記錄連線、取得 IP、斷線原因及 fallback AP 用戶端事件。
- 每 5 分鐘記錄上游 AP 的 SSID、BSSID、Channel、RSSI、IP、Heap 與 fallback AP 狀態。
- Panic 事件發生時，保留事件前約 24 KiB，事件後繼續記錄 90 秒。
- 辨識 ESP-IDF Panic、Backtrace、Assert、Task Watchdog、Brownout、Heap corruption 與 Stack canary。
- 唯讀 TCP 2323 串流，最多三個用戶端；遠端送入的資料會被丟棄。
- Wi-Fi AP 首次設定、mDNS、Wi-Fi 自動重連。
- Web 顯示 RSSI、上游 AP、連線／斷線次數、最後斷線原因、Link uptime、UART Error、Queue drops 與 Heap。
- Web OTA 更新 Logger 韌體。
- Windows PowerShell 自動重連收集器。
- 不需要第三方 Arduino 程式庫。

## 硬體需求

- ESP32-C3 開發板，建議 4 MB Flash。
- 穩定的 USB 5 V 電源。
- 兩條訊號線：UART TX 與 GND。
- 建議在 UART 訊號線串聯約 1 kΩ 保護電阻。

本專案實測晶片為 ESP32-C3 rev 0.4、4 MB Flash。其他 C3 板型通常可以使用，但必須先確認 GPIO4 已引出且沒有被板載周邊占用。

## 接線

| 目標設備 | ESP32-C3 Logger | 說明 |
|---|---|---|
| UART TX | GPIO4 | Logger 單向接收 |
| GND | GND | 必須共地 |
| UART RX | 不接 | 避免 Logger 控制或干擾目標設備 |
| VCC | 不接 | 兩塊板子各自供電 |

只允許直接接收 **3.3 V TTL UART**。RS-232、RS-485 A/B、5 V UART 或工業現場長線不能直接接到 ESP32-C3，必須使用合適的電平轉換器、收發器或隔離模組。詳細說明請看 [硬體與接線指南](docs/HARDWARE_zh-TW.md)。

## 第一次啟動

1. 燒錄完成後，Logger 會建立 `UART-Logger-xxxx` AP。
2. 連接該 AP，密碼為 `uartlogger`。
3. 開啟 `http://192.168.4.1/config`。
4. 填入 2.4 GHz Wi-Fi SSID、密碼、UART Baud、RX GPIO 與 TCP Port。
5. 儲存後 Logger 自動重新啟動。
6. 從路由器查詢 DHCP IP，或開啟 `http://uart-logger.local/`。

ESP32-C3 不支援 5 GHz Wi-Fi。若 15 秒內無法連線，Logger 會啟動 fallback AP，同時繼續嘗試 STA 自動重連；稍後連回原 Wi-Fi 時，fallback AP 仍會保留到下次重新啟動。

Web 預設沒有密碼。設定 Web 密碼後會啟用 Basic Auth；勾選「清除 Web 密碼」即可恢復無密碼。Web 使用者預設為 `admin`。

## Web 介面

首頁包含：

- UART 即時尾端畫面。
- 網路 IP、Wi-Fi RSSI、SSID／BSSID／Channel 與 Logger uptime。
- Wi-Fi 連線、取得 IP、斷線、遺失 IP 次數，以及最後一次斷線原因與目前 Link uptime。
- UART Error、Overflow 及最後發生時間。
- 接收佇列丟棄位元組與 TCP skipped chunks。
- Panic 快照數量、LittleFS 容量與可用 Heap。
- 日誌下載、設定與 Web OTA。

按下「清除畫面」只清除瀏覽器上的顯示，不會刪除 LittleFS 日誌。「清除所有日誌」才會刪除 Logger 內的 UART、Panic 與 Wi-Fi 事件檔案。

## 日誌檔案

| 檔案 | 用途 |
|---|---|
| `uart-0.log`、`uart-1.log` | 一般 UART 循環日誌 |
| `panic-0.log`～`panic-2.log` | Panic 事件前後快照 |
| `wifi-0.log`、`wifi-1.log` | Wi-Fi／AP 事件與每 5 分鐘心跳 |

每次 Logger 開啟日誌時會插入包含 Logger 版本、開機次數與時間的分隔標頭。Wi-Fi 日誌使用台灣時區（UTC+8）；NTP 尚未取得時間時改以 `uptime-ms` 標示。目標 UART 原始資料不會被改寫，但日誌並非逐位元組完全等同於目標輸出。

## Windows 長時間收集

```powershell
.\tools\capture_uart_tcp.ps1 `
  -HostName uart-logger.local `
  -Port 2323
```

工具會在 `captures` 建立每個 TCP 工作階段的 `.log`，斷線後每兩秒自動重連。重新連線時 Logger 會先傳送最近最多 4 KiB，因此相鄰檔案可能有少量重複。

## 編譯

### Arduino CLI

安裝 [Arduino CLI](https://arduino.github.io/arduino-cli/) 與 Espressif ESP32 Core，並確認 `arduino-cli` 已加入 PATH：

```powershell
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.8
.\tools\build.ps1
```

編譯並燒錄：

```powershell
.\tools\build.ps1 -Upload -Port COM18
```

若 Arduino CLI 不在 PATH，可手動指定：

```powershell
.\tools\build.ps1 -ArduinoCli 'D:\Tools\arduino-cli.exe'
```

### PlatformIO

專案已附 `platformio.ini`：

```powershell
pio run
pio run --target upload --upload-port COM18
```

## Web OTA

在 Web 設定頁選擇 Arduino CLI 產生的應用程式 `.bin`，不要使用 4 MB merged 映像。弱 Wi-Fi 下可限制上傳速率：

```powershell
curl.exe --header 'Expect:' --limit-rate 80k `
  -F 'firmware=@.arduino-build\esp32-c3-uart-logger.ino.bin' `
  http://uart-logger.local/update
```

OTA 成功會回覆 `Update complete; rebooting in 2 seconds`。若上一次傳輸中斷，可先從 Web 執行重新啟動，再重試 OTA。

## 如何修改成自己的版本

你可以修改：

- 預設 UART GPIO、Baud、TCP Port。
- UART 與接收佇列大小。
- 一般日誌與 Panic 快照容量。
- Panic 關鍵字與擷取時間。
- Web 頁面、狀態欄位與產品名稱。
- AP 名稱、mDNS hostname 與預設密碼策略。

完整步驟與程式位置請看 [二次開發與修改教學](docs/CUSTOMIZATION_zh-TW.md)。

常見連線、UART 與 OTA 問題請看 [疑難排解](docs/TROUBLESHOOTING_zh-TW.md)。

## 專案結構

```text
esp32-c3-uart-logger.ino    Arduino sketch 入口
src/main.cpp                Logger 韌體主程式
platformio.ini              PlatformIO 設定
tools/build.ps1             Arduino CLI 編譯／燒錄工具
tools/capture_uart_tcp.ps1  Windows TCP 自動收集器
docs/                       接線、修改與疑難排解文件
```

## 已知限制

- Alpha 版仍需要更多板型與長時間實測。
- Panic 依 UART 文字關鍵字觸發；若目標完全沒有輸出，就無法取得 Backtrace。
- LittleFS 容量有限；需要保存數天或數週時，應同時使用電腦端 TCP 收集器，或改用 SD 卡版本。
- TCP 2323 是區域網路上的明文唯讀串流，沒有 TLS 或帳密。
- Web Basic Auth 也不是端對端加密，不應直接暴露至網際網路。
- Wi-Fi 很弱時，OTA 應限制上傳速率或先把 Logger 移近 AP。

## 貢獻

歡迎提出 Issue、板型測試結果或 Pull Request。請先閱讀 [貢獻指南](CONTRIBUTING_zh-TW.md)。

## 授權

本專案使用 [MIT License](LICENSE)。你可以修改、散布與商業使用，但需保留授權與著作權聲明。
