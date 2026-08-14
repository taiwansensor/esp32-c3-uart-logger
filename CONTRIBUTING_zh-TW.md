# 貢獻指南

歡迎提交 Issue、Pull Request、板型測試或文件修正。

## 回報問題時請提供

- ESP32-C3 板型與 Flash 容量。
- 韌體版本與 Arduino ESP32 Core 版本。
- UART Baud、GPIO 與目標設備類型。
- Web 狀態中的 RSSI、Heap、Error、Overflow、Queue drops。
- 可公開的相關 UART 片段；請先移除密碼、權杖、MAC 或設備識別資料。
- 重現步驟與預期結果。

## Pull Request

1. 從新分支進行修改。
2. 不要提交 Wi-Fi 密碼、測試日誌、captures 或編譯目錄。
3. 維持 RX-only 預設與 3.3 V 安全說明。
4. 修改行為時同步更新 README 或 `docs`。
5. 至少完成編譯；涉及 UART、檔案或 OTA 時應提供實板結果。
6. 中文文件使用 UTF-8。

## 程式風格

- 優先避免動態配置與長時間阻塞 UART 接收。
- 網路用戶端不得拖慢主要記錄流程。
- 新增緩衝時說明 RAM 影響。
- 新增 Panic 關鍵字時避免過度寬鬆造成誤判。
