# Changelog

## 0.1.0-alpha.2 - 2026-08-14

- 將 UART driver buffer 提升至 16 KiB。
- 加入 32 KiB 高優先 UART 接收佇列。
- 加入 RSSI、最後 Error／Overflow、Queue drops 狀態。
- 降低 Web tail 與狀態輪詢頻率。
- 關閉 Wi-Fi modem sleep，改善弱訊號延遲。
- 改善 Web OTA 結果回報與延遲重啟。
- 完成弱 Wi-Fi 限速 OTA 實板驗證。

## 0.1.0-alpha.1 - 2026-08-13

- 第一個可運作版本。
- RX-only UART、RAM tail、LittleFS 循環日誌。
- Panic 關鍵字偵測與事件前後快照。
- Web 設定、日誌下載、Web OTA、mDNS 與 TCP 2323。
- Windows TCP 自動重連收集器。
