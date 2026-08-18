# 二次開發與修改教學

主要程式位於 `src/main.cpp`。修改前先保留一份可工作的韌體與設定，並在測試板驗證後再接到正式設備。

## 修改產品名稱與版本

搜尋：

```cpp
constexpr char kVersion[] = "0.1.0-alpha.3";
constexpr char kDefaultHostname[] = "uart-logger";
```

修改版本後，Web 狀態頁與日誌 Session header 會一起更新。Hostname 建議只使用英文字母、數字與連字號。

## 修改預設 UART

```cpp
constexpr uint32_t kDefaultBaud = 115200;
constexpr int kDefaultRxPin = 4;
```

支援的常見 Baud 包括 `9600`、`19200`、`38400`、`57600`、`115200`。Web 介面也能在不重新編譯的情況下修改。

如果要改成非 `8N1`，請調整：

```cpp
target_uart.begin(config.baud, SERIAL_8N1, config.rx_pin, -1);
```

最後一個 `-1` 表示不配置 TX，請保留以維持 RX-only。

## 修改緩衝容量

```cpp
constexpr size_t kHardwareRxBufferSize = 16 * 1024;
constexpr size_t kUartStreamSize = 32 * 1024;
constexpr size_t kRamRingSize = 24 * 1024;
```

- `kHardwareRxBufferSize`：UART driver 收到後、工作任務取走前的緩衝。
- `kUartStreamSize`：高優先 UART 任務與主流程之間的佇列。
- `kRamRingSize`：Web 尾端畫面與 Panic 事件前置資料。

加大緩衝會降低 Overflow 機率，但也會降低 Free Heap。修改後應在 Web 狀態確認 Heap，並以高流量 UART、弱 Wi-Fi、日誌下載與 OTA 同時測試。

## 修改日誌容量

```cpp
constexpr size_t kMainSegmentLimit = 384 * 1024;
constexpr size_t kPanicFileLimit = 128 * 1024;
constexpr size_t kWifiSegmentLimit = 64 * 1024;
constexpr uint32_t kWifiHeartbeatMs = 5 * 60 * 1000;
```

目前共有兩個一般日誌、三個 Panic 檔案及兩個 Wi-Fi 事件檔案。`kWifiHeartbeatMs` 控制 AP 狀態取樣週期；連線與斷線事件不受此週期限制，會立即寫入。所有上限加總後必須為 LittleFS 保留檔案系統管理空間，不能直接用滿分割區。

## 新增 Panic 關鍵字

找到 `line_has_panic_marker()`：

```cpp
static const char *markers[] = {
    "guru meditation error",
    "backtrace:",
    "assert failed"
};
```

比對前會轉成小寫，因此新增字串也應使用小寫。關鍵字太短或太常見會產生誤觸發。

事件後擷取時間由下列常數控制：

```cpp
constexpr uint32_t kPanicCaptureMs = 90 * 1000;
```

## 修改 Web 頁面

首頁 HTML、CSS 與 JavaScript 位於 `kRootHtml`。為減少 Flash 與 RAM 使用，目前直接放在 PROGMEM。

狀態資料由 `handle_status()` 產生 JSON。新增欄位時必須：

1. 對字串使用 `json_escape()`。
2. 注意最後一個欄位的逗號。
3. 避免在每次請求建立過大的 `String`。
4. 在弱 Wi-Fi 下測試頁面是否會拖慢 UART。

## 修改 Web 輪詢頻率

首頁目前設定：

- UART tail：每 1000 ms。
- 狀態：每 10000 ms。

若 Wi-Fi 很弱，可改成 2000 ms 與 30000 ms。頻率太高只會增加網路重送與主循環負擔。

## 修改 TCP Port 或用戶端數量

```cpp
constexpr uint16_t kDefaultTcpPort = 2323;
constexpr size_t kTcpClientCount = 3;
```

TCP 設計為唯讀。請不要加入遠端 UART TX，除非你完全理解目標設備的控制與安全風險。

## 移植到其他 ESP32

移植到 ESP32、S3、C6 時至少檢查：

- HardwareSerial 編號與可用 GPIO。
- USB CDC 設定。
- Flash 容量與 Partition scheme。
- RAM 是否足以容納三層緩衝。
- Wi-Fi API 與 Arduino Core 版本。

先以 RX 未接線的狀態測試 Web、LittleFS 與 OTA，再接入目標 UART。

## 建議驗證流程

1. 編譯並確認 Flash／RAM 沒有超出上限。
2. 首次啟動完成 AP 與 Wi-Fi 設定。
3. 餵入固定 UART 測試資料，確認檔案與 Web 相同。
4. 以連續 115200 流量測試 Overflow 與 Queue drops。
5. 人工送出 Panic 關鍵字，確認事件前後快照。
6. 測試 Wi-Fi 中斷、TCP 重連與弱訊號。
7. 測試 Web OTA 成功與中途中斷復原。
8. 至少執行 24～72 小時長測。
