```markdown
# Multi-Port Non-Blocking USB-PD State Machine Simulation (C Language)

以 C 語言實現之多接口並聯電源協定（USB-PD）狀態機控制系統，導入非阻塞調度與硬體邊界防禦機制。

## 核心設計亮點
1. **多埠記憶體隔離與非阻塞排程 (Non-Blocking Concurrency)**：
   - 使用 `struct` 封裝各埠獨立記憶體與狀態變數，以指標傳址調度，消除全域變數衝突。
   - 採用狀態輪詢搭配計時器，取代阻塞式死等（`while` loop），確保單埠穩壓延遲不阻礙系統主核心執行。
2. **硬體邊界保護與編譯期防禦**：
   - 導入 `_Static_assert` 於編譯階段攔截超出硬體物理通道上限之錯誤配置。
   - 狀態機內建電壓爬坡監控與 Timeout 防呆機制，於電壓下陷（Sag）或異常時即時觸發 `Fast Abort`。
3. **安全斷閘與主動放電 (Fail-Safe Discharge)**：
   - 電壓切換前強制清零供電位元（`BIT_VBUS_EN`）以防過衝（Overshoot）。
   - 異常隔離狀態下強制開啟放電迴路（`BIT_DISCH_EN`），保護待測晶片與治具。

## 執行與驗證
```bash
gcc -Wall -Wextra -O2 pd_statemachine.c -o pd_statemachine
./pd_statemachine
