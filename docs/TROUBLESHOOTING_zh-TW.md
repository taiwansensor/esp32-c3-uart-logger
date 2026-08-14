# 疑難排解

## 找不到設定 AP

- 確認 C3 已正常供電。
- 等待至少 15 秒；已設定但連線失敗時，必須等 STA timeout 後才建立 AP。
- 暫時斷開電腦或手機原本的 Wi-Fi，再重新掃描。
- 從 USB Serial 115200 查看是否顯示 `web http://192.168.4.1/`。

## 設定後仍回到 192.168.4.1

- 確認 SSID 是 2.4 GHz，不是 5 GHz。
- 重新輸入密碼；SSID 與密碼區分大小寫。
- 把 Logger 暫時移近 AP。
- 某些企業 Wi-Fi、Captive Portal 或 WPA3-only 網路不能直接使用。

## `uart-logger.local` 無法開啟

mDNS 在部分 Windows 或 VLAN 環境可能無法跨網段。請從路由器 DHCP 清單查詢 IP，或從 USB Serial 讀取啟動訊息。

## Captured bytes 一直是 0

- 確認目標 TX 接到 Logger RX，而不是 RX 接 RX。
- 確認兩板 GND 共地。
- 確認 Baud 與資料格式。
- 確認目標韌體真的有啟用 UART logger。
- 使用示波器或邏輯分析儀確認 TX 有訊號。

## UART Error 或 Overflow 增加

- Error 但沒有 Overflow：檢查 Baud、電壓、雜訊與接地。
- Overflow：主流程曾來不及消化 UART buffer。
- 查看 `last uart overflow` 與 `uart queue drops`。
- 降低 Web 使用頻率、改善 Wi-Fi、避免同時下載大檔。
- 仍持續增加時，可依修改教學加大 buffer，但需保留 Heap。

## OTA 顯示 Empty reply

先檢查版本與 uptime，不能只憑 HTTP 空回應判定成功。若版本沒有更新：

1. 從 Web 正常重新啟動 Logger，清除中斷的 Update session。
2. 改用有線距離較近的 AP，或把 C3 移近 AP。
3. 使用 `curl --header 'Expect:' --limit-rate 80k` 限速重試。
4. 仍失敗時改用 USB Serial 燒錄。

## Panic 沒有建立快照

- 目標可能沒有從 UART 輸出 Panic 文字。
- Panic 行可能沒有包含目前的關鍵字。
- UART 在當機前已停止或接線中斷。
- 查看一般 `uart-*.log` 是否只有輸出突然停止。

## TCP 收集檔出現重複內容

這是預期行為。TCP 用戶端重新連線時，Logger 會重播最近最多 4 KiB，避免短暫斷線完全漏掉事件，因此相鄰工作階段可能重複。
